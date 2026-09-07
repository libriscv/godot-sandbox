#include "../ast_clone.h"
#include "../compiler.h"
#include "../compiler_exception.h"
#include "../gdsmeta.h"
#include "../lexer.h"
#include "../parser.h"
#include "../trait_conformance.h"
#include <cassert>
#include <iostream>

using namespace gdscript;

static const std::string hook_source =
	"@requires_host_hook\nfunc on_round_finished(won: bool) -> int: return 1\n";

static void parser_checks() {
	Parser parser(Lexer(hook_source).tokenize());
	auto program = parser.parse();
	assert(program.functions[0].requires_host_hook);
	assert(clone_function(program.functions[0]).requires_host_hook);
	for (const std::string &source : {
		"@requires_host_hook(1)\nfunc f(): pass\n",
		"@requires_host_hook\n@requires_host_hook\nfunc f(): pass\n",
		"@requires_host_hook\nstatic func f(): pass\n",
		"@requires_host_hook\nvar x = 1\n",
		"@requires_host_hook\nsignal changed\n",
		"@requires_host_hook\n",
		"@requires_host_hook\n@test\nfunc f(): pass\n",
		"@rpc\n@requires_host_hook\nfunc f(): pass\n",
		"trait_name Hook\n@requires_host_hook\nfunc f(): pass\n",
		"class Nested:\n\t@requires_host_hook\n\tfunc f(): pass\n",
		"func outer():\n\t@requires_host_hook\n\tvar x = 1\n",
	}) {
		bool rejected = false;
		try { Parser(Lexer(source).tokenize()).parse(); }
		catch (const CompilerException &e) {
			rejected = e.message().find("@requires_host_hook") != std::string::npos && e.line() > 0 && e.column() > 0;
		}
		assert(rejected);
	}
}

static void metadata_and_contract_checks() {
	Compiler compiler;
	CompilerOptions options;
	options.emit_tests = false; // Requirements survive shipping builds.
	assert(!compiler.compile(hook_source, options).empty());
	const auto functions = compiler.get_function_signatures();
	assert(functions.size() == 1 && functions[0].requires_host_hook);
	const auto blob = encode_function_signatures(functions);
	std::vector<FunctionSignature> decoded;
	assert(decode_function_signatures(blob.data(), blob.size(), decoded));
	assert(decoded[0].requires_host_hook);
	// Ordinary pre-feature records remain readable and byte-identical.
	auto ordinary = functions;
	ordinary[0].requires_host_hook = false;
	const auto old_blob = encode_function_signatures(ordinary);
	assert(old_blob.size() + 9 == blob.size());
	assert(std::equal(old_blob.begin(), old_blob.end(), blob.begin()));
	assert(decode_function_signatures(old_blob.data(), old_blob.size(), decoded));
	assert(!decoded[0].requires_host_hook);
	for (size_t size = old_blob.size() + 1; size < blob.size(); ++size) {
		assert(!decode_function_signatures(blob.data(), size, decoded));
		assert(decoded.empty());
	}
	for (size_t offset : {old_blob.size(), old_blob.size() + 4, blob.size() - 1}) {
		auto corrupt = blob;
		corrupt[offset] = 0xff;
		assert(!decode_function_signatures(corrupt.data(), corrupt.size(), decoded));
		assert(decoded.empty());
	}
	ScriptMetadata meta, restored;
	meta.functions = functions;
	const auto metadata = encode_script_metadata(meta);
	assert(decode_script_metadata(metadata.data(), metadata.size(), restored));
	assert(restored.functions[0].requires_host_hook);

	ClassSignature host;
	host.name = "ModBrain";
	host.is_trait = true;
	assert(required_host_hooks(functions, host).size() == 1);
	assert(required_host_hooks(ordinary, host).empty());
	host.trait_methods = ordinary;
	assert(required_host_hooks(functions, host).empty());
	host.trait_methods[0].is_abstract = true;
	assert(required_host_hooks(functions, host).empty());
	host.trait_methods[0].parameters.clear();
	assert(required_host_hooks(functions, host)[0].find("takes 1 parameters") != std::string::npos);
	host.trait_methods = ordinary;
	host.trait_methods[0].parameters[0].declared_type = {"String", uint64_t(1) << 4, false};
	assert(required_host_hooks(functions, host)[0].find("incompatible type") != std::string::npos);
	// Widened mod parameters accept the host's arguments, but not vice versa.
	auto broad = functions;
	broad[0].parameters[0].declared_type = {"Variant", 0, false};
	assert(required_host_hooks(broad, host).empty());
	host.trait_methods = ordinary;
	host.trait_methods[0].declared_return = {"String", uint64_t(1) << 4, false};
	assert(required_host_hooks(functions, host)[0].find("returns 'int'") != std::string::npos);
	host.trait_methods = ordinary;
	host.trait_methods[0].is_static = true;
	assert(!required_host_hooks(functions, host).empty());
}

static void inherited_requirement() {
	Compiler compiler;
	CompilerOptions options;
	options.base_sources.push_back({"BaseMod", "base.sgd", "class_name BaseMod\n" + hook_source, false});
	assert(!compiler.compile("extends BaseMod\n", options).empty());
	bool found = false;
	for (const auto &f : compiler.get_function_signatures()) {
		if (f.name == "on_round_finished") found = f.requires_host_hook;
	}
	assert(found);
}

int main() {
	parser_checks();
	metadata_and_contract_checks();
	inherited_requirement();
	std::cout << "PASS required host hooks: parsing, inheritance, metadata and conformance\n";
}
