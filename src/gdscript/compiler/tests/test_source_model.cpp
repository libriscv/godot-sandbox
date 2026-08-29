#include "../source_model.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

using namespace gdscript;

int main() {
	SourceModel model = analyze_source(
		"## Adds values\nfunc add(a: int, b: int = 2) -> int:\n\treturn a + b\nvar value: int = 3\n",
		"res://sample.sgd", ANALYZE_ALL, 3, 8);
	assert(model.declarations.size() >= 2);
	assert(model.declarations[0].documentation == "Adds values");
	assert(model.declarations[0].parameters.size() == 2);
	assert(model.declarations[0].return_type == "int");
	const std::vector<uint8_t> bytes = encode_source_model(model);
	assert(bytes == encode_source_model(model));
	SourceModel decoded;
	assert(decode_source_model(bytes.data(), bytes.size(), decoded));
	assert(decoded.path == model.path);
	assert(decoded.declarations.size() == model.declarations.size());
	for (size_t n = 0; n < bytes.size(); n++) assert(!decode_source_model(bytes.data(), n, decoded));
	std::vector<uint8_t> trailing = bytes; trailing.push_back(0);
	assert(!decode_source_model(trailing.data(), trailing.size(), decoded));

	std::vector<uint8_t> future = bytes;
	uint32_t count = 0; std::memcpy(&count, future.data() + 8, 4); count++;
	std::memcpy(future.data() + 8, &count, 4);
	const uint32_t id = 0xf00d, length = 3;
	const uint8_t *id_bytes = reinterpret_cast<const uint8_t *>(&id);
	const uint8_t *len_bytes = reinterpret_cast<const uint8_t *>(&length);
	future.insert(future.end(), id_bytes, id_bytes + 4);
	future.insert(future.end(), len_bytes, len_bytes + 4);
	future.insert(future.end(), {1, 2, 3});
	assert(decode_source_model(future.data(), future.size(), decoded));

	SourceModel broken = analyze_source(
		"func first(\nvar x =\nfunc later():\n\treturn 1\n", "res://broken.sgd");
	assert(broken.diagnostics.size() >= 2);
	bool later = false;
	for (const auto &decl : broken.declarations) later |= decl.name == "later";
	assert(later);
	// Statements, not lines: a bracket, a block string or a backslash carries a
	// statement across newlines, and a comment never opens a string.
	auto errors = [](const SourceModel &model) {
		size_t count = 0;
		for (const auto &d : model.diagnostics) count += d.severity == DiagnosticSeverity::ERROR;
		return count;
	};
	auto has_code = [](const SourceModel &model, const char *code) {
		for (const auto &d : model.diagnostics) if (d.code == code) return true;
		return false;
	};

	const SourceModel continued = analyze_source(
		"func setup():\n\tvar list = [\n\t\t1,\n\t\t2,\n\t]\n\tvar s = \"it's fine\" # don't worry\n\treturn list\n",
		"res://continued.sgd");
	assert(errors(continued) == 0);

	const SourceModel block = analyze_source(
		"func f():\n\tvar s = \"\"\"\nfunc not_really():\n\"\"\"\n\treturn s\n", "res://block.sgd");
	assert(errors(block) == 0);
	assert(block.declarations.size() == 2); // not_really() is string content

	const SourceModel joined = analyze_source(
		"func add(\n\t\ta: int,\n\t\tb: int = 2) -> int:\n\treturn a + b\n", "res://joined.sgd");
	assert(errors(joined) == 0);
	assert(joined.declarations[0].parameters.size() == 2);
	assert(joined.declarations[0].return_type == "int");

	// Statement keywords are statements, not discarded expressions.
	const SourceModel keywords = analyze_source(
		"func value() -> int:\n\treturn 1\nfunc f(n):\n\twhile n:\n\t\tbreak\n\t\tcontinue\n\tpass\n\treturn value()\n",
		"res://keywords.sgd");
	assert(!has_code(keywords, "STANDALONE_EXPRESSION"));
	assert(!has_code(keywords, "DISCARDED_RETURN_VALUE"));
	const SourceModel standalone = analyze_source("func f():\n\tn\n", "res://standalone.sgd");
	assert(has_code(standalone, "STANDALONE_EXPRESSION"));

	// An unclosed bracket is reported at the line that opened it, and the next
	// declaration resumes analysis instead of the error swallowing the file.
	const SourceModel unclosed = analyze_source(
		"func first(\nfunc later():\n\treturn 1\n", "res://unclosed.sgd");
	assert(has_code(unclosed, "EXPECTED_CLOSING_DELIMITER"));
	assert(unclosed.diagnostics[0].range.start_line == 1);

	const SourceModel traits = analyze_source(
		"## Can be damaged\ntrait Damageable:\n\tfunc damage(amount: int) -> bool\n",
		"res://traits.sgd");
	assert(errors(traits) == 0);
	assert(traits.declarations.size() >= 3); // trait, method, parameter
	assert(traits.declarations[0].kind == DeclarationKind::TRAIT);
	assert(traits.declarations[0].name == "Damageable");
	assert(traits.declarations[0].documentation == "Can be damaged");
	assert(traits.declarations[1].kind == DeclarationKind::FUNCTION);
	assert(traits.declarations[1].parent == 0);
	const std::vector<uint8_t> trait_bytes = encode_source_model(traits);
	SourceModel decoded_traits;
	assert(decode_source_model(trait_bytes.data(), trait_bytes.size(), decoded_traits));
	assert(decoded_traits.declarations[0].kind == DeclarationKind::TRAIT);

	SourceModel invalid_kind;
	invalid_kind.declarations.push_back(SourceDeclaration{});
	invalid_kind.declarations[0].kind = static_cast<DeclarationKind>(255);
	const std::vector<uint8_t> invalid_kind_bytes = encode_source_model(invalid_kind);
	std::string decode_error;
	assert(!decode_source_model(invalid_kind_bytes.data(), invalid_kind_bytes.size(),
			decoded_traits, decode_error));
	assert(decode_error == "declaration 0 has unsupported kind 255");

	std::cout << "source model passed\n";
}
