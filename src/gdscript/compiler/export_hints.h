#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gdscript {

struct ExportHint {
	int32_t hint = 0;
	std::string hint_string;
	int32_t usage = 0;

	bool is_default() const { return hint == 0 && hint_string.empty() && usage == 0; }
};

struct ExportArgument {
	enum class Kind : uint8_t {
		NUMBER,
		STRING,
		NAME,
		OTHER,
	};

	Kind kind = Kind::NUMBER;
	double number = 0.0;
	std::string text;
	int line = 0;
	int column = 0;
};

bool build_export_hint(const std::string& name, const std::vector<ExportArgument>& args,
	ExportHint& out, std::string& error);

std::string format_hint_number(double value);

} // namespace gdscript
