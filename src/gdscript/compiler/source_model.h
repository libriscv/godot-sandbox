#pragma once

#include "property_signature.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gdscript {

struct SourceRange {
	uint32_t start_line = 0;
	uint32_t start_column = 0;
	uint32_t end_line = 0;
	uint32_t end_column = 0;
};

enum class DiagnosticSeverity : uint8_t { ERROR, WARNING, INFO };
enum class DeclarationKind : uint8_t {
	CLASS, FUNCTION, PARAMETER, VARIABLE, CONSTANT, SIGNAL, ENUM, ENUM_VALUE,
	NESTED_CLASS, ANNOTATION, TRAIT, STRUCT, COUNT,
};

enum DeclarationFlags : uint32_t {
	DECLARATION_STATIC = 1u << 0,
	DECLARATION_EXPORT = 1u << 1,
	DECLARATION_ONREADY = 1u << 2,
	DECLARATION_ABSTRACT = 1u << 3,
};
enum class CaretKind : uint8_t {
	NONE, IDENTIFIER, MEMBER, TYPE, CALL_ARGUMENT, ANNOTATION, RESOURCE_PATH,
	NODE_PATH, STRING_NAME,
};

struct SourceDiagnostic {
	DiagnosticSeverity severity = DiagnosticSeverity::ERROR;
	std::string code;
	std::string message;
	std::string path;
	SourceRange range;
};

struct SourceParameter {
	std::string name;
	std::string declared_type;
	std::string default_text;
	SourceRange declaration;
};

struct SourceEnumMember {
	std::string name;
	int64_t value = 0;
	SourceRange declaration;
};

struct SourceDeclaration {
	DeclarationKind kind = DeclarationKind::VARIABLE;
	std::string name;
	std::string declared_type;
	std::string resolved_type;
	SourceRange declaration;
	SourceRange lexical_scope;
	int32_t parent = -1;
	uint32_t flags = 0;
	std::vector<int32_t> children;
	std::string documentation;
	std::vector<SourceParameter> parameters;
	std::string return_type;
	std::vector<SourceEnumMember> enum_members;
	std::vector<std::string> annotation_arguments;
	// For file-level CLASS, the file's own extends.
	std::string base_type;
	std::string initializer_text;
	// Inline accessor bodies carry a synthesized name.
	std::string setter;
	std::string getter;
};

struct CaretContext {
	CaretKind kind = CaretKind::NONE;
	int32_t declaration = -1;
	std::string receiver_text;
	std::string receiver_type;
	std::string callee;
	int32_t argument_index = -1;
};

struct SourceModel {
	std::string path;
	std::vector<SourceDiagnostic> diagnostics;
	std::vector<SourceDeclaration> declarations;
	std::vector<PropertySignature> properties;
	CaretContext caret;
	std::vector<uint32_t> safe_lines;
};

enum AnalysisFlags : uint32_t {
	ANALYZE_DIAGNOSTICS = 1u << 0,
	ANALYZE_DECLARATIONS = 1u << 1,
	ANALYZE_CARET = 1u << 2,
	ANALYZE_DOCUMENTATION = 1u << 3,
	ANALYZE_SAFE_LINES = 1u << 4,
	ANALYZE_ALL = 0x1fu,
};

std::vector<uint8_t> encode_source_model(const SourceModel &model);
bool decode_source_model(const uint8_t *data, size_t size, SourceModel &out);
bool decode_source_model(const uint8_t *data, size_t size, SourceModel &out,
		std::string &error);

// Error-tolerant, non-codegen editor analysis. Normal Compiler::compile remains
// fail-fast and is never called in a recovery mode.
SourceModel analyze_source(const std::string &source, const std::string &path,
		uint32_t flags = ANALYZE_ALL, int32_t caret_line = 0,
		int32_t caret_column = 0);

} // namespace gdscript
