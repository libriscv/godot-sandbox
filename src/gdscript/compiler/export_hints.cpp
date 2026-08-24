#include "export_hints.h"
#include "globals.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace gdscript {

namespace {

constexpr int32_t USAGE_STORAGE_ONLY = 2 | 4096;

enum class ArgStyle : uint8_t {
	NONE,
	STRINGS,
	ONE_STRING,
	RANGE,
	CUSTOM,
};

struct ExportAnnotation {
	const char* name;
	int32_t hint;
	ArgStyle style;
	size_t min_args;
	size_t max_args;
	int32_t usage;
};

constexpr size_t ANY = size_t(-1);
const ExportAnnotation EXPORT_ANNOTATIONS[] = {
	{ "export", 0, ArgStyle::NONE, 0, 0, 0 },
	{ "export_range", 1, ArgStyle::RANGE, 2, ANY, 0 },
	{ "export_enum", 2, ArgStyle::STRINGS, 1, ANY, 0 },
	{ "export_exp_easing", 4, ArgStyle::STRINGS, 0, ANY, 0 },
	{ "export_flags", 6, ArgStyle::STRINGS, 1, ANY, 0 },
	{ "export_flags_2d_render", 7, ArgStyle::NONE, 0, 0, 0 },
	{ "export_flags_2d_physics", 8, ArgStyle::NONE, 0, 0, 0 },
	{ "export_flags_2d_navigation", 9, ArgStyle::NONE, 0, 0, 0 },
	{ "export_flags_3d_render", 10, ArgStyle::NONE, 0, 0, 0 },
	{ "export_flags_3d_physics", 11, ArgStyle::NONE, 0, 0, 0 },
	{ "export_flags_3d_navigation", 12, ArgStyle::NONE, 0, 0, 0 },
	{ "export_flags_avoidance", 37, ArgStyle::NONE, 0, 0, 0 },
	{ "export_file", 13, ArgStyle::STRINGS, 0, ANY, 0 },
	{ "export_dir", 14, ArgStyle::NONE, 0, 0, 0 },
	{ "export_global_file", 15, ArgStyle::STRINGS, 0, ANY, 0 },
	{ "export_global_dir", 16, ArgStyle::NONE, 0, 0, 0 },
	{ "export_multiline", 18, ArgStyle::NONE, 0, 0, 0 },
	{ "export_placeholder", 20, ArgStyle::ONE_STRING, 1, 1, 0 },
	{ "export_color_no_alpha", 21, ArgStyle::NONE, 0, 0, 0 },
	{ "export_node_path", 26, ArgStyle::STRINGS, 0, ANY, 0 },
	{ "export_storage", 0, ArgStyle::NONE, 0, 0, USAGE_STORAGE_ONLY },
	{ "export_custom", 0, ArgStyle::CUSTOM, 2, 3, 0 },
};

const ExportAnnotation* find_annotation(const std::string& name) {
	for (const ExportAnnotation& entry : EXPORT_ANNOTATIONS) {
		if (name == entry.name) {
			return &entry;
		}
	}
	return nullptr;
}

std::string arity_text(const ExportAnnotation& entry) {
	if (entry.max_args == ANY) {
		return "at least " + std::to_string(entry.min_args);
	}
	if (entry.min_args == entry.max_args) {
		return std::to_string(entry.min_args);
	}
	return std::to_string(entry.min_args) + " to " + std::to_string(entry.max_args);
}

bool integer_argument(const ExportArgument& arg, int64_t& out) {
	if (arg.kind == ExportArgument::Kind::NUMBER) {
		out = int64_t(arg.number);
		return true;
	}
	if (arg.kind != ExportArgument::Kind::NAME) {
		return false;
	}
	const GlobalConstant* constant = find_global_constant(arg.text);
	if (constant == nullptr || constant->is_float) {
		return false;
	}
	out = constant->int_value;
	return true;
}

bool number_argument(const ExportArgument& arg, double& out) {
	if (arg.kind == ExportArgument::Kind::NUMBER) {
		out = arg.number;
		return true;
	}
	if (arg.kind != ExportArgument::Kind::NAME) {
		return false;
	}
	const GlobalConstant* constant = find_global_constant(arg.text);
	if (constant == nullptr) {
		return false;
	}
	out = constant->is_float ? constant->float_value : double(constant->int_value);
	return true;
}

bool unevaluated(const ExportArgument& arg) {
	if (arg.kind == ExportArgument::Kind::OTHER) {
		return true;
	}
	return arg.kind == ExportArgument::Kind::NAME &&
		find_global_constant(arg.text) == nullptr;
}

} // namespace

std::string format_hint_number(double value) {
	if (std::isnan(value)) {
		return "nan";
	}
	if (std::isinf(value)) {
		return value < 0 ? "-inf" : "inf";
	}

	int decimals = 14;
	const double magnitude = std::fabs(value);
	if (magnitude > 10.0) {
		decimals -= int(std::floor(std::log10(magnitude)));
	}
	if (decimals < 0) {
		decimals = 0;
	}

	char buffer[512];
	std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
	std::string text(buffer);

	const size_t dot = text.find('.');
	if (dot == std::string::npos) {
		return text + ".0";
	}
	size_t last = text.find_last_not_of('0');
	if (last == dot) {
		last += 1;
	}
	text.erase(last + 1);
	return text;
}

bool build_export_hint(const std::string& name, const std::vector<ExportArgument>& args,
	ExportHint& out, std::string& error)
{
	const ExportAnnotation* entry = find_annotation(name);
	if (entry == nullptr) {
		return false;
	}

	out = ExportHint{};
	out.hint = entry->hint;
	out.usage = entry->usage;

	if (args.size() < entry->min_args || (entry->max_args != ANY && args.size() > entry->max_args)) {
		error = "@" + name + " takes " + arity_text(*entry) + " argument" +
			(entry->min_args == 1 && entry->max_args == 1 ? "" : "s") + ", got " +
			std::to_string(args.size());
		return true;
	}

	switch (entry->style) {
		case ArgStyle::NONE:
			return true;

		case ArgStyle::ONE_STRING:
		case ArgStyle::STRINGS: {
			for (const ExportArgument& arg : args) {
				if (unevaluated(arg)) {
					out = ExportHint{};
					return true;
				}
				if (arg.kind != ExportArgument::Kind::STRING) {
					error = "@" + name + " takes strings; this argument is not one";
					return true;
				}
				if (!out.hint_string.empty()) {
					out.hint_string += ',';
				}
				out.hint_string += arg.text;
			}
			return true;
		}

		case ArgStyle::RANGE: {
			for (size_t i = 0; i < args.size(); i++) {
				const ExportArgument& arg = args[i];
				if (unevaluated(arg)) {
					out = ExportHint{};
					return true;
				}
				if (!out.hint_string.empty()) {
					out.hint_string += ',';
				}
				if (i < 3) {
					double number = 0.0;
					if (!number_argument(arg, number)) {
						error = "@" + name + " argument " + std::to_string(i + 1) +
							" is a number in the engine";
						return true;
					}
					out.hint_string += format_hint_number(number);
					continue;
				}
				if (arg.kind != ExportArgument::Kind::STRING) {
					error = "@" + name + " argument " + std::to_string(i + 1) +
						" is a string in the engine";
					return true;
				}
				out.hint_string += arg.text;
			}
			return true;
		}

		case ArgStyle::CUSTOM: {
			for (const ExportArgument& arg : args) {
				if (unevaluated(arg)) {
					out = ExportHint{};
					return true;
				}
			}
			int64_t hint = 0;
			if (!integer_argument(args[0], hint)) {
				error = "@" + name + " takes a PROPERTY_HINT_* constant first";
				return true;
			}
			if (args[1].kind != ExportArgument::Kind::STRING) {
				error = "@" + name + " takes the hint string second";
				return true;
			}
			out.hint = int32_t(hint);
			out.hint_string = args[1].text;
			if (args.size() == 3) {
				int64_t usage = 0;
				if (!integer_argument(args[2], usage)) {
					error = "@" + name + " takes a PROPERTY_USAGE_* constant third";
					return true;
				}
				out.usage = int32_t(usage);
			}
			return true;
		}
	}
	return true;
}

} // namespace gdscript
