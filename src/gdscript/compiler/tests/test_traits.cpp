// Traits declarations, nominal conformance, run-time tests and
// trait-typed values all lower without changing Variant representation.
#include "../codegen.h"
#include "../chain.h"
#include "../compiler.h"
#include "../elf_builder.h"
#include "../compiler_exception.h"
#include "../function_signature.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include "../traits.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>

using namespace gdscript;

static Program parse(const std::string& source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	return parser.parse();
}

static IRProgram compile_to_ir(const std::string& source,
	const std::vector<std::pair<std::string, std::string>>& ancestry = {}) {
	Program program = parse(source);
	apply_traits(program);
	CodeGenerator codegen;
	codegen.set_engine_ancestry(ancestry);
	return codegen.generate(program);
}

static std::string rejection(const std::string& source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException& error) {
		return error.what();
	}
	return {};
}

static const IRFunction& find_function(const IRProgram& ir, const std::string& name) {
	for (const IRFunction& function : ir.functions) {
		if (function.name == name) return function;
	}
	throw std::runtime_error("Function not found: " + name);
}

static const FunctionSignature& find_signature(const IRProgram& ir, const std::string& name) {
	for (const FunctionSignature& signature : ir.signatures) {
		if (signature.name == name) return signature;
	}
	throw std::runtime_error("Signature not found: " + name);
}

static int count_opcode(const IRFunction& function, IROpcode opcode) {
	return int(std::count_if(function.instructions.begin(), function.instructions.end(),
		[opcode](const IRInstruction& instruction) { return instruction.opcode == opcode; }));
}

static bool calls_symbol(const IRProgram& ir, const IRFunction& function,
	const std::string& symbol) {
	for (const IRInstruction& instruction : function.instructions) {
		if (instruction.opcode == IROpcode::CALL && !instruction.operands.empty() &&
			instruction.operands[0].type == IRValue::Type::STRING &&
			ir.strings[instruction.operands[0].string_id] == symbol) return true;
	}
	return false;
}

static const std::string KILLABLE =
	"trait Killable:\n"
	"\t## Ends this value's lifetime.\n"
	"\tfunc die() -> void\n"
	"\tfunc take_damage(amount: int = 1) -> bool\n\n";

static void test_declaration_and_operator_parse() {
	Program program = parse(
		"class_name Actor\n"
		"uses Killable\n\n" + KILLABLE +
		"class Enemy uses Killable:\n"
		"\tfunc die() -> void: pass\n"
		"\tfunc take_damage(amount: int = 1) -> bool: return true\n\n"
		"struct Crate uses Killable:\n"
		"\tfunc die() -> void: pass\n"
		"\tfunc take_damage(amount: int = 1) -> bool: return true\n\n"
		"func test(value):\n"
		"\treturn value is Killable\n");
	assert(program.traits.size() == 1);
	assert(program.traits[0].name == "Killable");
	assert(program.traits[0].methods.size() == 2);
	assert(program.traits[0].methods[1].parameters.size() == 1);
	assert(program.uses == std::vector<std::string>{"Killable"});
	assert(program.structs.size() == 2);
	assert(program.structs[0].uses == std::vector<std::string>{"Killable"});
	auto* returned = dynamic_cast<ReturnStmt*>(program.functions[0].body[0].get());
	assert(returned != nullptr);
	auto* test = dynamic_cast<TypeTestExpr*>(returned->value.get());
	assert(test != nullptr && test->type.single_name() == "Killable");
}

static void test_trait_body_restrictions() {
	Program program = parse(
		"trait Stateful extends Node uses Base:\n"
		"\t## State docs\n"
		"\tvar value: int = 1\n"
		"\tconst LIMIT := 4\n"
		"\tenum State { READY, DONE }\n"
		"\tsignal changed(value: int)\n"
		"\tstatic func label() -> String: return \"state\"\n"
		"\tfunc increment() -> void: value += 1\n"
		"\t@abstract func reset() -> void\n");
	const TraitDecl& trait = program.traits[0];
	assert(trait.base_name == "Node");
	assert(trait.uses == std::vector<std::string>{"Base"});
	assert(trait.vars.size() == 1 && trait.constants.size() == 1);
	assert(trait.enums.size() == 1 && trait.signals.size() == 1);
	assert(trait.methods.size() == 3);
	assert(trait.methods[0].is_static && !trait.methods[0].is_abstract);
	assert(trait.methods[2].is_abstract);
	assert(rejection("trait I:\n\t@abstract func run(): pass\n").find(
		"cannot have a body") != std::string::npos);
	assert(rejection("trait I:\n\tstatic func run()\n").find(
		"must have a body") != std::string::npos);
	assert(rejection("trait I:\n\tclass Inner:\n\t\tpass\n").find(
		"cannot declare an inner class") != std::string::npos);
	Program file = parse(
		"trait_name Movable\n"
		"extends CharacterBody2D\n"
		"var speed: float = 100.0\n"
		"func move(delta: float) -> void: pass\n");
	assert(file.trait_name == "Movable");
	assert(file.traits.size() == 1 && file.traits[0].is_file_level);
	assert(file.traits[0].base_name == "CharacterBody2D");
}

static void test_splicing_conflicts_and_displaced_calls() {
	const IRProgram ir = compile_to_ir(
		"uses Counter\n"
		"trait Counter:\n"
		"\tvar count: int = 1\n"
		"\tfunc bump(amount: int = 1) -> int:\n"
		"\t\tcount += amount\n"
		"\t\treturn count\n"
		"func bump(amount: int = 1) -> int:\n"
		"\treturn Counter.bump(amount) + 10\n");
	assert(ir.globals.size() == 1 && ir.globals[0].name == "count");
	assert(find_function(ir, "@trait.Counter.bump").name == "@trait.Counter.bump");

	const std::string conflict = rejection(
		"trait A:\n\tfunc tick() -> void: pass\n"
		"trait B:\n\tfunc tick() -> void: pass\n"
		"class Broken uses A, B:\n\tpass\n");
	assert(conflict.find("both declare 'tick'; override it") != std::string::npos);
	compile_to_ir(
		"trait A:\n\tfunc tick() -> void: pass\n"
		"trait B:\n\tfunc tick() -> void: pass\n"
		"class Good uses A, B:\n\tfunc tick() -> void: pass\n");
	compile_to_ir(
		"trait A:\n\tvar value: int = 1\n"
		"trait B:\n\tvar value: int = 2\n"
		"struct Good uses A, B:\n\tpass\n");
	assert(rejection(
		"trait A:\n\tuses B\n"
		"trait B:\n\tuses A\n"
		"class Broken uses A:\n\tpass\n").find("Trait uses cycle") != std::string::npos);
	assert(rejection(
		"trait WithSignal:\n\tsignal changed\n"
		"class Broken uses WithSignal:\n\tpass\n").find(
		"nested classes do not support signals") != std::string::npos);

	std::vector<ChainLink> links;
	links.push_back(ChainLink{"Base", "base.sgd", parse(
		"class_name Base\nuses Counter\n"
		"trait Counter:\n\tfunc bump() -> int: return 1\n")});
	links.push_back(ChainLink{"Derived", "derived.sgd", parse(
		"extends Base\nfunc bump() -> int: return Counter.bump() + 1\n")});
	std::vector<const TraitDecl*> available;
	for (const ChainLink& link : links)
		for (const TraitDecl& trait : link.program.traits) available.push_back(&trait);
	for (ChainLink& link : links) apply_traits(link.program, available);
	Program merged = merge_chain(std::move(links));
	CodeGenerator chain_codegen;
	const IRProgram chain_ir = chain_codegen.generate(merged);
	assert(find_function(chain_ir, "@super0.bump").name == "@super0.bump");
	assert(calls_symbol(chain_ir, find_function(chain_ir, "bump"), "@super0.bump"));
}

static void test_base_requirements() {
	const std::string source =
		"extends CharacterBody2D\n"
		"uses Movable\n"
		"trait Movable extends Node2D:\n\tfunc move() -> void: pass\n";
	assert(rejection(source).find("requires base 'Node2D'") != std::string::npos);
	const IRProgram accepted = compile_to_ir(source,
		{{"CharacterBody2D", "Node2D,CanvasItem,Node,Object"}});
	assert(accepted.script_uses == std::vector<std::string>{"Movable"});
	assert(rejection(
		"trait Spatial extends Node2D:\n\tfunc move() -> void: pass\n"
		"trait ResourceLike extends Resource uses Spatial:\n\tpass\n").find(
		"requires unrelated base") != std::string::npos);
	compile_to_ir(
		"trait Rooted extends Node:\n\tfunc touch() -> void: pass\n"
		"trait Spatial extends Node2D uses Rooted:\n\tpass\n",
		{{"Node2D", "CanvasItem,Node,Object"}});
	const IRProgram inherited_requirement = compile_to_ir(
		"trait Rooted extends Node:\n\tfunc touch() -> void: pass\n"
		"trait Spatial uses Rooted:\n\tpass\n"
		"func accept(value: Spatial) -> Spatial:\n\treturn value\n",
		{{"Node2D", "CanvasItem,Node,Object"}});
	assert(inherited_requirement.trait_signatures[1].native_base == "Node");
	const FunctionSignature accept = find_signature(inherited_requirement, "accept");
	assert(accept.return_type == Variant::OBJECT);
	assert(accept.return_class_name == "Node");
	assert(accept.parameters[0].type == Variant::OBJECT);
	assert(accept.parameters[0].class_name == "Node");
	assert(rejection(
		"trait Spatial extends Node2D:\n\tpass\n"
		"trait ResourceLike extends Resource:\n\tpass\n"
		"trait Broken uses Spatial, ResourceLike:\n\tpass\n").find(
		"requires unrelated base") != std::string::npos);
}

static void test_trait_constants_and_enums() {
	const IRProgram ir = compile_to_ir(
		"trait Stateful:\n"
		"\tconst LIMIT := 9\n"
		"\tenum State { READY = 3, DEAD = 7 }\n"
		"func qualified() -> int:\n\treturn Stateful.State.DEAD + Stateful.LIMIT\n"
		"func through_value(value: Stateful) -> int:\n\treturn value.State.READY\n");
	assert(count_opcode(find_function(ir, "qualified"), IROpcode::LOAD_IMM) >= 2);
	assert(count_opcode(find_function(ir, "through_value"), IROpcode::LOAD_IMM) == 1);

	const IRProgram inherited = compile_to_ir(
		"trait Living:\n"
		"\tvar health: int = 10\n"
		"\tsignal changed(value: int)\n"
		"\tfunc damage(amount: int) -> int\n"
		"trait Damageable uses Living:\n\tpass\n"
		"func use(value: Damageable) -> int:\n"
		"\tvalue.health = value.damage(2)\n"
		"\treturn value.health\n");
	assert(count_opcode(find_function(inherited, "use"), IROpcode::VCALL) == 1);
	const ClassSignature& derived = inherited.trait_signatures[1];
	assert(derived.uses == std::vector<std::string>{"Living"});
	assert(derived.trait_methods.size() == 1);
	assert(derived.trait_fields.size() == 1);
	assert(derived.trait_signals.size() == 1);
}

static void test_nominal_conformance_and_inheritance() {
	IRProgram ir = compile_to_ir(KILLABLE +
		"class Base extends Node uses Killable:\n"
		"\tfunc die() -> void: pass\n"
		"\tfunc take_damage(amount) -> bool: return true\n\n"
		"class Child extends Base:\n\tpass\n");
	assert(ir.trait_signatures.size() == 1);
	assert(ir.class_signatures.size() == 2);
	const ClassSignature& child = ir.class_signatures.back();
	assert(child.name == "Child");
	assert(child.uses == std::vector<std::string>{"Killable"});

	const std::string missing = rejection(KILLABLE +
		"class Broken uses Killable:\n"
		"\tfunc die() -> void: pass\n");
	assert(missing.find("does not implement abstract method 'take_damage()'") != std::string::npos);

	const std::string arity = rejection(
		"trait I:\n\tfunc run(value: int) -> void\n\n"
		"class Broken uses I:\n\tfunc run() -> void: pass\n");
	assert(arity.find("takes 0 parameters") != std::string::npos);

	const std::string return_type = rejection(
		"trait I:\n\tfunc run() -> int\n\n"
		"class Broken uses I:\n\tfunc run(): return 1\n");
	assert(return_type.find("untyped return") != std::string::npos);
}

static void test_folded_and_dynamic_tests() {
	const IRProgram ir = compile_to_ir(KILLABLE +
		"struct Crate uses Killable:\n"
		"\tfunc die() -> void: pass\n"
		"\tfunc take_damage(amount: int = 1) -> bool: return true\n\n"
		"func folded(value: Crate):\n\treturn value is Killable\n\n"
		"func builtin(value: int):\n\treturn value is Killable\n\n"
		"func dynamic(value):\n\treturn value is Killable\n");
	assert(count_opcode(find_function(ir, "folded"), IROpcode::TRAIT_TEST) == 0);
	assert(count_opcode(find_function(ir, "builtin"), IROpcode::TRAIT_TEST) == 0);
	assert(count_opcode(find_function(ir, "dynamic"), IROpcode::TRAIT_TEST) == 1);
}

static void test_narrowing_and_trait_calls() {
	const IRProgram ir = compile_to_ir(KILLABLE +
		"func narrowed(value):\n"
		"\tif value is Killable:\n"
		"\t\tvalue.die()\n\n"
		"func typed(value: Killable) -> bool:\n"
		"\treturn value.take_damage()\n\n"
		"func nullable(value: Killable?):\n"
		"\tif value != null:\n"
		"\t\tvalue.die()\n");
	assert(count_opcode(find_function(ir, "narrowed"), IROpcode::TRAIT_TEST) == 1);
	assert(count_opcode(find_function(ir, "narrowed"), IROpcode::VCALL) == 1);
	assert(count_opcode(find_function(ir, "typed"), IROpcode::VCALL) == 1);
	assert(count_opcode(find_function(ir, "nullable"), IROpcode::VCALL) == 1);

	const std::string unknown = rejection(KILLABLE +
		"func broken(value: Killable):\n\tvalue.heal()\n");
	assert(unknown.find("'Killable' has no method 'heal'") != std::string::npos);
}

static void test_trait_typed_containers() {
	const IRProgram ir = compile_to_ir(KILLABLE +
		"func visit(values: Array[Killable]):\n"
		"\tfor value in values:\n"
		"\t\tvalue.die()\n\n"
		"func first(values: Array[Killable]):\n"
		"\tvalues[0].die()\n\n"
		"func add(values: Array[Killable], value):\n"
		"\tvalues.append(value)\n");
	assert(count_opcode(find_function(ir, "visit"), IROpcode::TRAIT_TEST) == 1);
	assert(count_opcode(find_function(ir, "visit"), IROpcode::VCALL) == 1);
	assert(count_opcode(find_function(ir, "first"), IROpcode::TRAIT_TEST) == 1);
	assert(count_opcode(find_function(ir, "add"), IROpcode::TRAIT_TEST) == 1);
}

static void test_signature_publication_and_backend_cache() {
	const IRProgram ir = compile_to_ir(
		"class_name Enemy\n"
		"extends Node\n"
		"uses Stateful\n"
		"trait Stateful:\n"
		"\tvar health: int = 10\n"
		"\tsignal changed(value: int)\n"
		"\tfunc die() -> void\n"
		"\tfunc take_damage(amount: int = 1) -> bool\n\n"
		"func die() -> void: pass\n"
		"func take_damage(amount: int = 1) -> bool: return true\n\n"
		"func dynamic(value):\n\treturn value is Stateful\n");
	assert(ir.trait_signatures.size() == 1);
	assert(ir.trait_signatures[0].is_trait);
	assert(ir.trait_signatures[0].trait_methods.size() == 2);
	assert(ir.trait_signatures[0].trait_fields.size() == 1);
	assert(ir.trait_signatures[0].trait_signals.size() == 1);

	std::vector<ClassSignature> published = ir.trait_signatures;
	published.insert(published.end(), ir.class_signatures.begin(), ir.class_signatures.end());
	const std::vector<uint8_t> blob = encode_class_signatures(published);
	std::vector<ClassSignature> decoded;
	assert(decode_class_signatures(blob.data(), blob.size(), decoded));
	assert(decoded[0].is_trait);
	assert(decoded[0].trait_methods[1].name == "take_damage");
	assert(decoded[0].trait_fields[0].name == "health");
	assert(decoded[0].trait_signals[0].name == "changed");

	RISCVCodeGen backend{VariantLayout(false)};
	const std::vector<uint8_t> elf = backend.generate(ir);
	assert(!elf.empty());
	// One direct-mapped cache is 64 entries of {handle,result}.
	assert(backend.get_global_data_size() >= 64 * 16);
}

// Mirrors the host's lookup: the caches must be in .symtab, or
// Sandbox::clear_trait_caches() finds nothing to clear.
static uint64_t elf_symbol(const std::vector<uint8_t>& elf, const std::string& name,
	uint64_t* r_size = nullptr)
{
	auto u16 = [&](size_t at) { return uint64_t(elf[at]) | (uint64_t(elf[at + 1]) << 8); };
	auto u32 = [&](size_t at) {
		uint64_t value = 0;
		for (int i = 0; i < 4; i++) value |= uint64_t(elf[at + i]) << (i * 8);
		return value;
	};
	auto u64 = [&](size_t at) {
		uint64_t value = 0;
		for (int i = 0; i < 8; i++) value |= uint64_t(elf[at + i]) << (i * 8);
		return value;
	};
	const uint64_t section_offset = u64(0x28);
	const uint64_t section_size = u16(0x3A);
	const uint64_t section_count = u16(0x3C);
	for (uint64_t i = 0; i < section_count; i++) {
		const size_t header = size_t(section_offset + i * section_size);
		if (u32(header + 4) != 2) continue; // SHT_SYMTAB
		const size_t symbols = size_t(u64(header + 0x18));
		const size_t symbols_size = size_t(u64(header + 0x20));
		const size_t strings_header = size_t(section_offset + u32(header + 0x28) * section_size);
		const size_t strings = size_t(u64(strings_header + 0x18));
		for (size_t at = symbols; at + 24 <= symbols + symbols_size; at += 24) {
			if (name != reinterpret_cast<const char*>(&elf[strings + u32(at)])) continue;
			if (r_size != nullptr) *r_size = u64(at + 16);
			return u64(at + 8);
		}
	}
	return 0;
}

static void test_trait_caches_are_exported_symbols() {
	const IRProgram ir = compile_to_ir(KILLABLE +
		"trait Movable:\n\tfunc step() -> void\n\n"
		"func first(value):\n\treturn value is Killable\n\n"
		"func second(value):\n\treturn value is Movable\n");
	assert(ir.trait_signatures.size() == 2);

	ElfBuilder builder;
	const std::vector<uint8_t> elf = builder.build(ir);
	const std::string prefix = TRAIT_CACHE_SYMBOL_PREFIX;
	uint64_t size = 0;
	const uint64_t first = elf_symbol(elf, prefix + "0", &size);
	assert(first != 0);
	assert(size == TraitCacheLayout::AREA_SIZE);
	const uint64_t second = elf_symbol(elf, prefix + "1");
	assert(second == first + TraitCacheLayout::AREA_SIZE);
	assert(elf_symbol(elf, prefix + "2") == 0);
}

static void test_an_unassigned_trait_local_is_not_proven() {
	const IRProgram ir = compile_to_ir(KILLABLE +
		"struct Crate uses Killable:\n"
		"\tfunc die() -> void: pass\n"
		"\tfunc take_damage(amount: int = 1) -> bool: return true\n\n"
		"func declared():\n\tvar d: Killable\n\treturn d is Killable\n\n"
		"func assigned():\n\tvar d: Killable = Crate()\n\treturn d is Killable\n");
	// GDScript leaves the declaration null, so the test cannot fold true.
	assert(count_opcode(find_function(ir, "declared"), IROpcode::TRAIT_TEST) == 1);
	assert(count_opcode(find_function(ir, "assigned"), IROpcode::TRAIT_TEST) == 0);

	const std::string unproven = rejection(KILLABLE +
		"func broken():\n\tvar d: Killable\n\td.die()\n");
	assert(unproven.find("may be null") != std::string::npos);
}

static void test_a_nullable_trait_needs_a_null_check() {
	const std::string unchecked = rejection(KILLABLE +
		"func broken(value: Killable?):\n\tvalue.die()\n");
	assert(unchecked.find("may be null") != std::string::npos);

	// The binding carries the declaration: naming the trait must not crash.
	const std::string bound = rejection(KILLABLE +
		"func broken(value: Killable?):\n"
		"\tmatch value:\n"
		"\t\tvar bound:\n"
		"\t\t\tbound.die()\n");
	assert(bound.find("may be null") != std::string::npos);
}

static const ClassSignature& find_published_class(const Compiler& compiler,
	const std::string& name) {
	for (const ClassSignature& signature : compiler.get_class_signatures()) {
		if (signature.name == name) return signature;
	}
	throw std::runtime_error("Published class not found: " + name);
}

static void test_file_level_trait_sources() {
	CompilerOptions options;
	options.base_sources.push_back(CompilerOptions::BaseSource{
		"Movable", "movable.sgd",
		"trait_name Movable\n"
		"var distance: int = 4\n"
		"func move() -> int: return distance\n",
		true});

	Compiler compiler;
	const std::vector<uint8_t> elf = compiler.compile(
		"uses Movable\n"
		"func run() -> int: return move()\n", options);
	assert(!elf.empty());
	assert(compiler.get_error().empty());
	assert(compiler.get_script_uses() == std::vector<std::string>{"Movable"});
	assert(find_published_class(compiler, "Movable").is_trait);
}

static void test_transitive_file_level_trait_sources() {
	CompilerOptions options;
	options.base_sources.push_back(CompilerOptions::BaseSource{
		"Powered", "powered.sgd",
		"trait_name Powered\n"
		"uses Movable\n"
		"func power() -> int: return move() * 2\n",
		true});
	options.base_sources.push_back(CompilerOptions::BaseSource{
		"Movable", "movable.sgd",
		"trait_name Movable\n"
		"func move() -> int: return 6\n",
		true});

	Compiler compiler;
	const std::vector<uint8_t> elf = compiler.compile(
		"uses Powered\n"
		"func run() -> int: return power()\n", options);
	assert(!elf.empty());
	assert(compiler.get_error().empty());
	assert(compiler.get_script_uses() == std::vector<std::string>({"Movable", "Powered"}));
	assert(find_published_class(compiler, "Movable").is_trait);
	assert(find_published_class(compiler, "Powered").is_trait);
}

static void test_two_qualified_traits_from_one_provider() {
	const std::string provider =
		"class_name TraitLibrary\n"
		"trait Alpha:\n\tfunc alpha() -> int: return 10\n"
		"trait Beta:\n\tfunc beta() -> int: return 20\n"
		"trait Unused:\n\tfunc unused() -> int: return 30\n";
	CompilerOptions options;
	// The host emits one trait-only source request per qualified name. Repeating
	// the provider must expose both names without declaring Unused twice.
	options.base_sources.push_back(CompilerOptions::BaseSource{
		"TraitLibrary.Alpha", "trait_library.sgd", provider, true});
	options.base_sources.push_back(CompilerOptions::BaseSource{
		"TraitLibrary.Beta", "trait_library.sgd", provider, true});

	Compiler compiler;
	const std::vector<uint8_t> elf = compiler.compile(
		"uses TraitLibrary.Alpha, TraitLibrary.Beta\n"
		"func run() -> int: return alpha() + beta()\n", options);
	assert(!elf.empty());
	assert(compiler.get_error().empty());
	assert(find_published_class(compiler, "TraitLibrary.Alpha").is_trait);
	assert(find_published_class(compiler, "TraitLibrary.Beta").is_trait);
	assert(find_published_class(compiler, "Unused").is_trait);
}

int main() {
	std::cout << "=== Trait Tests ===" << std::endl;
	test_declaration_and_operator_parse();
	test_trait_body_restrictions();
	test_splicing_conflicts_and_displaced_calls();
	test_base_requirements();
	test_trait_constants_and_enums();
	test_nominal_conformance_and_inheritance();
	test_folded_and_dynamic_tests();
	test_narrowing_and_trait_calls();
	test_trait_typed_containers();
	test_signature_publication_and_backend_cache();
	test_trait_caches_are_exported_symbols();
	test_an_unassigned_trait_local_is_not_proven();
	test_a_nullable_trait_needs_a_null_check();
	test_file_level_trait_sources();
	test_transitive_file_level_trait_sources();
	test_two_qualified_traits_from_one_provider();
	std::cout << "All trait tests passed!" << std::endl;
	return 0;
}
