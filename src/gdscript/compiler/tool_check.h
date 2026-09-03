#pragma once
#include "compiler.h"
#include "source_model.h"
#include <algorithm>
#include <iostream>
#include <utility>
#include <vector>
#include <string>

namespace gdscript {

inline int check_source(const std::string& source, const std::string& path,
	const CompilerOptions& options)
{
	const std::string where = path.empty() ? std::string("<stdin>") : path;
	size_t errors = 0;
	size_t warnings = 0;

	const SourceModel model = analyze_source(source, where,
		ANALYZE_DIAGNOSTICS | ANALYZE_DECLARATIONS);
	std::vector<int> reported_lines;
	for (const SourceDiagnostic& diagnostic : model.diagnostics) {
		const bool is_error = diagnostic.severity == DiagnosticSeverity::ERROR;
		if (is_error) {
			errors++;
			reported_lines.push_back(int(diagnostic.range.start_line));
		} else {
			warnings++;
		}
		std::cerr << where << ":" << diagnostic.range.start_line << ":"
			<< diagnostic.range.start_column << ": "
			<< (is_error ? "error" : "warning") << ": " << diagnostic.message
			<< " [" << diagnostic.code << "]" << std::endl;
	}

	Compiler compiler;
	CompilerOptions check = options;
	check.output_elf = false;
	compiler.compile(source, check);
	const CompilerError& failure = compiler.get_error_info();
	const bool duplicate = std::find(reported_lines.begin(), reported_lines.end(),
		failure.line) != reported_lines.end();
	if (failure.has_error && !duplicate) {
		errors++;
		std::cerr << where << ":" << failure.line << ":" << failure.column
			<< ": error: " << failure.message << std::endl;
		if (!failure.hint.empty()) {
			std::cerr << "  hint: " << failure.hint << std::endl;
		}
	}

	if (errors == 0) {
		std::cout << where << ": ok"
			<< (warnings != 0 ? ", " + std::to_string(warnings) + " warning(s)" : "")
			<< std::endl;
	}
	return errors == 0 ? 0 : 1;
}

} // namespace gdscript
