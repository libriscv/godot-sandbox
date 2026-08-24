// Structs: sugar for a Dictionary with a fixed set of keys.
//
// Nothing about a struct survives into the IR, so what these tests pin down is
// the lowering: that an instance is a MAKE_DICTIONARY holding every declared
// field, that a field access is a Dictionary get or set rather than the
// property syscall (which reaches an Object and nothing else), and that a field
// name the struct does not declare is a compile error instead of a new key.
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../ir_optimizer.h"
#include "../riscv_codegen.h"
#include "../compiler_exception.h"
#include "../variant_layout.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;

// -= Helpers =-

static Program parse(const std::string& source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	return parser.parse();
}

static IRProgram compile_to_ir(const std::string& source, bool optimize = false) {
	Program program = parse(source);
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	if (optimize) {
		IROptimizer optimizer;
		optimizer.optimize(ir);
	}
	return ir;
}

static const IRFunction& find_function(const IRProgram& ir, const std::string& name) {
	for (const auto& func : ir.functions) {
		if (func.name == name) {
			return func;
		}
	}
	throw std::runtime_error("Function not found: " + name);
}

static int count_opcode(const IRFunction& func, IROpcode opcode) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == opcode) {
			count++;
		}
	}
	return count;
}

// The method names of every VCALL in a function, in order.
static std::vector<std::string> vcall_methods(const IRFunction& func) {
	std::vector<std::string> methods;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::VCALL) {
			methods.push_back(std::get<std::string>(instr.operands.at(2).value));
		}
	}
	return methods;
}

static int count_vcalls(const IRFunction& func, const std::string& method) {
	int count = 0;
	for (const auto& name : vcall_methods(func)) {
		if (name == method) {
			count++;
		}
	}
	return count;
}

// Dictionary element accesses, by Dictionary_Op. A field read is
// ECALL_DICTIONARY_OPS with GET; a field write is the DICT_SET opcode.
static int count_dict_gets(const IRFunction& func) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() >= 3 &&
			std::get<int64_t>(instr.operands[1].value) == 524 &&
			std::get<int64_t>(instr.operands[2].value) == 0) {
			count++;
		}
	}
	return count;
}

// The string a register was last loaded with before instruction `before`.
static std::string string_in_register(const IRProgram& ir, const IRFunction& func,
	size_t before, int reg)
{
	for (size_t i = before; i-- > 0; ) {
		const auto& instr = func.instructions[i];
		if (instr.opcode != IROpcode::LOAD_STRING) {
			continue;
		}
		if (std::get<int>(instr.operands.at(0).value) != reg) {
			continue;
		}
		return ir.string_constants.at(std::get<int64_t>(instr.operands.at(1).value));
	}
	throw std::runtime_error("no LOAD_STRING for register " + std::to_string(reg));
}

// The keys of the first MAKE_DICTIONARY in a function, in the order they are
// built. For a struct instance that is the order the fields were declared in.
static std::vector<std::string> dictionary_keys(const IRProgram& ir, const IRFunction& func) {
	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];
		if (instr.opcode != IROpcode::MAKE_DICTIONARY) {
			continue;
		}
		const int64_t pairs = std::get<int64_t>(instr.operands.at(1).value);
		std::vector<std::string> keys;
		for (int64_t pair = 0; pair < pairs; pair++) {
			const int key_reg = std::get<int>(instr.operands.at(2 + pair * 2).value);
			keys.push_back(string_in_register(ir, func, i, key_reg));
		}
		return keys;
	}
	throw std::runtime_error("no MAKE_DICTIONARY in function " + func.name);
}

// The immediate a register was last loaded with before instruction `before`.
static int64_t int_in_register(const IRFunction& func, size_t before, int reg) {
	for (size_t i = before; i-- > 0; ) {
		const auto& instr = func.instructions[i];
		if (instr.opcode != IROpcode::LOAD_IMM) {
			continue;
		}
		if (std::get<int>(instr.operands.at(0).value) != reg) {
			continue;
		}
		return std::get<int64_t>(instr.operands.at(1).value);
	}
	throw std::runtime_error("no LOAD_IMM for register " + std::to_string(reg));
}

// The integer values of the first MAKE_DICTIONARY in a function, by field order.
static std::vector<int64_t> dictionary_int_values(const IRFunction& func) {
	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];
		if (instr.opcode != IROpcode::MAKE_DICTIONARY) {
			continue;
		}
		const int64_t pairs = std::get<int64_t>(instr.operands.at(1).value);
		std::vector<int64_t> values;
		for (int64_t pair = 0; pair < pairs; pair++) {
			const int value_reg = std::get<int>(instr.operands.at(3 + pair * 2).value);
			values.push_back(int_in_register(func, i, value_reg));
		}
		return values;
	}
	throw std::runtime_error("no MAKE_DICTIONARY in function " + func.name);
}

// Returns true when compiling the source throws a CompilerException.
static bool rejects(const std::string& source) {
	try {
		compile_to_ir(source);
		return false;
	} catch (const CompilerException&) {
		return true;
	}
}

// The declaration every test below builds on.
static const std::string BANK_ACCOUNT =
	"struct BankAccount:\n"
	"\tvar balance = 0\n"
	"\tvar loan = 0\n"
	"\n";

// -= Tests =-

static void test_struct_declaration_parses() {
	std::cout << "Testing that a struct declaration parses..." << std::endl;

	const Program program = parse(
		"struct BankAccount:\n"
		"\tvar balance = 0\n"
		"\tvar loan: int = 5\n"
		"\tvar owner: String\n"
		"\tvar note\n"
		"\n"
		"func test():\n"
		"\treturn 1\n");

	assert(program.structs.size() == 1);
	const StructDecl& decl = program.structs[0];
	assert(decl.name == "BankAccount");
	assert(decl.fields.size() == 4);

	assert(decl.fields[0].name == "balance");
	assert(decl.fields[0].type_hint.empty());
	assert(decl.fields[0].default_value != nullptr);

	assert(decl.fields[1].name == "loan");
	assert(decl.fields[1].type_hint == "int");
	assert(decl.fields[1].default_value != nullptr);

	// A type hint without a value, and a value without a type hint, are both
	// field declarations.
	assert(decl.fields[2].type_hint == "String");
	assert(decl.fields[2].default_value == nullptr);
	assert(decl.fields[3].type_hint.empty());
	assert(decl.fields[3].default_value == nullptr);

	assert(decl.field_index("loan") == 1);
	assert(decl.find_field("nope") == nullptr);
	assert(decl.field_list() == "balance, loan, owner, note");

	// The function after the struct is still parsed.
	assert(program.functions.size() == 1);

	// An empty struct is written with 'pass', like an empty function body.
	const Program empty = parse("struct Empty:\n\tpass\n\nfunc test():\n\treturn 1\n");
	assert(empty.structs.size() == 1);
	assert(empty.structs[0].fields.empty());

	std::cout << "  ✓ struct declarations parse" << std::endl;
}

static void test_struct_body_holds_only_fields() {
	std::cout << "Testing that a struct body holds only fields..." << std::endl;

	// A method in a struct body would silently not be part of the Dictionary
	// the struct lowers to, so it is rejected rather than skipped.
	assert(rejects("struct S:\n\tfunc method():\n\t\treturn 1\n"));
	// The same field twice is a mistake, not a redefinition.
	assert(rejects("struct S:\n\tvar x = 1\n\tvar x = 2\n"));

	std::cout << "  ✓ a struct body holds only fields" << std::endl;
}

static void test_construction_builds_a_dictionary() {
	std::cout << "Testing that constructing a struct builds a Dictionary..." << std::endl;

	const IRProgram ir = compile_to_ir(BANK_ACCOUNT +
		"func test():\n"
		"\treturn BankAccount.new()\n");
	const IRFunction& test = find_function(ir, "test");

	// One Dictionary, holding every declared field in declaration order, with
	// the declared defaults.
	assert(count_opcode(test, IROpcode::MAKE_DICTIONARY) == 1);
	const std::vector<std::string> keys = dictionary_keys(ir, test);
	assert((keys == std::vector<std::string>{ "balance", "loan" }));
	assert((dictionary_int_values(test) == std::vector<int64_t>{ 0, 0 }));

	// A struct is not an object: nothing about it reaches the property syscalls.
	assert(count_opcode(test, IROpcode::VGET) == 0);
	assert(count_opcode(test, IROpcode::VSET) == 0);

	// An empty struct is an empty Dictionary rather than an error.
	const IRProgram empty = compile_to_ir(
		"struct Empty:\n\tpass\n\nfunc test():\n\treturn Empty.new()\n");
	assert(dictionary_keys(empty, find_function(empty, "test")).empty());

	std::cout << "  ✓ constructing a struct builds a Dictionary" << std::endl;
}

static void test_both_constructor_forms() {
	std::cout << "Testing that both constructor forms build the same thing..." << std::endl;

	// `BankAccount.new(...)` is the Godot idiom; `BankAccount(...)` reads like
	// the built-in types. Both mean the same thing.
	const IRProgram with_new = compile_to_ir(BANK_ACCOUNT +
		"func test():\n\treturn BankAccount.new(100, 50)\n");
	const IRProgram plain = compile_to_ir(BANK_ACCOUNT +
		"func test():\n\treturn BankAccount(100, 50)\n");

	const IRFunction& a = find_function(with_new, "test");
	const IRFunction& b = find_function(plain, "test");
	assert(dictionary_keys(with_new, a) == dictionary_keys(plain, b));
	assert(dictionary_int_values(a) == dictionary_int_values(b));
	assert((dictionary_int_values(a) == std::vector<int64_t>{ 100, 50 }));

	std::cout << "  ✓ both constructor forms build the same thing" << std::endl;
}

static void test_named_arguments() {
	std::cout << "Testing named constructor arguments..." << std::endl;

	// Named arguments land on the field they name, whatever order they are
	// written in, and whichever constructor form is used.
	const IRProgram reordered = compile_to_ir(BANK_ACCOUNT +
		"func test():\n\treturn BankAccount.new(loan = 50, balance = 100)\n");
	assert((dictionary_int_values(find_function(reordered, "test")) ==
		std::vector<int64_t>{ 100, 50 }));

	const IRProgram plain = compile_to_ir(BANK_ACCOUNT +
		"func test():\n\treturn BankAccount(balance = 100, loan = 50)\n");
	assert((dictionary_int_values(find_function(plain, "test")) ==
		std::vector<int64_t>{ 100, 50 }));

	// Positional first, then named, the way Python and GDScript's own default
	// arguments read.
	const IRProgram mixed = compile_to_ir(BANK_ACCOUNT +
		"func test():\n\treturn BankAccount.new(100, loan = 50)\n");
	assert((dictionary_int_values(find_function(mixed, "test")) ==
		std::vector<int64_t>{ 100, 50 }));

	// A field left out keeps its declared default.
	const IRProgram partial = compile_to_ir(BANK_ACCOUNT +
		"func test():\n\treturn BankAccount.new(loan = 50)\n");
	assert((dictionary_int_values(find_function(partial, "test")) ==
		std::vector<int64_t>{ 0, 50 }));

	// A name the struct does not declare, a field given a value twice, a
	// positional argument after a named one, and more values than fields.
	assert(rejects(BANK_ACCOUNT + "func test():\n\treturn BankAccount.new(blance = 1)\n"));
	assert(rejects(BANK_ACCOUNT + "func test():\n\treturn BankAccount.new(1, balance = 2)\n"));
	assert(rejects(BANK_ACCOUNT + "func test():\n\treturn BankAccount.new(balance = 1, 2)\n"));
	assert(rejects(BANK_ACCOUNT + "func test():\n\treturn BankAccount.new(1, 2, 3)\n"));

	// Naming an argument of anything that is not a struct constructor would
	// drop the name silently, so it is rejected instead.
	assert(rejects("func other(a):\n\treturn a\n\nfunc test():\n\treturn other(a = 1)\n"));

	std::cout << "  ✓ named constructor arguments" << std::endl;
}

static void test_field_access_is_dictionary_access() {
	std::cout << "Testing that a field access is a Dictionary access..." << std::endl;

	const IRProgram ir = compile_to_ir(BANK_ACCOUNT +
		"func test():\n"
		"\tvar account = BankAccount.new()\n"
		"\taccount.balance = 100\n"
		"\treturn account.balance\n");
	const IRFunction& test = find_function(ir, "test");

	// The property syscalls reach an Object's properties and nothing else, so a
	// struct field has to take the element path or it throws at run time.
	assert(count_opcode(test, IROpcode::VGET) == 0);
	assert(count_opcode(test, IROpcode::VSET) == 0);
	assert(count_opcode(test, IROpcode::DICT_SET) == 1);
	assert(count_dict_gets(test) == 1);

	// A field of a struct held in a parameter is known too, from the type hint.
	const IRProgram parameter = compile_to_ir(BANK_ACCOUNT +
		"func test(account: BankAccount):\n\treturn account.balance\n");
	assert(count_opcode(find_function(parameter, "test"), IROpcode::VGET) == 0);
	assert(count_dict_gets(find_function(parameter, "test")) == 1);

	// And of one returned by a function that declares it returns one.
	const IRProgram returned = compile_to_ir(BANK_ACCOUNT +
		"func open() -> BankAccount:\n\treturn BankAccount.new()\n"
		"\n"
		"func test():\n\treturn open().balance\n");
	assert(count_opcode(find_function(returned, "test"), IROpcode::VGET) == 0);
	assert(count_dict_gets(find_function(returned, "test")) == 1);

	std::cout << "  ✓ a field access is a Dictionary access" << std::endl;
}

static void test_unknown_field_is_rejected() {
	std::cout << "Testing that an unknown field is rejected..." << std::endl;

	// Catching the typo is what a struct buys over a bare Dictionary, so a
	// field the struct does not declare is an error on read and on write alike.
	assert(rejects(BANK_ACCOUNT +
		"func test():\n\tvar a = BankAccount.new()\n\treturn a.blance\n"));
	assert(rejects(BANK_ACCOUNT +
		"func test():\n\tvar a = BankAccount.new()\n\ta.blance = 1\n\treturn a\n"));
	assert(rejects(BANK_ACCOUNT +
		"func test(a: BankAccount):\n\treturn a.blance\n"));

	// A method call on an instance is still a Dictionary method, not a field.
	const IRProgram methods = compile_to_ir(BANK_ACCOUNT +
		"func test():\n\tvar a = BankAccount.new()\n\treturn a.size()\n");
	assert(count_vcalls(find_function(methods, "test"), "size") == 1);

	// The declared fields are still reachable through the Dictionary itself.
	const IRProgram by_key = compile_to_ir(BANK_ACCOUNT +
		"func test():\n\tvar a = BankAccount.new()\n\treturn a[\"balance\"]\n");
	assert(count_dict_gets(find_function(by_key, "test")) == 1);

	std::cout << "  ✓ an unknown field is rejected" << std::endl;
}

static void test_subscript_answers_for_the_name() {
	std::cout << "Testing that a constant subscript is field-checked..." << std::endl;

	assert(rejects(BANK_ACCOUNT +
		"func test():\n\tvar a = BankAccount.new()\n\treturn a[\"blance\"]\n"));
	assert(rejects(BANK_ACCOUNT +
		"func test():\n\tvar a = BankAccount.new()\n\ta[\"blance\"] = 1\n\treturn a\n"));
	assert(rejects(BANK_ACCOUNT +
		"func test():\n\tvar a = BankAccount.new()\n\ta[\"blance\"] += 1\n\treturn a\n"));
	assert(rejects(BANK_ACCOUNT +
		"func test(a: BankAccount):\n\treturn a[\"blance\"]\n"));
	assert(rejects(BANK_ACCOUNT +
		"var vault: BankAccount\n\nfunc test():\n\treturn vault[\"blance\"]\n"));
	assert(rejects(BANK_ACCOUNT +
		"func make() -> BankAccount:\n\treturn BankAccount.new()\n"
		"\nfunc test():\n\treturn make()[\"blance\"]\n"));

	assert(rejects("const K = \"blance\"\n\n" + BANK_ACCOUNT +
		"func test():\n\tvar a = BankAccount.new()\n\treturn a[K]\n"));
	const IRProgram const_key = compile_to_ir("const K = \"balance\"\n\n" + BANK_ACCOUNT +
		"func test():\n\tvar a = BankAccount.new()\n\treturn a[K]\n");
	assert(count_dict_gets(find_function(const_key, "test")) == 1);
	const IRProgram shadowed = compile_to_ir("const K = \"blance\"\n\n" + BANK_ACCOUNT +
		"func test():\n\tvar K = \"balance\"\n\tvar a = BankAccount.new()\n\treturn a[K]\n");
	assert(count_dict_gets(find_function(shadowed, "test")) == 1);

	const IRProgram computed = compile_to_ir(BANK_ACCOUNT +
		"func test(k):\n\tvar a = BankAccount.new()\n\ta[k] = 1\n\treturn a\n");
	assert(count_opcode(find_function(computed, "test"), IROpcode::DICT_SET) == 1);

	const IRProgram plain = compile_to_ir(
		"func test():\n\tvar d = {}\n\td[\"anything\"] = 1\n\treturn d[\"anything\"]\n");
	assert(count_dict_gets(find_function(plain, "test")) == 1);

	std::cout << "  ✓ a constant subscript is field-checked" << std::endl;
}

static void test_struct_is_a_type_not_a_value() {
	std::cout << "Testing that a struct name is a type, not a value..." << std::endl;

	assert(rejects(BANK_ACCOUNT + "func test():\n\treturn BankAccount\n"));
	// Two structs of the same name, a struct named after a Godot singleton, a
	// struct named after a function, and a global named after a struct: each
	// would make one of the two names unreachable.
	assert(rejects("struct S:\n\tvar x = 1\n\nstruct S:\n\tvar y = 2\n"));
	assert(rejects("struct Engine:\n\tvar x = 1\n"));
	assert(rejects("struct S:\n\tvar x = 1\n\nfunc S():\n\treturn 1\n"));
	assert(rejects("struct S:\n\tvar x = 1\n\nvar S = 1\n"));

	std::cout << "  ✓ a struct name is a type, not a value" << std::endl;
}

static void test_declared_field_types() {
	std::cout << "Testing declared field types..." << std::endl;

	// A typed field with no value gets the default of its type, the way a typed
	// variable with no initializer does.
	const IRProgram defaults = compile_to_ir(
		"struct S:\n"
		"\tvar count: int\n"
		"\tvar ratio: float\n"
		"\tvar name: String\n"
		"\tvar items: Array\n"
		"\tvar flag: bool\n"
		"\tvar anything\n"
		"\n"
		"func test():\n\treturn S.new()\n");
	const IRFunction& test = find_function(defaults, "test");
	assert((dictionary_keys(defaults, test) ==
		std::vector<std::string>{ "count", "ratio", "name", "items", "flag", "anything" }));
	assert(count_opcode(test, IROpcode::LOAD_FLOAT_IMM) == 1);
	assert(count_opcode(test, IROpcode::MAKE_ARRAY) == 1);

	// GDScript's one implicit numeric conversion applies to a field's declared
	// type, both in the declaration and at the assignment.
	const IRProgram converts = compile_to_ir(
		"struct S:\n\tvar ratio: float = 0\n"
		"\n"
		"func test():\n\tvar s = S.new()\n\ts.ratio = 3\n\treturn s.ratio\n");
	assert(count_opcode(find_function(converts, "test"), IROpcode::CONVERT) == 2);

	// Anything else that GDScript rejects is rejected here too, rather than
	// reinterpreted: the backend would read the payload as the declared type.
	assert(rejects("struct S:\n\tvar count: int = 0\n"
		"\n"
		"func test():\n\tvar s = S.new()\n\ts.count = 1.5\n\treturn s\n"));
	assert(rejects("struct S:\n\tvar count: int = 0\n"
		"\n"
		"func test():\n\treturn S.new(1.5)\n"));

	std::cout << "  ✓ declared field types" << std::endl;
}

static void test_nested_structs() {
	std::cout << "Testing nested structs..." << std::endl;

	const std::string source =
		"struct Inner:\n"
		"\tvar x = 1\n"
		"\tvar y = 2\n"
		"\n"
		"struct Outer:\n"
		"\tvar inner: Inner\n"
		"\tvar tag = 0\n"
		"\n"
		"func test():\n"
		"\tvar o = Outer.new()\n"
		"\treturn o.inner.x\n";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction& test = find_function(ir, "test");

	// A field declared as another struct defaults to an instance of it, so both
	// Dictionaries are built.
	assert(count_opcode(test, IROpcode::MAKE_DICTIONARY) == 2);
	// Reading through the nesting stays on the element path the whole way.
	assert(count_opcode(test, IROpcode::VGET) == 0);
	assert(count_dict_gets(test) == 2);
	// And the field of the nested struct is checked against Inner, not Outer.
	assert(rejects(
		"struct Inner:\n\tvar x = 1\n"
		"\n"
		"struct Outer:\n\tvar inner: Inner\n"
		"\n"
		"func test():\n\tvar o = Outer.new()\n\treturn o.inner.tag\n"));

	// A struct that holds itself by value has no finite default, so it is
	// reported instead of recursing until the compiler runs out of stack.
	assert(rejects("struct Chain:\n\tvar next: Chain\n\nfunc test():\n\treturn Chain.new()\n"));
	// Giving the field a value breaks the cycle.
	compile_to_ir("struct Chain:\n\tvar next: Chain = null\n"
		"\n"
		"func test():\n\treturn Chain.new()\n");

	std::cout << "  ✓ nested structs" << std::endl;
}

static void test_struct_type_hints() {
	std::cout << "Testing struct type hints on variables..." << std::endl;

	// `var a: BankAccount` with no initializer is a fresh instance, the way
	// `var a: Array` is an empty Array.
	const IRProgram bare = compile_to_ir(BANK_ACCOUNT +
		"func test():\n\tvar account: BankAccount\n\treturn account.balance\n");
	const IRFunction& test = find_function(bare, "test");
	assert(count_opcode(test, IROpcode::MAKE_DICTIONARY) == 1);
	assert((dictionary_keys(bare, test) == std::vector<std::string>{ "balance", "loan" }));

	// The annotation is checked against what the initializer actually is.
	compile_to_ir(BANK_ACCOUNT +
		"func test():\n\tvar account: BankAccount = BankAccount.new()\n\treturn account\n");
	assert(rejects(BANK_ACCOUNT +
		"struct Other:\n\tvar x = 1\n"
		"\n"
		"func test():\n\tvar account: BankAccount = Other.new()\n\treturn account\n"));
	assert(rejects(BANK_ACCOUNT +
		"func test():\n\tvar account: BankAccount = 5\n\treturn account\n"));

	std::cout << "  ✓ struct type hints on variables" << std::endl;
}

static void test_struct_globals() {
	std::cout << "Testing struct globals..." << std::endl;

	// A struct-typed global with no initializer is built by the init function,
	// because a Dictionary has no compile-time representation to write into the
	// globals array.
	const IRProgram typed = compile_to_ir(BANK_ACCOUNT +
		"var account: BankAccount\n"
		"\n"
		"func test():\n\taccount.balance = 1\n\treturn account.balance\n");
	// A plain `var` is a member, so its initializer runs per instance.
	assert(typed.has_member_init);
	assert(count_opcode(typed.member_init, IROpcode::MAKE_DICTIONARY) == 1);
	assert(typed.globals.at(0).type_hint == Variant::DICTIONARY);

	const IRFunction& test = find_function(typed, "test");
	assert(count_opcode(test, IROpcode::VGET) == 0);
	assert(count_opcode(test, IROpcode::VSET) == 0);
	assert(count_dict_gets(test) == 1);
	assert(count_opcode(test, IROpcode::DICT_SET) == 1);

	// Which struct a global holds is also taken from its initializer, which is
	// how a global is usually written.
	const IRProgram inferred = compile_to_ir(BANK_ACCOUNT +
		"var account = BankAccount.new(7)\n"
		"\n"
		"func test():\n\treturn account.balance\n");
	assert(count_opcode(find_function(inferred, "test"), IROpcode::VGET) == 0);
	assert(count_dict_gets(find_function(inferred, "test")) == 1);
	assert(rejects(BANK_ACCOUNT +
		"var account = BankAccount.new()\n"
		"\n"
		"func test():\n\treturn account.blance\n"));

	std::cout << "  ✓ struct globals" << std::endl;
}

static void test_compound_assignment_to_a_field() {
	std::cout << "Testing compound assignment to a field..." << std::endl;

	// `a.balance += 5` is the natural way to write this, and rewrites to
	// `a.balance = a.balance + 5`: one read, one add, one write.
	const IRProgram ir = compile_to_ir(BANK_ACCOUNT +
		"func test():\n"
		"\tvar account = BankAccount.new()\n"
		"\taccount.balance += 5\n"
		"\treturn account.balance\n");
	const IRFunction& test = find_function(ir, "test");
	assert(count_opcode(test, IROpcode::DICT_SET) == 1);
	assert(count_dict_gets(test) == 2);
	assert(count_opcode(test, IROpcode::ADD) == 1);

	// The rewrite needs a second copy of the target, so it is offered only for
	// the targets that can be rebuilt without evaluating anything twice.
	assert(rejects(BANK_ACCOUNT +
		"func open() -> BankAccount:\n\treturn BankAccount.new()\n"
		"\n"
		"func test():\n\topen().balance += 5\n\treturn 1\n"));
	// An unknown field is still rejected through this path.
	assert(rejects(BANK_ACCOUNT +
		"func test():\n\tvar a = BankAccount.new()\n\ta.blance += 5\n\treturn a\n"));

	std::cout << "  ✓ compound assignment to a field" << std::endl;
}

static void test_dictionary_member_access() {
	std::cout << "Testing member access on a plain Dictionary..." << std::endl;

	// In GDScript `d.key` is `d["key"]`. The property syscall the member path
	// used to take works on an Object and throws on a Dictionary, so a value
	// the compiler knows to be a Dictionary takes the element path.
	const IRProgram ir = compile_to_ir(
		"func test():\n"
		"\tvar d = {\"a\": 1}\n"
		"\td.a = 5\n"
		"\treturn d.a\n");
	const IRFunction& test = find_function(ir, "test");
	assert(count_opcode(test, IROpcode::VGET) == 0);
	assert(count_opcode(test, IROpcode::VSET) == 0);
	assert(count_dict_gets(test) == 1);
	assert(count_opcode(test, IROpcode::DICT_SET) == 1);

	// A Dictionary has no declared keys, so any name is allowed on one.
	compile_to_ir("func test():\n\tvar d = {}\n\td.anything = 1\n\treturn d.anything\n");

	// An object property is still a property: nothing here changes that path.
	const IRProgram object = compile_to_ir("func test():\n\treturn self.name\n");
	assert(count_opcode(find_function(object, "test"), IROpcode::VGET) == 1);

	std::cout << "  ✓ member access on a plain Dictionary" << std::endl;
}

static void test_an_untracked_instance_still_reaches_its_fields() {
	std::cout << "Testing member access on a value of unknown type..." << std::endl;

	// Untracked struct: tag branches to element read/write, VGET/VSET fallback for Objects.
	const IRProgram ir = compile_to_ir(BANK_ACCOUNT +
		"func test(account):\n"
		"\treturn account.balance\n");
	const IRFunction& test = find_function(ir, "test");
	assert(count_opcode(test, IROpcode::TYPE_TEST) == 1);
	assert(count_dict_gets(test) == 1);
	// Object fallback survives: type unknown, could be either.
	assert(count_opcode(test, IROpcode::VGET) == 1);

	// The same for a write.
	const IRProgram write = compile_to_ir(BANK_ACCOUNT +
		"func test(account):\n"
		"\taccount.balance = 5\n");
	const IRFunction& w = find_function(write, "test");
	assert(count_opcode(w, IROpcode::TYPE_TEST) == 1);
	assert(count_opcode(w, IROpcode::DICT_SET) == 1);
	assert(count_opcode(w, IROpcode::VSET) == 1);

	// Out of an Array, which carries no element type.
	const IRProgram walked = compile_to_ir(BANK_ACCOUNT +
		"func test():\n"
		"\tvar total = 0\n"
		"\tfor account in [BankAccount.new(100), BankAccount.new(50)]:\n"
		"\t\ttotal += account.balance\n"
		"\treturn total\n");
	const IRFunction& walk = find_function(walked, "test");
	assert(count_dict_gets(walk) == 1);
	assert(count_opcode(walk, IROpcode::TYPE_TEST) == 1);

	// Unknown type: no struct to validate against, same as bare Dictionary.
	compile_to_ir(BANK_ACCOUNT + "func test(account):\n\treturn account.blance\n");

	std::cout << "  ✓ member access on a value of unknown type" << std::endl;
}

static void test_struct_program_reaches_riscv() {
	std::cout << "Testing that a struct program compiles to RISC-V..." << std::endl;

	// The whole pipeline, including the optimizer -- which runs the IR verifier
	// between passes in a debug build -- and the backend.
	const std::string source = BANK_ACCOUNT +
		"var vault: BankAccount\n"
		"\n"
		"func deposit(account: BankAccount, amount):\n"
		"\taccount.balance += amount\n"
		"\treturn account.balance\n"
		"\n"
		"func test():\n"
		"\tvar account = BankAccount.new(balance = 100, loan = 50)\n"
		"\tdeposit(account, 5)\n"
		"\tvault.loan = account.loan\n"
		"\treturn account.balance + vault.loan\n";

	IRProgram ir = compile_to_ir(source, true);
	RISCVCodeGen riscv{ VariantLayout(false) };
	const std::vector<uint8_t> code = riscv.generate(ir);
	assert(!code.empty());
	assert(code.size() % 2 == 0);

	std::cout << "  ✓ a struct program compiles to RISC-V" << std::endl;
}

int main() {
	std::cout << "=== Struct Tests ===" << std::endl << std::endl;

	test_struct_declaration_parses();
	test_struct_body_holds_only_fields();
	test_construction_builds_a_dictionary();
	test_both_constructor_forms();
	test_named_arguments();
	test_field_access_is_dictionary_access();
	test_unknown_field_is_rejected();
	test_subscript_answers_for_the_name();
	test_struct_is_a_type_not_a_value();
	test_declared_field_types();
	test_nested_structs();
	test_struct_type_hints();
	test_struct_globals();
	test_compound_assignment_to_a_field();
	test_dictionary_member_access();
	test_an_untracked_instance_still_reaches_its_fields();
	test_struct_program_reaches_riscv();

	std::cout << std::endl << "All struct tests passed!" << std::endl;
	return 0;
}
