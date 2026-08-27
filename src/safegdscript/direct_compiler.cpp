#include "compiler_backend.h"

#ifdef SAFEGDSCRIPT_DIRECT_COMPILER

#include "../gdscript/compiler/compiler.h"
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

namespace {

std::string to_utf8(const String &p_text) {
	const CharString utf8 = p_text.utf8();
	return std::string(utf8.get_data(), size_t(utf8.length()));
}

String from_utf8(const std::string &p_text) {
	return String::utf8(p_text.c_str(), int64_t(p_text.size()));
}

std::vector<std::string> to_utf8_list(const PackedStringArray &p_strings) {
	std::vector<std::string> out;
	out.reserve(size_t(p_strings.size()));
	for (int64_t i = 0; i < p_strings.size(); i++) {
		out.push_back(to_utf8(p_strings[i]));
	}
	return out;
}

PackedByteArray to_packed(const std::vector<uint8_t> &p_bytes) {
	PackedByteArray out;
	if (p_bytes.empty()) {
		return out;
	}
	out.resize(int64_t(p_bytes.size()));
	memcpy(out.ptrw(), p_bytes.data(), p_bytes.size());
	return out;
}

class DirectCompilerBackend final : public GDScriptCompilerBackend {
public:
	const char *name() const override { return "direct"; }
	bool available() override { return true; }

	void set_restricted(bool p_restricted) override { m_options.restricted = p_restricted; }

	void set_autoloads(const PackedStringArray &p_names) override {
		m_options.autoloads = to_utf8_list(p_names);
	}

	void set_global_classes(const PackedStringArray &p_pairs) override {
		m_options.global_script_classes.clear();
		for (int64_t i = 0; i + 1 < p_pairs.size(); i += 2) {
			m_options.global_script_classes.emplace_back(to_utf8(p_pairs[i]), to_utf8(p_pairs[i + 1]));
		}
	}

	void set_base_sources(const PackedStringArray &p_triples) override {
		m_options.base_sources.clear();
		for (int64_t i = 0; i + 2 < p_triples.size(); i += 3) {
			m_options.base_sources.push_back(gdscript::CompilerOptions::BaseSource{
					to_utf8(p_triples[i]), to_utf8(p_triples[i + 1]), to_utf8(p_triples[i + 2]) });
		}
	}

	bool can_build_profiled() override { return true; }
	bool can_build_debug() override { return true; }

	PackedByteArray compile(const String &p_source, const BuildOptions &p_options) override {
		gdscript::CompilerOptions options = m_options;
		options.output_elf = true;
		options.profiling = p_options.profiling;
		options.profiling_clock = gdscript::ProfilingClock::TIME;
		if (p_options.debug) {
			options.debug_info = true;
			for (int64_t i = 0; i < p_options.breakpoints.size(); i++) {
				const int32_t line = p_options.breakpoints[i];
				if (line > 0) {
					options.breakpoint_lines.push_back(uint32_t(line));
				}
			}
		}

		gdscript::Compiler compiler;
		const std::vector<uint8_t> elf_data = compiler.compile(to_utf8(p_source), options);
		remember(compiler);

		if (elf_data.empty()) {
			UtilityFunctions::print("ERROR: Compilation failed: ", m_error);
			return PackedByteArray();
		}
		return to_packed(elf_data);
	}

	bool validate(const String &p_source, Validation &r_result) override {
		gdscript::CompilerOptions options = m_options;
		options.output_elf = false;

		gdscript::Compiler compiler;
		compiler.compile(to_utf8(p_source), options);
		const gdscript::CompilerError &error = compiler.get_error_info();

		r_result = Validation();
		r_result.valid = !error.has_error;
		if (!r_result.valid) {
			r_result.line = error.line;
			r_result.column = error.column;
			r_result.message = from_utf8(error.message);
			if (!error.hint.empty()) {
				r_result.message += String(" (") + from_utf8(error.hint) + String(")");
			}
			if (r_result.message.is_empty()) {
				r_result.message = "Compilation failed";
			}
		}
		return true;
	}

	String error_message() override {
		return m_error.is_empty() ? String("compilation failed") : m_error;
	}

	std::vector<gdscript::FunctionSignature> function_signatures() override { return m_signatures; }
	std::vector<gdscript::FunctionSignature> signal_signatures() override { return m_signals; }
	std::vector<gdscript::ClassSignature> class_signatures() override { return m_classes; }
	gdscript::LineTable line_table() override { return m_line_table; }
	PackedInt32Array installed_breakpoints() override { return m_breakpoints; }
	bool is_tool() override { return m_is_tool; }
	ScriptClass script_class() override { return m_script_class; }

private:
	void remember(const gdscript::Compiler &p_compiler) {
		m_error = from_utf8(p_compiler.get_error());
		m_signatures = p_compiler.get_function_signatures();
		m_signals = p_compiler.get_signal_signatures();
		m_classes = p_compiler.get_class_signatures();
		m_line_table = p_compiler.get_line_table();
		m_is_tool = p_compiler.is_tool();

		m_breakpoints.clear();
		for (const uint32_t line : p_compiler.get_installed_breakpoints()) {
			m_breakpoints.push_back(int32_t(line));
		}

		m_script_class = ScriptClass();
		m_script_class.class_name = from_utf8(p_compiler.get_class_name());
		m_script_class.base_class = from_utf8(p_compiler.get_base_class());
		m_script_class.base_is_path = p_compiler.base_is_path();
		m_script_class.native_base_class = from_utf8(p_compiler.get_native_base_class());
		m_script_class.native_base_is_path = p_compiler.native_base_is_path();
	}

	gdscript::CompilerOptions m_options;

	String m_error;
	std::vector<gdscript::FunctionSignature> m_signatures;
	std::vector<gdscript::FunctionSignature> m_signals;
	std::vector<gdscript::ClassSignature> m_classes;
	gdscript::LineTable m_line_table;
	PackedInt32Array m_breakpoints;
	bool m_is_tool = false;
	ScriptClass m_script_class;
};

} // namespace

GDScriptCompilerBackend *direct_compiler_backend() {
	static DirectCompilerBackend *backend = new DirectCompilerBackend();
	return backend;
}

#else

GDScriptCompilerBackend *direct_compiler_backend() {
	return nullptr;
}

#endif // SAFEGDSCRIPT_DIRECT_COMPILER
