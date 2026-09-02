#pragma once
#include <string>
#include <utility>
#include <vector>

namespace gdscript {

struct ParseDiagnostic {
	std::string code;
	std::string message;
	int line = 0;
	int column = 0;
	int end_line = 0;
	int end_column = 0;
};

struct DiagnosticSink {
	std::vector<ParseDiagnostic> diagnostics;
	size_t limit = 100;

	void add(std::string code, std::string message, int line, int column,
		int end_line, int end_column) {
		if (diagnostics.size() >= limit) {
			return;
		}
		if (line < 1) line = 1;
		if (column < 1) column = 1;
		if (end_line < line) end_line = line;
		if (end_line == line && end_column <= column) end_column = column + 1;
		diagnostics.push_back({std::move(code), std::move(message), line, column,
			end_line, end_column});
	}
	bool full() const { return diagnostics.size() >= limit; }
};

} // namespace gdscript
