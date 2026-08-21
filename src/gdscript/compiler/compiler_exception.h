#pragma once

#include <string>
#include <optional>
#include <exception>
#include <sstream>

namespace gdscript {

enum class ErrorType {
	LEXER_ERROR,
	PARSER_ERROR,
	SEMANTIC_ERROR,
	CODEGEN_ERROR,
	RISCV_codegen_ERROR,
	OPTIMIZER_ERROR,
	IR_VERIFIER_ERROR,
	ELF_ERROR,
	UNKNOWN_ERROR
};

const char* error_type_to_string(ErrorType type);

class CompilerException : public std::exception {
private:
	ErrorType m_error_type;
	std::string m_message;
	std::string m_file;
	int m_line = 0;
	int m_column = 0;
	std::string m_function;
	std::string m_source_line;
	std::string m_hint;
	mutable std::string m_what;

public:
	CompilerException(
		ErrorType error_type,
		const std::string& message,
		int line = 0,
		int column = 0,
		const std::string& function = "",
		const std::string& file = "",
		const std::string& source_line = "",
		const std::string& hint = ""
	)
		: m_error_type(error_type)
		, m_message(message)
		, m_file(file)
		, m_line(line)
		, m_column(column)
		, m_function(function)
		, m_source_line(source_line)
		, m_hint(hint)
	{}

	ErrorType error_type() const { return m_error_type; }
	const char* error_type_string() const { return error_type_to_string(m_error_type); }

	const char* what() const noexcept override {
		if (m_what.empty()) {
			m_what = format_message();
		}
		return m_what.c_str();
	}

	const std::string& message() const { return m_message; }
	const std::string& file() const { return m_file; }
	int line() const { return m_line; }
	int column() const { return m_column; }
	const std::string& function() const { return m_function; }
	const std::string& source_line() const { return m_source_line; }
	const std::string& hint() const { return m_hint; }

	void set_function(const std::string& function) { m_function = function; m_what.clear(); }
	void set_file(const std::string& file) { m_file = file; m_what.clear(); }
	void set_source_line(const std::string& source_line) { m_source_line = source_line; m_what.clear(); }
	void set_hint(const std::string& hint) { m_hint = hint; m_what.clear(); }

	std::string format_message() const {
		std::ostringstream oss;

		oss << "[" << error_type_string() << "] " << m_message;

		if (m_line > 0) {
			oss << " (line " << m_line;
			if (m_column > 0) {
				oss << ", column " << m_column;
			}
			oss << ")";
		}

		if (!m_function.empty()) {
			oss << "\n  in function: " << m_function;
		}

		if (!m_file.empty()) {
			oss << "\n  in file: " << m_file;
		}

		if (!m_source_line.empty()) {
			oss << "\n\n  " << m_source_line;
			if (m_column > 0) {
				oss << "\n  ";
				for (int i = 1; i < m_column; ++i) {
					if (static_cast<size_t>(i - 1) < m_source_line.length() && m_source_line[i - 1] == '\t') {
						oss << "    ";
					} else {
						oss << " ";
					}
				}
				oss << "^";
			}
		}

		if (!m_hint.empty()) {
			oss << "\n\n  Hint: " << m_hint;
		}

		return oss.str();
	}

	std::string to_string() const {
		std::ostringstream oss;
		oss << "[" << error_type_string() << "] " << m_message;

		if (m_line > 0) {
			oss << " at line " << m_line;
			if (m_column > 0) {
				oss << ":" << m_column;
			}
		}

		if (!m_function.empty()) {
			oss << " in '" << m_function << "'";
		}

		return oss.str();
	}

	static CompilerException lexer_error(const std::string& message, int line, int column) {
		return CompilerException(ErrorType::LEXER_ERROR, message, line, column);
	}

	static CompilerException parser_error(const std::string& message, int line, int column) {
		return CompilerException(ErrorType::PARSER_ERROR, message, line, column);
	}

	static CompilerException semantic_error(const std::string& message, int line, int column, const std::string& function = "") {
		return CompilerException(ErrorType::SEMANTIC_ERROR, message, line, column, function);
	}

	static CompilerException codegen_error(const std::string& message, const std::string& function = "") {
		return CompilerException(ErrorType::CODEGEN_ERROR, message, 0, 0, function);
	}

	static CompilerException riscv_codegen_error(const std::string& message, const std::string& function = "") {
		return CompilerException(ErrorType::RISCV_codegen_ERROR, message, 0, 0, function);
	}

	static CompilerException undefined_variable(const std::string& var_name, int line, int column, const std::string& function = "") {
		return CompilerException(
			ErrorType::SEMANTIC_ERROR,
			"Undefined variable: " + var_name,
			line,
			column,
			function,
			"",
			"",
			"Make sure '" + var_name + "' is declared before use"
		);
	}

	static CompilerException type_error(const std::string& message, int line, int column, const std::string& function = "") {
		return CompilerException(
			ErrorType::SEMANTIC_ERROR,
			"Type error: " + message,
			line,
			column,
			function
		);
	}

	static CompilerException syntax_error(const std::string& message, int line, int column) {
		return CompilerException(
			ErrorType::PARSER_ERROR,
			"Syntax error: " + message,
			line,
			column
		);
	}
};

#define THROW_COMPILER_ERROR(error_type, message) \
	throw gdscript::CompilerException((error_type), (message), __LINE__, 0, __func__, __FILE__)

} // namespace gdscript
