#include "compiler_exception.h"

namespace gdscript {

const char* error_type_to_string(ErrorType type) {
	switch (type) {
		case ErrorType::LEXER_ERROR:         return "Lexer Error";
		case ErrorType::PARSER_ERROR:        return "Parser Error";
		case ErrorType::SEMANTIC_ERROR:      return "Semantic Error";
		case ErrorType::CODEGEN_ERROR:       return "Code Generation Error";
		case ErrorType::RISCV_codegen_ERROR: return "RISC-V Code Generation Error";
		case ErrorType::OPTIMIZER_ERROR:     return "Optimizer Error";
		case ErrorType::ELF_ERROR:           return "ELF Error";
		case ErrorType::UNKNOWN_ERROR:       return "Unknown Error";
		default:                             return "Unknown Error";
	}
}

} // namespace gdscript
