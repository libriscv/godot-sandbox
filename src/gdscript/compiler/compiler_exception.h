#pragma once

#include <string>
#include <optional>
#include <exception>
#include <sstream>

namespace gdscript {

/**
 * Categories of compilation errors.
 * Helps users understand what phase of compilation failed.
 */
enum class ErrorType {
	LEXER_ERROR,          // Tokenization errors
	PARSER_ERROR,         // Syntax errors
	SEMANTIC_ERROR,       // Type checking, variable validation, etc.
	CODEGEN_ERROR,        // IR generation errors
	RISCV_codegen_ERROR,  // RISC-V code generation errors
	OPTIMIZER_ERROR,      // IR optimization errors
	ELF_ERROR,            // ELF binary creation errors
	UNKNOWN_ERROR
};

/**
 * Converts ErrorType to a human-readable string.
 */
const char* error_type_to_string(ErrorType type);

/**
 * Rich exception class for GDScript compiler errors.
 *
 * Captures comprehensive context including:
 * - Error message
 * - Error type/category
 * - Source location (line, column)
 * - Function/method context
 * - Source code snippet (when available)
 * - Hints for fixing the error
 *
 * All information is preserved through the compilation pipeline
 * to provide end-users with actionable error messages.
 */
class CompilerException : public std::exception {
private:
	ErrorType m_error_type;
	std::string m_message;
	std::string m_file;          // Source file (if known)
	int m_line = 0;              // Line number (0 if unknown)
	int m_column = 0;            // Column number (0 if unknown)
	std::string m_function;      // Function name (empty if global scope)
	std::string m_source_line;   // The actual source line that caused the error
	std::string m_hint;          // Helpful hint for fixing the error
	mutable std::string m_what;  // Cached formatted message

public:
	/**
	 * Constructor with full context information.
	 *
	 * @param error_type  The category of error
	 * @param message     The primary error message
	 * @param line        Line number (optional, defaults to 0)
	 * @param column      Column number (optional, defaults to 0)
	 * @param function    Function name where error occurred (optional)
	 * @param file        Source file name (optional)
	 * @param source_line The actual source code line (optional)
	 * @param hint        Helpful hint for fixing the error (optional)
	 */
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

	// Getters

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

	/**
	 * Setters for adding context after construction.
	 * Useful when building exceptions gradually through the pipeline.
	 */
	void set_function(const std::string& function) { m_function = function; m_what.clear(); }
	void set_file(const std::string& file) { m_file = file; m_what.clear(); }
	void set_source_line(const std::string& source_line) { m_source_line = source_line; m_what.clear(); }
	void set_hint(const std::string& hint) { m_hint = hint; m_what.clear(); }

	/**
	 * Formats the complete error message with all available context.
	 * This is what gets returned by what().
	 */
	std::string format_message() const {
		std::ostringstream oss;

		// Error type and primary message
		oss << "[" << error_type_string() << "] " << m_message;

		// Location information
		if (m_line > 0) {
			oss << " (line " << m_line;
			if (m_column > 0) {
				oss << ", column " << m_column;
			}
			oss << ")";
		}

		// Function context
		if (!m_function.empty()) {
			oss << "\n  in function: " << m_function;
		}

		// File information
		if (!m_file.empty()) {
			oss << "\n  in file: " << m_file;
		}

		// Source line snippet
		if (!m_source_line.empty()) {
			oss << "\n\n  " << m_source_line;
			if (m_column > 0) {
				// Add a pointer to the exact column
				oss << "\n  ";
				for (int i = 1; i < m_column; ++i) {
					// Handle tabs (assume tab width of 4 for simplicity)
					if (static_cast<size_t>(i - 1) < m_source_line.length() && m_source_line[i - 1] == '\t') {
						oss << "    ";
					} else {
						oss << " ";
					}
				}
				oss << "^";
			}
		}

		// Helpful hint
		if (!m_hint.empty()) {
			oss << "\n\n  Hint: " << m_hint;
		}

		return oss.str();
	}

	/**
	 * Returns a compact one-line error message.
	 * Useful for logging or when you don't need the full formatted output.
	 */
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

	/**
	 * Helper factory methods for common error scenarios.
	 * These make it easier to throw exceptions with consistent formatting.
	 */

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

/**
 * Helper macro to throw CompilerException with current file and line.
 * Useful for quick errors during development.
 */
#define THROW_COMPILER_ERROR(error_type, message) \
	throw gdscript::CompilerException((error_type), (message), __LINE__, 0, __func__, __FILE__)

} // namespace gdscript
