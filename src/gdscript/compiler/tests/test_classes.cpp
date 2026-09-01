#include "../chain.h"
#include "../codegen.h"
#include "../compiler.h"
#include "../compiler_exception.h"
#include "../ir_optimizer.h"
#include "../lexer.h"
#include "../parser.h"
#include "../syscall_numbers.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
	if (condition) {
		return;
	}
	std::cerr << "FAILED: " << what << std::endl;
	failures++;
}

IRProgram compile_to_ir(const std::string& source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	return codegen.generate(program);
}

std::string compile_error(const std::string& source) {
	Compiler compiler;
	CompilerOptions options;
	if (!compiler.compile(source, options).empty()) {
		return "";
	}
	return compiler.get_error();
}

std::string restricted_error(const std::string& source) {
	Compiler compiler;
	CompilerOptions options;
	options.restricted = true;
	if (!compiler.compile(source, options).empty()) {
		return "";
	}
	return compiler.get_error();
}

IRProgram compile_to_ir_restricted(const std::string& source, bool restricted) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	codegen.set_restricted(restricted);
	return codegen.generate(program);
}

std::vector<uint8_t> compile(const std::string& source) {
	Compiler compiler;
	CompilerOptions options;
	std::vector<uint8_t> elf = compiler.compile(source, options);
	if (elf.empty()) {
		std::cerr << "FAILED to compile: " << compiler.get_error() << std::endl;
		failures++;
	}
	return elf;
}

const IRFunction* find_function(const IRProgram& ir, const std::string& name) {
	for (const IRFunction& func : ir.functions) {
		if (func.name == name) {
			return &func;
		}
	}
	return nullptr;
}

std::vector<std::string> called_names(const IRProgram& ir, const IRFunction& func) {
	std::vector<std::string> names;
	for (const IRInstruction& instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL) {
			names.push_back(ir.strings[instr.operands[0].string_id]);
		}
	}
	return names;
}

int count_opcode(const IRFunction& func, IROpcode opcode) {
	int count = 0;
	for (const IRInstruction& instr : func.instructions) {
		if (instr.opcode == opcode) {
			count++;
		}
	}
	return count;
}

const char* CHAIN =
	"class Base:\n"
	"\tvar v = 1\n"
	"\tfunc _init(a = 10):\n"
	"\t\tv = a\n"
	"\tfunc greet(x):\n"
	"\t\treturn x + v\n"
	"\tfunc who():\n"
	"\t\treturn 1\n"
	"class Derived extends Base:\n"
	"\tvar extra = 5\n"
	"\tfunc _init():\n"
	"\t\tsuper(42)\n"
	"\tfunc greet(x):\n"
	"\t\treturn super.greet(x) * 2\n"
	"func test():\n"
	"\tvar d = Derived.new()\n"
	"\treturn d.greet(7)\n";

void test_a_method_is_a_lifted_function() {
	std::cout << "Testing what a method becomes..." << std::endl;

	const IRProgram ir = compile_to_ir(CHAIN);

	const IRFunction* greet = find_function(ir, "@Base.greet");
	check(greet != nullptr, "a method is lifted to @Class.method");
	if (greet != nullptr) {
		check(greet->parameters.size() == 2 && greet->parameters[0] == "self",
			"the instance travels in the first parameter slot");
		check(greet->parameters[1] == "x", "the declared parameters follow it");
	}

	for (const IRFunction& func : ir.functions) {
		check(func.name.find('.') == std::string::npos || func.name[0] == '@',
			"a lifted method is named with a leading '@': " + func.name);
	}

	check(ir.signatures.size() == ir.functions.size(), "one signature per function");

	std::cout << "  ✓ A method is a function taking the instance first" << std::endl;
}

void test_the_instance_is_a_dictionary() {
	std::cout << "Testing what new() builds..." << std::endl;

	const IRProgram ir = compile_to_ir(CHAIN);
	const IRFunction* test = find_function(ir, "test");
	check(test != nullptr, "test() is generated");
	if (test == nullptr) {
		return;
	}

	check(count_opcode(*test, IROpcode::MAKE_DICTIONARY) == 1,
		"new() builds one Dictionary");

	for (const IRInstruction& instr : test->instructions) {
		if (instr.opcode != IROpcode::MAKE_DICTIONARY) {
			continue;
		}
		check(instr.operands[1].immediate() == 3,
			"an instance of Derived holds v, extra and the class it was made from");
	}

	const std::vector<std::string> calls = called_names(ir, *test);
	check(calls.size() == 2 && calls[0] == "@Derived._init" && calls[1] == "@Derived.greet",
		"new() calls _init, then the method the call site names");

	std::cout << "  ✓ An instance is a Dictionary with one key per field" << std::endl;
}

void test_a_method_call_is_a_direct_call() {
	std::cout << "Testing how a method is reached..." << std::endl;

	const IRProgram ir = compile_to_ir(CHAIN);
	const IRFunction* test = find_function(ir, "test");
	if (test != nullptr) {
		check(count_opcode(*test, IROpcode::VCALL) == 0, "no VCALL reaches a class method");
	}

	const IRProgram inherited = compile_to_ir(CHAIN);
	const IRFunction* derived_greet = find_function(inherited, "@Derived.greet");
	check(derived_greet != nullptr, "Derived declares its own greet");
	if (derived_greet != nullptr) {
		const std::vector<std::string> calls = called_names(inherited, *derived_greet);
		check(calls.size() == 1 && calls[0] == "@Base.greet",
			"super.greet() calls the base's greet");
	}

	const IRFunction* derived_init = find_function(inherited, "@Derived._init");
	if (derived_init != nullptr) {
		const std::vector<std::string> calls = called_names(inherited, *derived_init);
		check(calls.size() == 1 && calls[0] == "@Base._init",
			"super(42) calls the base's _init");
	}

	const IRProgram uninherited = compile_to_ir(CHAIN);
	const IRFunction* uninherited_test = find_function(uninherited, "test");
	(void)uninherited_test;
	const IRProgram calls_who = compile_to_ir(
		std::string(CHAIN) + "func other():\n\tvar d = Derived.new()\n\treturn d.who()\n");
	const IRFunction* other = find_function(calls_who, "other");
	if (other != nullptr) {
		const std::vector<std::string> calls = called_names(calls_who, *other);
		check(calls.size() == 2 && calls[1] == "@Base.who",
			"an inherited method is called on the class that declares it");
	}

	std::cout << "  ✓ A method call is a direct call to the lifted function" << std::endl;
}

void test_a_field_is_the_instance_dictionary() {
	std::cout << "Testing what a field name reaches..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"class C:\n"
		"\tvar n = 0\n"
		"\tfunc bump(k = 1):\n"
		"\t\tn += k\n"
		"\t\treturn n\n"
		"func test():\n"
		"\tvar c = C.new()\n"
		"\tc.n = 3\n"
		"\treturn c.bump()\n");

	const IRFunction* bump = find_function(ir, "@C.bump");
	check(bump != nullptr, "the method is lifted");
	if (bump != nullptr) {
		check(count_opcode(*bump, IROpcode::VGET) == 0 &&
			count_opcode(*bump, IROpcode::VSET) == 0,
			"a field is not reached through the property syscalls");
		check(count_opcode(*bump, IROpcode::DICT_SET_CONST) == 1, "n += k writes the Dictionary");
	}

	const IRFunction* test = find_function(ir, "test");
	if (test != nullptr) {
		check(count_opcode(*test, IROpcode::DICT_SET_CONST) == 1, "c.n = 3 writes the Dictionary");
		check(count_opcode(*test, IROpcode::VSET) == 0, "and not the property syscall");
	}

	const IRProgram shadowing_a_global = compile_to_ir(
		"var n = 100\n"
		"class C:\n"
		"\tvar n = 0\n"
		"\tfunc bump():\n"
		"\t\tn = n + 1\n"
		"\t\treturn n\n"
		"func test():\n"
		"\treturn C.new().bump()\n");
	const IRFunction* over_global = find_function(shadowing_a_global, "@C.bump");
	check(over_global != nullptr, "the method is lifted");
	if (over_global != nullptr) {
		check(count_opcode(*over_global, IROpcode::LOAD_GLOBAL) == 0 &&
			count_opcode(*over_global, IROpcode::STORE_GLOBAL) == 0,
			"a field does not reach the script's global of the same name");
		check(count_opcode(*over_global, IROpcode::DICT_SET_CONST) == 1,
			"the write lands on the instance");
	}

	const IRProgram shadowed = compile_to_ir(
		"class C:\n"
		"\tvar n = 0\n"
		"\tfunc f():\n"
		"\t\tvar n = 7\n"
		"\t\treturn n\n"
		"func test():\n"
		"\treturn C.new().f()\n");
	const IRFunction* f = find_function(shadowed, "@C.f");
	check(f != nullptr && count_opcode(*f, IROpcode::CALL_SYSCALL) == 0,
		"a local shadows the field of the same name");

	std::cout << "  ✓ A field is one key of the instance Dictionary" << std::endl;
}

void test_super_at_script_level() {
	std::cout << "Testing super where the base is a native class..." << std::endl;

	const std::vector<uint8_t> with_super = compile(
		"func f():\n"
		"\treturn super.get_index()\n");
	const std::vector<uint8_t> with_self = compile(
		"func f():\n"
		"\treturn self.get_index()\n");
	check(!with_super.empty() && with_super == with_self,
		"super.m() and self.m() compile alike at script level");

	const std::string init = compile_error(
		"func _init():\n"
		"\tsuper()\n");
	check(init.find("no base") != std::string::npos,
		"super() at script level is refused: " + init);

	check(compile_error(
		"func f(super):\n"
		"\treturn super\n").empty(),
		"a parameter named super is still a parameter");

	std::cout << "  ✓ super at script level is the self-call it stands for" << std::endl;
}

void test_self_local_call_is_direct_and_typed() {
	std::cout << "Testing self.local() direct dispatch..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"func increment(x : int) -> int:\n"
		"\treturn x + 1\n"
		"func test(x : int) -> int:\n"
		"\treturn self.increment(x)\n");
	const IRFunction* test = find_function(ir, "test");
	const IRFunction* increment = find_function(ir, "increment");
	check(test != nullptr && count_opcode(*test, IROpcode::CALL) == 1,
		"self.increment() is a guest CALL");
	check(test != nullptr && count_opcode(*test, IROpcode::GET_NODE) == 0 &&
		count_opcode(*test, IROpcode::VCALL) == 0,
		"self.increment() needs neither GET_NODE nor VCALL");
	bool trusted = false;
	if (test != nullptr) {
		for (const IRInstruction& instr : test->instructions) {
			trusted = trusted || (instr.opcode == IROpcode::CALL && instr.trusted_internal_call);
		}
	}
	check(trusted, "an exactly typed local call uses the trusted entry");
	check(increment != nullptr && count_opcode(*increment, IROpcode::COERCE) == 1,
		"the public entry retains host-side coercion");

	std::cout << "  ✓ self.local() is a trusted guest call" << std::endl;
}

void test_what_is_refused() {
	std::cout << "Testing the refusals..." << std::endl;

	const std::string singleton_base = compile_error(
		"class A extends Engine:\n"
		"\tvar x = 1\n"
		"func test():\n\treturn 1\n");
	check(singleton_base.find("singleton") != std::string::npos,
		"extending a singleton is refused: " + singleton_base);

	const std::string extends_struct = compile_error(
		"struct S:\n"
		"\tvar x = 1\n"
		"class A extends S:\n"
		"\tvar y = 2\n"
		"func test():\n\treturn 1\n");
	check(extends_struct.find("extends struct") != std::string::npos,
		"extending a struct is refused: " + extends_struct);

	const std::string cycle = compile_error(
		"class A extends B:\n"
		"\tvar x = 1\n"
		"class B extends A:\n"
		"\tvar y = 2\n"
		"func test():\n\treturn 1\n");
	check(cycle.find("extends itself") != std::string::npos,
		"a cycle in the chain is refused: " + cycle);

	const std::string redeclared = compile_error(
		"class A:\n"
		"\tvar x = 1\n"
		"class B extends A:\n"
		"\tvar x = 2\n"
		"func test():\n\treturn 1\n");
	check(redeclared.find("redeclares field") != std::string::npos,
		"a field the base already declares is refused: " + redeclared);

	const std::string no_init = compile_error(
		"class A:\n"
		"\tvar x = 1\n"
		"func test():\n"
		"\treturn A.new(5)\n");
	check(no_init.find("no _init()") != std::string::npos,
		"arguments to a class with no _init are refused: " + no_init);

	const std::string arity = compile_error(
		"class A:\n"
		"\tfunc _init(a):\n"
		"\t\tpass\n"
		"func test():\n"
		"\treturn A.new(1, 2)\n");
	check(arity.find("Too many arguments") != std::string::npos,
		"too many arguments to _init are refused: " + arity);

	const std::string no_method = compile_error(
		"class A:\n"
		"\tfunc f():\n"
		"\t\treturn 1\n"
		"class B extends A:\n"
		"\tfunc g():\n"
		"\t\treturn super.nope()\n"
		"func test():\n\treturn 1\n");
	check(no_method.find("declares no") != std::string::npos,
		"a super call the base cannot answer is refused: " + no_method);

	const std::string no_base = compile_error(
		"class A:\n"
		"\tfunc f():\n"
		"\t\treturn super.f()\n"
		"func test():\n\treturn 1\n");
	check(no_base.find("extends nothing") != std::string::npos,
		"super in a class with no base is refused: " + no_base);

	const std::string duplicate = compile_error(
		"class A:\n"
		"\tfunc f():\n"
		"\t\treturn 1\n"
		"\tfunc f():\n"
		"\t\treturn 2\n"
		"func test():\n\treturn 1\n");
	check(duplicate.find("more than once") != std::string::npos,
		"a method declared twice is refused: " + duplicate);

	const std::string slots = compile_error(
		"class A:\n"
		"\tfunc f(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p):\n"
		"\t\treturn a\n"
		"func test():\n\treturn 1\n");
	check(slots.find("at most 15") != std::string::npos,
		"a method needing seventeen slots is refused: " + slots);

	const std::string body = compile_error(
		"class A:\n"
		"\tsignal boom\n"
		"func test():\n\treturn 1\n");
	check(body.find("constant, field and function") != std::string::npos,
		"what a class body does not hold is refused: " + body);

	std::cout << "  ✓ What cannot work is refused at compile time" << std::endl;
}

void test_a_class_type_travels() {
	std::cout << "Testing where the class of a value is known..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"class C:\n"
		"\tvar n = 0\n"
		"\tfunc get_n():\n"
		"\t\treturn n\n"
		"func make() -> C:\n"
		"\treturn C.new()\n"
		"func from_return():\n"
		"\treturn make().get_n()\n"
		"func from_parameter(c: C):\n"
		"\treturn c.get_n()\n"
		"func from_hint():\n"
		"\tvar c: C = C.new()\n"
		"\treturn c.get_n()\n");

	for (const char* name : { "from_return", "from_parameter", "from_hint" }) {
		const IRFunction* func = find_function(ir, name);
		check(func != nullptr, std::string("function ") + name + " is generated");
		if (func == nullptr) {
			continue;
		}
		const std::vector<std::string> calls = called_names(ir, *func);
		check(std::find(calls.begin(), calls.end(), "@C.get_n") != calls.end(),
			std::string(name) + " reaches the method directly");
		check(count_opcode(*func, IROpcode::VCALL) == 0,
			std::string(name) + " makes no VCALL");
	}

	const IRProgram untyped = compile_to_ir(
		"class C:\n"
		"\tvar n = 0\n"
		"\tfunc get_n():\n"
		"\t\treturn n\n"
		"func f(c):\n"
		"\treturn c.get_n()\n");
	const IRFunction* f = find_function(untyped, "f");
	check(f != nullptr && count_opcode(*f, IROpcode::VCALL) == 1,
		"an untyped receiver still goes through VCALL");

	std::cout << "  ✓ A class travels the way a struct's does" << std::endl;
}

void test_a_lambda_in_a_method_sees_the_class() {
	std::cout << "Testing a lambda declared in a class method..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"var value = 999\n"
		"class Counter:\n"
		"\tvar value = 7\n"
		"\tfunc read():\n"
		"\t\tvar f = func(): return value\n"
		"\t\treturn f.call()\n"
		"func test():\n"
		"\treturn Counter.new().read()\n");

	const IRFunction* lambda = find_function(ir, "@lambda_0");
	check(lambda != nullptr, "the lambda was lifted");
	if (lambda != nullptr) {
		check(count_opcode(*lambda, IROpcode::LOAD_GLOBAL) == 0,
			"a field name in the lambda does not reach the global");
		check(count_opcode(*lambda, IROpcode::ARRAY_GET) >= 1,
			"the lambda reads the instance out of its captures");
	}

	const IRFunction* read = find_function(ir, "@Counter.read");
	check(read != nullptr && count_opcode(*read, IROpcode::MAKE_ARRAY) == 1,
		"the method builds a capture array for the lambda");

	const IRProgram method_call = compile_to_ir(
		"class Counter:\n"
		"\tvar n = 1\n"
		"\tfunc bump(): return n + 1\n"
		"\tfunc run():\n"
		"\t\tvar f = func(): return bump()\n"
		"\t\treturn f.call()\n"
		"func test():\n"
		"\treturn Counter.new().run()\n");
	const IRFunction* run = find_function(method_call, "@Counter.run");
	check(run != nullptr && count_opcode(*run, IROpcode::MAKE_ARRAY) == 1,
		"a bare method call in a lambda captures the instance too");

	const IRProgram free_lambda = compile_to_ir(
		"var value = 999\n"
		"func test():\n"
		"\tvar f = func(): return value\n"
		"\treturn f.call()\n");
	const IRFunction* free_fn = find_function(free_lambda, "@lambda_0");
	check(free_fn != nullptr && count_opcode(*free_fn, IROpcode::LOAD_GLOBAL) == 1,
		"a lambda outside a class still reads the global live");

	std::cout << "  ✓ A lambda in a method resolves fields against its class" << std::endl;
}

void test_a_qualified_base_is_refused() {
	std::cout << "Testing a qualified base class..." << std::endl;

	const std::string error = compile_error(
		"class A:\n\tvar x = 1\n"
		"class C extends A.B:\n\tvar y = 2\n"
		"func test():\n\treturn 1\n");
	check(error.find("cannot extend 'A.B'") != std::string::npos,
		"a qualified base names the whole path it refused: " + error);

	const std::string plain = compile_error(
		"class A:\n\tvar x = 1\n"
		"class C extends A:\n\tvar y = 2\n"
		"func test():\n\treturn C.new().x\n");
	check(plain.empty(), "an unqualified base still resolves: " + plain);

	std::cout << "  ✓ A base the compiler cannot resolve is a diagnostic" << std::endl;
}


const char* NATIVE =
	"class Sprite extends Node2D:\n"
	"\tvar hp = 3\n"
	"\tfunc hurt(n):\n"
	"\t\thp -= n\n"
	"\t\tposition = n\n"
	"\t\tmove_local_x(1.0)\n"
	"\t\treturn get_index() + rotation\n"
	"func test():\n"
	"\tvar s = Sprite.new()\n"
	"\treturn s.hurt(1)\n";

int count_syscall(const IRFunction& func, int number) {
	int count = 0;
	for (const IRInstruction& instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() > 1 &&
			instr.operands[1].immediate() == number)
		{
			count++;
		}
	}
	return count;
}

bool vcalls(const IRProgram& ir, const IRFunction& func, const std::string& name) {
	for (const IRInstruction& instr : func.instructions) {
		if (instr.opcode == IROpcode::VCALL &&
			ir.strings[instr.operands[2].string_id] == name)
		{
			return true;
		}
	}
	return false;
}

void test_a_native_base_is_constructed_with_the_instance() {
	std::cout << "Testing a class extending an engine class..." << std::endl;

	const IRProgram ir = compile_to_ir(NATIVE);
	const IRFunction* test = find_function(ir, "test");
	check(test != nullptr, "test() is lowered");
	if (test == nullptr) {
		return;
	}

	check(count_syscall(*test, ECALL_NODE_CREATE) == 1,
		"Sprite.new() instantiates Node2D exactly once");
	const auto& names = ir.string_constants;
	check(std::find(names.begin(), names.end(), std::string("Node2D")) != names.end(),
		"the base class name travels as a string constant");
	check(std::find(names.begin(), names.end(), std::string("@base")) != names.end(),
		"the instance carries the base under a key no field can spell");

	for (const IRInstruction& instr : test->instructions) {
		if (instr.opcode == IROpcode::MAKE_DICTIONARY) {
			check(instr.operands[1].immediate() == 3,
				"the instance Dictionary holds the declared field, the base and the class");
		}
	}

	std::cout << "  ✓ The instance is a Dictionary that carries its engine object"
		<< std::endl;
}

const char* PUBLISHED =
	"class Marker extends Node2D:\n"
	"\tvar hits = 0\n"
	"\tvar tag : String = \"\"\n"
	"\tfunc _init():\n"
	"\t\thits = 1\n"
	"\tfunc launch(by : int, twice = false) -> int:\n"
	"\t\treturn by\n"
	"\tstatic func origin():\n"
	"\t\treturn 0\n"
	"class Plain:\n"
	"\tvar v = 1\n"
	"func test():\n"
	"\tvar m = Marker.new()\n"
	"\tvar p = Plain.new()\n"
	"\treturn m.launch(1)\n";

const gdscript::ClassSignature* find_class(const IRProgram& ir, const std::string& name) {
	for (const ClassSignature& cls : ir.class_signatures) {
		if (cls.name == name) {
			return &cls;
		}
	}
	return nullptr;
}

const FunctionSignature* find_signature(const IRProgram& ir, const std::string& name) {
	for (const FunctionSignature& sig : ir.signatures) {
		if (sig.name == name) {
			return &sig;
		}
	}
	return nullptr;
}

void test_the_bind_syscall_is_emitted_for_an_engine_base() {
	std::cout << "Testing the nested-class bind syscall..." << std::endl;

	const IRProgram ir = compile_to_ir(PUBLISHED);
	const IRFunction* test = find_function(ir, "test");
	check(test != nullptr, "test() is lowered");
	if (test == nullptr) {
		return;
	}

	check(count_syscall(*test, ECALL_CLASS_BIND) == 1,
		"only the class with an engine base is bound");

	// A class is a script instance before its _init() runs, the way add_child(self)
	// inside GDScript's _init() already works.
	size_t bind_at = SIZE_MAX;
	size_t init_at = SIZE_MAX;
	size_t dict_at = SIZE_MAX;
	for (size_t i = 0; i < test->instructions.size(); i++) {
		const IRInstruction& instr = test->instructions[i];
		if (instr.opcode == IROpcode::MAKE_DICTIONARY && dict_at == SIZE_MAX) {
			dict_at = i;
		}
		if (instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() > 1 &&
			instr.operands[1].immediate() == ECALL_CLASS_BIND)
		{
			bind_at = i;
		}
		if (instr.opcode == IROpcode::CALL &&
			ir.strings[instr.operands[0].string_id] == "@Marker._init")
		{
			init_at = i;
		}
	}
	check(dict_at != SIZE_MAX && bind_at != SIZE_MAX && init_at != SIZE_MAX,
		"the dictionary, the bind and _init are all emitted");
	check(dict_at < bind_at && bind_at < init_at,
		"the bind lands after the instance is built and before its _init");

	std::cout << "  \u2713 Only an engine-based class binds, before its _init" << std::endl;
}

// A struct is a value, so declaring one is an instance. A class is an object,
// and GDScript leaves `var a: Inner` null -- checked against the engine.
// Constructing one eagerly also ran its bind while the machine was still loading.
void test_a_class_typed_declaration_constructs_nothing() {
	std::cout << "Testing what a class-typed declaration defaults to..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"class Marker extends Node2D:\n"
		"\tvar hits = 0\n"
		"struct Point:\n"
		"\tvar x = 0\n"
		"var member : Marker\n"
		"func test():\n"
		"\tvar local : Marker\n"
		"\tvar value : Point\n"
		"\treturn 1\n");

	check(ir.has_member_init == false || count_syscall(ir.member_init, ECALL_NODE_CREATE) == 0,
		"a class-typed member builds no engine object at startup");
	check(ir.has_member_init == false || count_syscall(ir.member_init, ECALL_CLASS_BIND) == 0,
		"and asks for no bind while the machine is still loading");

	const IRFunction* test = find_function(ir, "test");
	check(test != nullptr, "test() is lowered");
	if (test == nullptr) {
		return;
	}
	check(count_syscall(*test, ECALL_NODE_CREATE) == 0,
		"a class-typed local constructs nothing either");
	check(count_opcode(*test, IROpcode::MAKE_DICTIONARY_KEYED) == 1,
		"the struct-typed local is still an instance");

	std::cout << "  \u2713 A class-typed declaration is null; a struct-typed one is an instance"
		<< std::endl;
}

void test_class_signatures_are_published() {
	std::cout << "Testing the published class table..." << std::endl;

	const IRProgram ir = compile_to_ir(PUBLISHED);
	check(ir.class_signatures.size() == 1,
		"a class with no engine base has nothing for the host to attach");

	const ClassSignature* marker = find_class(ir, "Marker");
	check(marker != nullptr, "the engine-based class is published");
	if (marker == nullptr) {
		return;
	}
	check(marker->native_base == "Node2D", "the engine class the chain bottoms out in");
	check(marker->base_name.empty(), "no declared parent class");
	check(marker->fields.size() == 2, "both fields travel");
	check(marker->fields[0].name == "hits" && marker->fields[1].name == "tag",
		"fields keep their declaration order");
	check(marker->fields[1].type == int32_t(Variant::STRING), "a declared field type travels");
	check(marker->methods.size() == 3, "every declared method travels");
	check(find_class(ir, "Plain") == nullptr, "a class without an engine base does not");

	bool saw_static = false;
	for (const ClassMethod& method : marker->methods) {
		if (method.name == "origin") {
			saw_static = method.is_static;
		}
	}
	check(saw_static, "a static method is marked, so the host passes it no self");

	// The synthetic self slot is not part of the declaration, so the signature
	// the host checks arity against never mentions it.
	const FunctionSignature* launch = find_signature(ir, "@Marker.launch");
	check(launch != nullptr, "the lifted method has a full signature");
	if (launch == nullptr) {
		return;
	}
	check(launch->parameters.size() == 2, "self is not a declared parameter");
	check(launch->parameters[0].name == "by", "the first parameter is the first declared one");
	check(launch->parameters[0].type == int32_t(Variant::INT), "a declared parameter type travels");
	check(launch->return_type == int32_t(Variant::INT), "the return type travels");
	check(launch->required_arguments == 1, "the parameter with a default is optional");

	// One blob, one entry point: appending a section to the function table would
	// fail to decode against every host built before it existed.
	const std::vector<uint8_t> blob = encode_class_signatures(ir.class_signatures);
	std::vector<ClassSignature> decoded;
	check(decode_class_signatures(blob.data(), blob.size(), decoded),
		"the class table round-trips through its blob");
	check(decoded.size() == 1 && decoded[0].name == "Marker" &&
		decoded[0].native_base == "Node2D" && decoded[0].fields.size() == 2 &&
		decoded[0].methods.size() == 3,
		"the decoded table is what was encoded");

	std::cout << "  \u2713 The host learns the class, its fields and its methods" << std::endl;
}

void test_super_on_a_native_base_is_marked() {
	std::cout << "Testing the super marker on a native base..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"class Marker extends Node2D:\n"
		"\tfunc get_name():\n"
		"\t\treturn super.get_name()\n"
		"\tfunc plain():\n"
		"\t\treturn move_local_x(1.0)\n"
		"func test():\n"
		"\treturn Marker.new()\n");

	const IRFunction* shadowed = find_function(ir, "@Marker.get_name");
	const IRFunction* plain = find_function(ir, "@Marker.plain");
	check(shadowed != nullptr && plain != nullptr, "both methods are lifted");
	if (shadowed == nullptr || plain == nullptr) {
		return;
	}

	// Without the marker the object's own script instance answers first and
	// re-enters the very method that made the call.
	int marked = 0;
	int unmarked = 0;
	for (const IRInstruction& instr : shadowed->instructions) {
		if (instr.opcode == IROpcode::VCALL) {
			instr.super_call ? marked++ : unmarked++;
		}
	}
	check(marked == 1 && unmarked == 0, "super. on the native base is marked");

	for (const IRInstruction& instr : plain->instructions) {
		if (instr.opcode == IROpcode::VCALL) {
			check(!instr.super_call, "a name the class does not declare needs no bypass");
		}
	}

	std::cout << "  \u2713 Only a super call on the native base asks for the bypass"
		<< std::endl;
}

void test_what_the_class_does_not_declare_reaches_the_base() {
	std::cout << "Testing fallthrough to the native base..." << std::endl;

	const IRProgram ir = compile_to_ir(NATIVE);
	const IRFunction* hurt = find_function(ir, "@Sprite.hurt");
	check(hurt != nullptr, "the method is lifted");
	if (hurt == nullptr) {
		return;
	}

	check(count_opcode(*hurt, IROpcode::DICT_SET_CONST) == 1,
		"'hp' is the instance Dictionary, not a property of the base");

	check(count_opcode(*hurt, IROpcode::VSET) == 1,
		"'position' is a property set on the base");
	check(count_opcode(*hurt, IROpcode::VGET) == 1,
		"'rotation' is a property get on the base");
	check(vcalls(ir, *hurt, "move_local_x"), "an undeclared call goes to the base");
	check(vcalls(ir, *hurt, "get_index"), "so does one whose value is used");
	check(called_names(ir, *hurt).empty(), "neither became a call to a lifted method");

	std::cout << "  ✓ Names the class does not declare are the base's" << std::endl;
}

void test_the_script_still_wins_over_the_base() {
	std::cout << "Testing what shadows the base..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"func helper():\n"
		"\treturn 7\n"
		"class C extends Node2D:\n"
		"\tfunc f():\n"
		"\t\treturn helper()\n"
		"func test():\n"
		"\treturn C.new().f()\n");
	const IRFunction* f = find_function(ir, "@C.f");
	check(f != nullptr && called_names(ir, *f) == std::vector<std::string>{ "helper" },
		"a script function shadows a base method of the same name");

	const IRProgram globals = compile_to_ir(
		"var speed = 4\n"
		"class C extends Node2D:\n"
		"\tfunc f():\n"
		"\t\treturn Vector2(speed, 1)\n"
		"func test():\n"
		"\treturn C.new().f()\n");
	const IRFunction* gf = find_function(globals, "@C.f");
	check(gf != nullptr && count_opcode(*gf, IROpcode::MAKE_VECTOR2) == 1,
		"Vector2() is still built inline, not called on the base");
	check(gf != nullptr && count_opcode(*gf, IROpcode::LOAD_GLOBAL) == 1,
		"a global variable is read as one, not as a base property");

	std::cout << "  ✓ The base is the last name looked up, not the first" << std::endl;
}

void test_super_reaches_the_native_base() {
	std::cout << "Testing super where the base is an engine class..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"class C extends Node2D:\n"
		"\tfunc _init():\n"
		"\t\tsuper()\n"
		"\tfunc get_index():\n"
		"\t\treturn super.get_index() + 1\n"
		"func test():\n"
		"\treturn C.new().get_index()\n");
	const IRFunction* method = find_function(ir, "@C.get_index");
	check(method != nullptr && vcalls(ir, *method, "get_index"),
		"super.get_index() reaches the base rather than recursing");

	const IRFunction* init = find_function(ir, "@C._init");
	check(init != nullptr && count_syscall(*init, ECALL_NODE_CREATE) == 0,
		"super() constructs nothing: the base is already there");

	const std::string with_args = compile_error(
		"class C extends Node2D:\n"
		"\tfunc _init():\n"
		"\t\tsuper(1)\n"
		"func test():\n\treturn 1\n");
	check(with_args.find("takes no arguments") != std::string::npos,
		"super() with arguments is refused: " + with_args);

	const IRProgram chain = compile_to_ir(
		"class A extends Node2D:\n"
		"\tvar x = 1\n"
		"class B extends A:\n"
		"\tfunc f():\n"
		"\t\treturn get_index()\n"
		"func test():\n"
		"\treturn B.new().f()\n");
	const IRFunction* bf = find_function(chain, "@B.f");
	check(bf != nullptr && vcalls(chain, *bf, "get_index"),
		"a class inherits the native base of the class it extends");

	std::cout << "  ✓ super walks the file's chain, then the engine's" << std::endl;
}

void test_class_name_and_extends_are_published() {
	std::cout << "Testing what the script tells the host..." << std::endl;

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = false;
	compiler.compile(
		"class_name Turret\n"
		"extends Node2D\n"
		"func test():\n\treturn 1\n", options);
	check(compiler.get_class_name() == "Turret", "class_name travels beside the ELF");
	check(compiler.get_base_class() == "Node2D", "so does the base");
	check(!compiler.base_is_path(), "a name is not a path");

	Compiler path_compiler;
	path_compiler.compile(
		"extends \"res://base.gd\"\n"
		"func test():\n\treturn 1\n", options);
	check(path_compiler.get_base_class() == "res://base.gd", "a path base is kept as written");
	check(path_compiler.base_is_path(), "and marked as one");

	Compiler dotted;
	dotted.compile("extends Outer.Inner\nfunc test():\n\treturn 1\n", options);
	check(dotted.get_base_class() == "Outer.Inner", "a qualified base keeps its dots");

	Compiler bare;
	bare.compile("func test():\n\treturn 1\n", options);
	check(bare.get_class_name().empty() && bare.get_base_class().empty(),
		"a script that declares neither publishes neither");

	check(compile_error("class_name A\nclass_name B\nfunc test():\n\treturn 1\n")
		.find("one class_name") != std::string::npos,
		"a second class_name is refused");
	check(compile_error("extends Node\nextends Node2D\nfunc test():\n\treturn 1\n")
		.find("extends one base") != std::string::npos,
		"a second extends is refused");

	// Godot answers "Unexpected \"extends\" in class body" to either keyword once
	// anything else has been declared. A var, a const, a class and a func all count.
	check(compile_error("func test():\n\treturn 1\nextends Node\n")
		.find("comes before every other declaration") != std::string::npos,
		"an extends below a function is refused");
	check(compile_error("var x = 1\nclass_name A\nfunc test():\n\treturn 1\n")
		.find("comes before every other declaration") != std::string::npos,
		"and so is a class_name below a global");
	check(compile_error("@tool\nextends Node\nclass_name A\nfunc test():\n\treturn 1\n").empty(),
		"a file-level annotation is not a declaration, and the two head in either order");

	std::cout << "  ✓ Neither reaches the machine code; both reach the host" << std::endl;
}

void test_restrictions_refuse_what_needs_a_class() {
	std::cout << "Testing the restricted dialect..." << std::endl;

	const std::string named = restricted_error(
		"class_name Turret\nfunc test():\n\treturn 1\n");
	check(named.find("allows engine classes") != std::string::npos,
		"class_name is refused under restrictions: " + named);

	const std::string extends = restricted_error(
		"extends Node2D\nfunc test():\n\treturn 1\n");
	check(extends.find("allows engine classes") != std::string::npos,
		"a top-level extends is refused under restrictions: " + extends);

	const std::string native = restricted_error(
		"class C extends Node2D:\n"
		"\tvar x = 1\n"
		"func test():\n\treturn 1\n");
	check(native.find("allows engine classes") != std::string::npos,
		"a native base is refused under restrictions: " + native);

	check(compile_error("class_name Turret\nfunc test():\n\treturn 1\n").empty(),
		"class_name compiles unrestricted");
	check(compile_error("extends Node2D\nfunc test():\n\treturn 1\n").empty(),
		"a top-level extends compiles unrestricted");
	check(compile_error("class C extends Node2D:\n\tvar x = 1\nfunc test():\n\treturn 1\n").empty(),
		"a native base compiles unrestricted");

	check(restricted_error(CHAIN).empty(),
		"a class extending one declared in the file is unaffected");

	Compiler open_compiler;
	Compiler shut_compiler;
	CompilerOptions open_options;
	CompilerOptions shut_options;
	shut_options.restricted = true;
	check(open_compiler.compile(CHAIN, open_options) ==
			shut_compiler.compile(CHAIN, shut_options),
		"and compiles to the same program either way");

	std::cout << "  ✓ What can only work by reaching a class is refused" << std::endl;
}


void test_a_top_level_extends_reaches_the_owner() {
	std::cout << "Testing a bare name under a top-level extends..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"extends Node2D\n"
		"func test():\n"
		"\tposition = Vector2(1, 2)\n"
		"\treturn position\n");
	const IRFunction* test = find_function(ir, "test");
	check(test != nullptr, "test() is lowered");
	if (test != nullptr) {
		check(count_opcode(*test, IROpcode::VSET) == 1,
			"a bare name the script does not declare is written to the owner");
		check(count_opcode(*test, IROpcode::VGET) == 1, "and read from it");
		check(count_opcode(*test, IROpcode::GET_NODE) == 2,
			"the owner is reached the way a bare call reaches it");
	}

	check(!compile_error("func test():\n\tposition = 1\n").empty(),
		"without an extends there is nothing to fall through to");
	check(compile_error(
			"extends \"res://base.gd\"\n"
			"func test():\n"
			"\treturn speed\n").empty(),
		"a path base is still a base");

	// A class of its own has a base of its own; the script's is not it.
	const IRProgram nested = compile_to_ir(
		"extends Node2D\n"
		"class Plain:\n"
		"\tfunc f():\n"
		"\t\treturn 1\n"
		"func test():\n"
		"\treturn Plain.new().f()\n");
	const IRFunction* f = find_function(nested, "@Plain.f");
	check(f != nullptr && count_opcode(*f, IROpcode::VGET) == 0,
		"a class without a base does not borrow the script's owner");

	std::cout << "  ✓ A bare name falls through to what the script extends" << std::endl;
}

void test_an_instance_answers_is_and_as() {
	std::cout << "Testing 'is' and 'as' on a class instance..." << std::endl;

	const IRProgram own = compile_to_ir(
		"class Sprite extends Node2D:\n"
		"\tvar hp = 3\n"
		"class Other extends Node2D:\n"
		"\tvar hp = 3\n"
		"func test():\n"
		"\treturn Sprite.new() is Sprite\n");
	const IRFunction* own_test = find_function(own, "test");
	check(own_test != nullptr && !vcalls(own, *own_test, "is_class"),
		"an instance answers its own class name without asking the engine");

	const IRProgram other = compile_to_ir(
		"class Sprite extends Node2D:\n"
		"\tvar hp = 3\n"
		"class Other extends Node2D:\n"
		"\tvar hp = 3\n"
		"func test():\n"
		"\treturn Sprite.new() is Other\n");
	const IRFunction* other_test = find_function(other, "test");
	check(other_test != nullptr && !vcalls(other, *other_test, "is_class"),
		"and answers a sibling class without asking either");

	const IRProgram base = compile_to_ir(
		"class Sprite extends Node2D:\n"
		"\tvar hp = 3\n"
		"func test():\n"
		"\treturn Sprite.new() is Node\n");
	const IRFunction* base_test = find_function(base, "test");
	check(base_test != nullptr && vcalls(base, *base_test, "is_class"),
		"what the engine knows is asked of the engine");
	check(base_test != nullptr && count_opcode(*base_test, IROpcode::DICT_GET_CONST) >= 1,
		"and it is asked of the object the instance holds, not of the Dictionary");

	const IRProgram inherited = compile_to_ir(
		"class Base extends Node2D:\n"
		"\tvar hp = 3\n"
		"class Derived extends Base:\n"
		"\tvar extra = 1\n"
		"func test():\n"
		"\treturn Derived.new() is Base\n");
	const IRFunction* inherited_test = find_function(inherited, "test");
	check(inherited_test != nullptr && !vcalls(inherited, *inherited_test, "is_class"),
		"a class it derives from is settled by the declaration too");

	// The cast answers the same instance, so what worked before it works after.
	const IRProgram cast = compile_to_ir(
		"class Sprite extends Node2D:\n"
		"\tvar hp = 3\n"
		"func test():\n"
		"\tvar s = Sprite.new() as Node2D\n"
		"\treturn s.hp\n");
	const IRFunction* cast_test = find_function(cast, "test");
	check(cast_test != nullptr && count_opcode(*cast_test, IROpcode::VGET) == 0,
		"a field read after a cast is still the instance Dictionary");

	std::cout << "  ✓ An instance answers for its class and for its base" << std::endl;
}

void test_an_untyped_instance_reaches_the_base() {
	std::cout << "Testing an instance whose type is not tracked..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"class Sprite extends Node2D:\n"
		"\tvar hp = 3\n"
		"func poke(m):\n"
		"\tm.position = Vector2(1, 2)\n"
		"\treturn m.position\n"
		"func test():\n"
		"\treturn poke(Sprite.new())\n");
	const IRFunction* poke = find_function(ir, "poke");
	check(poke != nullptr, "poke() is lowered");
	if (poke != nullptr) {
		check(count_opcode(*poke, IROpcode::VGET) >= 1 && count_opcode(*poke, IROpcode::VSET) >= 1,
			"a Dictionary reaching an untyped '.x' can be an instance, so the base is tried");
	}

	// Nothing to reach means nothing to emit: a script with no such class pays nothing.
	const IRProgram plain = compile_to_ir(
		"func poke(m):\n"
		"\tm.position = Vector2(1, 2)\n"
		"\treturn m.position\n"
		"func test():\n"
		"\treturn poke({})\n");
	const IRFunction* plain_poke = find_function(plain, "poke");
	check(plain_poke != nullptr, "poke() is lowered");
	if (plain_poke != nullptr && poke != nullptr) {
		check(plain_poke->instructions.size() < poke->instructions.size(),
			"a script that declares no engine-based class emits no fallthrough");
	}

	std::cout << "  ✓ An untyped instance still reaches its base" << std::endl;
}

void test_a_class_body_holds_constants_and_static_methods() {
	std::cout << "Testing 'const' and 'static func' in a class body..." << std::endl;

	// A class holds no storage of its own, so a constant folds at the use site and
	// nothing of it reaches the IR -- the same deal the file's own consts get.
	const IRProgram folded = compile_to_ir(
		"class Limits:\n"
		"\tconst MAX = 40\n"
		"\tvar v = MAX\n"
		"func test():\n"
		"\treturn [Limits.MAX, Limits.new().v]\n");
	const IRFunction* folded_test = find_function(folded, "test");
	check(folded_test != nullptr, "test() is lowered");
	if (folded_test != nullptr) {
		int immediates = 0;
		for (const IRInstruction& instr : folded_test->instructions) {
			if (instr.opcode == IROpcode::LOAD_IMM
				&& instr.operands[1].immediate() == 40) {
				immediates++;
			}
		}
		check(immediates == 2,
			"both the qualified name and the field default are the immediate");
		check(count_opcode(*folded_test, IROpcode::LOAD_GLOBAL) == 0,
			"a class constant needs no global slot");
	}

	// A base's constant is inherited, and reached by a bare name in the body.
	const IRProgram inherited = compile_to_ir(
		"class Base:\n"
		"\tconst STEP = 7\n"
		"class Derived extends Base:\n"
		"\tfunc f():\n"
		"\t\treturn STEP\n"
		"func test():\n"
		"\treturn [Derived.STEP, Derived.new().f()]\n");
	const IRFunction* derived_f = find_function(inherited, "@Derived.f");
	check(derived_f != nullptr && count_opcode(*derived_f, IROpcode::LOAD_IMM) == 1,
		"a bare name in the body finds the base's constant");

	// A static method is the lifted function without the instance parameter.
	const IRProgram statics = compile_to_ir(
		"class Math:\n"
		"\tstatic func twice(x):\n"
		"\t\treturn x * 2\n"
		"func test():\n"
		"\treturn Math.twice(21)\n");
	const IRFunction* twice = find_function(statics, "@Math.twice");
	check(twice != nullptr && twice->parameters.size() == 1 && twice->parameters[0] == "x",
		"a static method takes its declared parameters and nothing else");
	const IRFunction* statics_test = find_function(statics, "test");
	if (statics_test != nullptr) {
		check(count_opcode(*statics_test, IROpcode::VCALL) == 0,
			"and the call site names it directly");
		const std::vector<std::string> names = called_names(statics, *statics_test);
		check(std::find(names.begin(), names.end(), "@Math.twice") != names.end(),
			"by its lifted name");
	}

	// What has no instance cannot reach one.
	check(compile_error("class A:\n\tvar v = 1\n\tstatic func f():\n\t\treturn v\n"
		"func test():\n\treturn A.f()\n")
		.find("one per instance") != std::string::npos,
		"a field is out of reach from a static method");
	check(compile_error("class A:\n\tstatic func f():\n\t\treturn self\n"
		"func test():\n\treturn A.f()\n")
		.find("runs without one") != std::string::npos,
		"and so is self");
	check(compile_error("class A:\n\tvar v = 1\n\tfunc f():\n\t\treturn v\n"
		"func test():\n\treturn A.f()\n")
		.find("one per instance") != std::string::npos,
		"an instance method is not callable on the class");
	check(compile_error("class A:\n\tconst X = [1, 2]\n"
		"func test():\n\treturn A.X\n")
		.find("not a compile-time value") != std::string::npos,
		"a constant that cannot fold is refused, not given a slot");
	check(compile_error("class A:\n\tconst X = 1\n"
		"func test():\n\treturn A.Y\n")
		.find("no constant named") != std::string::npos,
		"a name the class does not declare is a diagnostic");

	std::cout << "  ✓ A class body holds constants and static methods" << std::endl;
}

void test_an_untracked_instance_answers_is() {
	std::cout << "Testing 'is' on an instance the compiler stopped tracking..." << std::endl;

	// Out of a container the value is a Dictionary and nothing more, so the
	// answer has to come from what new() wrote into it.
	const IRProgram ir = compile_to_ir(
		"class Base:\n"
		"\tvar v = 1\n"
		"class Derived extends Base:\n"
		"\tvar w = 2\n"
		"class Other:\n"
		"\tvar u = 3\n"
		"func take(x):\n"
		"\treturn x is Base\n"
		"func test():\n"
		"\treturn take(Derived.new())\n");
	const IRFunction* take = find_function(ir, "take");
	check(take != nullptr, "take() is lowered");
	if (take != nullptr) {
		check(!vcalls(ir, *take, "is_class") && !vcalls(ir, *take, "get_script"),
			"the file declares the chain, so the engine is not asked");
		check(count_opcode(*take, IROpcode::DICT_GET_CONST) == 1,
			"one get answers it, whatever the chain's length");
		check(count_opcode(*take, IROpcode::CMP_EQ) == 2,
			"Base and Derived answer true, Other is not compared against");
	}

	const auto& names = ir.string_constants;
	check(std::find(names.begin(), names.end(), std::string("@class")) != names.end(),
		"the instance carries the class under a key no field can spell");

	// A struct is a plain Dictionary: it carries no name, so nothing to compare.
	const IRProgram plain = compile_to_ir(
		"struct Point:\n"
		"\tvar x = 0\n"
		"func take(p):\n"
		"\treturn p is Point\n"
		"func test():\n"
		"\treturn take(Point.new())\n");
	check(std::find(plain.string_constants.begin(), plain.string_constants.end(),
		std::string("@class")) == plain.string_constants.end(),
		"a struct is not tagged");

	std::cout << "  ✓ An instance answers 'is' after the compiler loses its type" << std::endl;
}

void test_a_class_without_a_base_is_still_refcounted() {
	std::cout << "Testing 'is Object' on a class that extends nothing..." << std::endl;

	// GDScript gives a class with no `extends` an implicit RefCounted base, so
	// both names answer true. The Dictionary holds no object to ask.
	const IRProgram ir = compile_to_ir(
		"class Plain:\n"
		"\tvar v = 1\n"
		"func test():\n"
		"\treturn [Plain.new() is RefCounted, Plain.new() is Object, Plain.new() is Node]\n");
	const IRFunction* test = find_function(ir, "test");
	check(test != nullptr && !vcalls(ir, *test, "is_class"),
		"the declaration settles it without asking the engine");
	if (test != nullptr) {
		std::vector<int64_t> answers;
		for (const IRInstruction& instr : test->instructions) {
			if (instr.opcode == IROpcode::LOAD_BOOL) {
				answers.push_back(instr.operands[1].immediate());
			}
		}
		check(answers == std::vector<int64_t>{1, 1, 0},
			"RefCounted and Object answer true, an unrelated engine class false");
	}

	// A struct is a Dictionary and nothing more: it is not an Object.
	const IRProgram plain = compile_to_ir(
		"struct Point:\n"
		"\tvar x = 0\n"
		"func test():\n"
		"\treturn Point.new() is Object\n");
	const IRFunction* plain_test = find_function(plain, "test");
	if (plain_test != nullptr) {
		bool answered_true = false;
		for (const IRInstruction& instr : plain_test->instructions) {
			if (instr.opcode == IROpcode::LOAD_BOOL
				&& instr.operands[1].immediate() == 1) {
				answered_true = true;
			}
		}
		check(!answered_true, "a struct does not borrow the implicit base");
	}

	std::cout << "  \u2713 A class with no base is a RefCounted" << std::endl;
}

void test_a_method_that_returns_self_keeps_the_type() {
	std::cout << "Testing a method that answers 'self'..." << std::endl;

	// Without this the second call is a VCALL on a Dictionary, which is the
	// retarget-to-@base path, and a class with no base has none.
	const IRProgram ir = compile_to_ir(
		"class Builder:\n"
		"\tvar v = 0\n"
		"\tfunc bump():\n"
		"\t\tv += 1\n"
		"\t\treturn self\n"
		"func test():\n"
		"\treturn Builder.new().bump().bump().v\n");
	const IRFunction* test = find_function(ir, "test");
	check(test != nullptr && count_opcode(*test, IROpcode::VCALL) == 0,
		"chaining stays a direct call");
	if (test != nullptr) {
		const std::vector<std::string> names = called_names(ir, *test);
		check(std::count(names.begin(), names.end(), "@Builder.bump") == 2,
			"both calls are the lifted method");
	}

	// A method that can answer something else keeps the instance untracked:
	// the value is not known to be one.
	const IRProgram mixed = compile_to_ir(
		"class Builder:\n"
		"\tvar v = 0\n"
		"\tfunc bump(stop):\n"
		"\t\tif stop:\n"
		"\t\t\treturn null\n"
		"\t\treturn self\n"
		"func test():\n"
		"\treturn Builder.new().bump(false).bump(false).v\n");
	const IRFunction* mixed_test = find_function(mixed, "test");
	check(mixed_test != nullptr && count_opcode(*mixed_test, IROpcode::VCALL) >= 1,
		"a method that can answer null does not settle the type");

	// Falling off the end answers null, so the last statement has to be the return.
	const IRProgram falls_off = compile_to_ir(
		"class Builder:\n"
		"\tvar v = 0\n"
		"\tfunc bump(stop):\n"
		"\t\tif not stop:\n"
		"\t\t\treturn self\n"
		"func test():\n"
		"\treturn Builder.new().bump(false).bump(false).v\n");
	const IRFunction* falls_off_test = find_function(falls_off, "test");
	check(falls_off_test != nullptr && count_opcode(*falls_off_test, IROpcode::VCALL) >= 1,
		"a path that falls off the end does not settle the type either");

	std::cout << "  \u2713 A self-returning method answers the receiver" << std::endl;
}


// -= `extends <ScriptClass>` chains =-
//
// The host reads the base sources and hands them over; the compiler folds them
// into one program. Root first here matches CompilerOptions: nearest base first.
// Bases are listed root first here, which is how a chain reads; the option is
// nearest base first, which is the order the host walks them in.
struct Link {
	std::string name;
	std::string source;
};

std::string link_path(const Link& base) {
	return "res://" + base.name + ".gd";
}

std::vector<std::pair<std::string, std::string>> project_classes(const std::vector<Link>& bases) {
	std::vector<std::pair<std::string, std::string>> classes;
	for (const Link& base : bases) {
		classes.emplace_back(base.name, link_path(base));
	}
	// A class_name script the chain does not contain, to pin down that reaching
	// into one is an error rather than a property read on the owner.
	classes.emplace_back("Elsewhere", "res://elsewhere.gd");
	return classes;
}

CompilerOptions chain_options(const std::vector<Link>& bases) {
	CompilerOptions options;
	for (size_t i = bases.size(); i-- > 0;) {
		options.base_sources.push_back(CompilerOptions::BaseSource{
			bases[i].name, link_path(bases[i]), bases[i].source });
	}
	options.global_script_classes = project_classes(bases);
	return options;
}

IRProgram compile_chain_to_ir(const std::string& source, const std::vector<Link>& bases) {
	// Same path the Compiler takes, stopping at the IR so the tests can read it.
	std::vector<ChainLink> links;
	for (const Link& base : bases) {
		Lexer base_lexer(base.source);
		Parser base_parser(base_lexer.tokenize());
		ChainLink link;
		link.name = base.name;
		link.path = link_path(base);
		link.program = base_parser.parse();
		links.push_back(std::move(link));
	}
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	ChainLink leaf;
	leaf.program = parser.parse();
	links.push_back(std::move(leaf));

	CodeGenerator codegen;
	codegen.set_global_script_classes(project_classes(bases));
	return codegen.generate(merge_chain(std::move(links)));
}

std::string chain_error(const std::string& source, const std::vector<Link>& bases) {
	Compiler compiler;
	if (!compiler.compile(source, chain_options(bases)).empty()) {
		return "";
	}
	return compiler.get_error();
}

std::vector<uint8_t> compile_chain(const std::string& source, const std::vector<Link>& bases) {
	Compiler compiler;
	std::vector<uint8_t> elf = compiler.compile(source, chain_options(bases));
	if (elf.empty()) {
		std::cerr << "FAILED to compile chain: " << compiler.get_error() << std::endl;
		failures++;
	}
	return elf;
}

const char* PHYSICS_TEST = // root: the only link that extends an engine class
	"extends Node2D\n"
	"class_name PhysicsTest2D\n"
	"enum TestCollisionShape { RECTANGLE, CIRCLE }\n"
	"const CENTER = 320\n"
	"var output = 0\n"
	"static func get_collision_shape(kind):\n"
	"\treturn kind + 1\n"
	"func test_name():\n"
	"\treturn \"physics\"\n"
	"func describe():\n"
	"\treturn test_name()\n";

const char* UNIT_TEST =
	"extends PhysicsTest2D\n"
	"class_name PhysicsUnitTest2D\n"
	"var monitors = 0\n"
	"func register_monitors(n):\n"
	"\tmonitors = n\n"
	"\treturn monitors\n"
	"func test_name():\n"
	"\treturn \"unit:\" + super.test_name()\n";

void test_a_chain_merges_into_one_program() {
	std::cout << "Testing what a merged extends chain produces..." << std::endl;

	const IRProgram ir = compile_chain_to_ir(
		"extends PhysicsUnitTest2D\n"
		"func test_start():\n"
		"\treturn register_monitors(CENTER)\n",
		{ { "PhysicsTest2D", PHYSICS_TEST }, { "PhysicsUnitTest2D", UNIT_TEST } });

	check(ir.base_class == "PhysicsUnitTest2D", "the declared base stays the script class");
	check(ir.native_base_class == "Node2D",
		"the native base is what the chain bottoms out at, not '" + ir.native_base_class + "'");

	const IRFunction* start = find_function(ir, "test_start");
	check(start != nullptr, "the leaf's own function is there");
	if (start != nullptr) {
		const std::vector<std::string> calls = called_names(ir, *start);
		check(std::find(calls.begin(), calls.end(), "register_monitors") != calls.end(),
			"an inherited method is a direct call, not a VCALL on the owner");
		check(count_opcode(*start, IROpcode::VCALL) == 0,
			"nothing falls through to the owner Node2D");
	}

	check(find_function(ir, "register_monitors") != nullptr, "the base's body is compiled in");

	// Members concatenate base first, so __init_members runs them in chain order.
	check(ir.globals.size() == 3 && ir.globals[0].name == "CENTER" &&
		ir.globals[1].name == "output" && ir.globals[2].name == "monitors",
		"members initialize base first");

	std::cout << "  \u2713 A chain becomes one flat program" << std::endl;
}

void test_an_override_keeps_the_base_copy() {
	std::cout << "Testing override and super across a 3-level chain..." << std::endl;

	const IRProgram ir = compile_chain_to_ir(
		"extends PhysicsUnitTest2D\n"
		"func test_name():\n"
		"\treturn \"leaf:\" + super.test_name()\n"
		"func run():\n"
		"\treturn describe()\n",
		{ { "PhysicsTest2D", PHYSICS_TEST }, { "PhysicsUnitTest2D", UNIT_TEST } });

	// Three declarations of one name: the leaf keeps it, the two it displaced
	// move to symbols only super names.
	check(find_function(ir, "test_name") != nullptr, "the leaf owns the plain name");
	check(find_function(ir, "@super0.test_name") != nullptr, "the root copy survives");
	check(find_function(ir, "@super1.test_name") != nullptr, "the middle copy survives too");

	// Resolved per link: the middle link's super is the root's, not the middle's.
	const IRFunction* middle = find_function(ir, "@super1.test_name");
	if (middle != nullptr) {
		const std::vector<std::string> calls = called_names(ir, *middle);
		check(std::find(calls.begin(), calls.end(), "@super0.test_name") != calls.end(),
			"super in the middle link reaches the root copy");
	}
	const IRFunction* leaf = find_function(ir, "test_name");
	if (leaf != nullptr) {
		const std::vector<std::string> calls = called_names(ir, *leaf);
		check(std::find(calls.begin(), calls.end(), "@super1.test_name") != calls.end(),
			"super in the leaf reaches the middle copy, not the root's");
	}

	// Virtual dispatch falls out of the one flat table: the root's describe()
	// calls test_name(), which is now the leaf's override.
	const IRFunction* describe = find_function(ir, "describe");
	if (describe != nullptr) {
		const std::vector<std::string> calls = called_names(ir, *describe);
		check(std::find(calls.begin(), calls.end(), "test_name") != calls.end(),
			"a base body calling an overridden name reaches the override");
	}

	std::cout << "  \u2713 Overrides displace, they do not erase" << std::endl;
}

void test_an_overridden_setter_keeps_direct_storage_access() {
	std::cout << "Testing a named setter override across a script chain..." << std::endl;

	const IRProgram ir = compile_chain_to_ir(
		"extends Combatant\n"
		"signal resume\n"
		"func set_active(value):\n"
		"\tsuper.set_active(value)\n"
		"\tawait resume\n",
		{ { "Combatant",
			"extends Node\n"
			"class_name Combatant\n"
			"var active = false: set = set_active\n"
			"func set_active(value):\n"
			"\tactive = value\n" } });

	const IRFunction* base_setter = find_function(ir, "@super0.set_active");
	check(base_setter != nullptr, "the displaced base setter survives");
	if (base_setter != nullptr) {
		check(count_opcode(*base_setter, IROpcode::STORE_GLOBAL) == 1,
			"the base setter writes its backing slot directly under its mangled name");
		check(count_opcode(*base_setter, IROpcode::CALL) == 0 &&
			count_opcode(*base_setter, IROpcode::CALL_HOSTED) == 0,
			"the base setter does not recursively invoke the derived setter");
	}

	const IRFunction* derived_setter = find_function(ir, "set_active");
	check(derived_setter != nullptr && derived_setter->is_coroutine,
		"the derived setter remains a coroutine");

	std::cout << "  setter storage semantics survive name mangling" << std::endl;
}

void test_super_of_the_enclosing_function() {
	std::cout << "Testing super() in a merged chain..." << std::endl;

	// super() is the base copy of the function it is written in, not of _init.
	const IRProgram ir = compile_chain_to_ir(
		"extends Base\n"
		"func step(n):\n"
		"\treturn super(n) + 1\n",
		{ { "Base",
			"extends Node\n"
			"class_name Base\n"
			"func step(n):\n"
			"\treturn n * 2\n" } });

	const IRFunction* step = find_function(ir, "step");
	check(step != nullptr, "the override is there");
	if (step != nullptr) {
		const std::vector<std::string> calls = called_names(ir, *step);
		check(std::find(calls.begin(), calls.end(), "@super0.step") != calls.end(),
			"super() calls the base copy of the enclosing function");
	}

	std::cout << "  \u2713 super() answers the enclosing function's base copy" << std::endl;
}

void test_a_base_static_and_enum_are_reachable() {
	std::cout << "Testing base statics, consts and enums..." << std::endl;

	// Qualified by the link's own class_name, which qualifies nothing once the
	// bodies are one program, and unqualified.
	const std::vector<Link> bases = { { "PhysicsTest2D", PHYSICS_TEST },
		{ "PhysicsUnitTest2D", UNIT_TEST } };

	const IRProgram ir = compile_chain_to_ir(
		"extends PhysicsUnitTest2D\n"
		"func f():\n"
		"\treturn PhysicsTest2D.get_collision_shape(PhysicsTest2D.TestCollisionShape.RECTANGLE)\n"
		"func g():\n"
		"\treturn get_collision_shape(TestCollisionShape.CIRCLE) + CENTER\n",
		bases);

	const IRFunction* f = find_function(ir, "f");
	check(f != nullptr, "the qualified form compiles");
	if (f != nullptr) {
		const std::vector<std::string> calls = called_names(ir, *f);
		check(std::find(calls.begin(), calls.end(), "get_collision_shape") != calls.end(),
			"Base.static() is a direct call to the merged function");
		check(count_opcode(*f, IROpcode::VGET) == 0,
			"the qualifier produces no property read on the owner");
	}
	check(!compile_chain(
		"extends PhysicsUnitTest2D\n"
		"func f():\n"
		"\treturn PhysicsTest2D.CENTER + PhysicsTest2D.get_collision_shape(0)\n", bases).empty(),
		"a base constant is reachable through the link name too");

	std::cout << "  \u2713 Base statics, constants and enums resolve unqualified" << std::endl;
}

void test_what_a_chain_refuses() {
	std::cout << "Testing what a merged chain refuses..." << std::endl;

	const std::string collision = chain_error(
		"extends Base\n"
		"var v = 2\n",
		{ { "Base", "extends Node\nclass_name Base\nvar v = 1\n" } });
	check(collision.find("already declared") != std::string::npos,
		"a member declared in both links is refused: " + collision);

	const std::string cycle = chain_error(
		"extends Base\n"
		"func f():\n"
		"\treturn 1\n",
		{ { "Base", "extends Node\nclass_name Base\n" },
			{ "Base", "extends Node\nclass_name Base\n" } });
	check(cycle.find("twice in the 'extends' chain") != std::string::npos,
		"a repeated link is refused rather than merged twice: " + cycle);

	Compiler restricted_compiler;
	CompilerOptions restricted = chain_options({ { "Base", "extends Node\nclass_name Base\n" } });
	restricted.restricted = true;
	check(restricted_compiler.compile("extends Base\nfunc f():\n\treturn 1\n",
		restricted).empty(), "a restricted Sandbox refuses a base body");
	check(restricted_compiler.get_error().find("restricted") != std::string::npos,
		"and says so: " + restricted_compiler.get_error());

	check(!compile_chain(
		"extends Base\n"
		"func f():\n"
		"\treturn Elsewhere.helper()\n",
		{ { "Base", "extends Node\nclass_name Base\n" } }).empty(),
		"a script class outside the chain is reached through an instance");
	const std::string bare = chain_error(
		"extends Base\n"
		"func f():\n"
		"\treturn Elsewhere\n",
		{ { "Base", "extends Node\nclass_name Base\n" } });
	check(bare.find("none of its body is compiled into this program") != std::string::npos,
		"a script class is not a value on its own: " + bare);

	std::cout << "  \u2713 Collisions and cycles are refused" << std::endl;
}

} // namespace

int main() {
	std::cout << "=== Inner Class Tests ===" << std::endl << std::endl;

	test_a_method_is_a_lifted_function();
	test_the_instance_is_a_dictionary();
	test_a_method_call_is_a_direct_call();
	test_a_field_is_the_instance_dictionary();
	test_super_at_script_level();
	test_self_local_call_is_direct_and_typed();
	test_what_is_refused();
	test_a_class_type_travels();
	test_a_lambda_in_a_method_sees_the_class();
	test_a_qualified_base_is_refused();
	test_a_native_base_is_constructed_with_the_instance();
	test_the_bind_syscall_is_emitted_for_an_engine_base();
	test_class_signatures_are_published();
	test_a_class_typed_declaration_constructs_nothing();
	test_super_on_a_native_base_is_marked();
	test_what_the_class_does_not_declare_reaches_the_base();
	test_the_script_still_wins_over_the_base();
	test_super_reaches_the_native_base();
	test_class_name_and_extends_are_published();
	test_restrictions_refuse_what_needs_a_class();
	test_a_top_level_extends_reaches_the_owner();
	test_an_instance_answers_is_and_as();
	test_an_untyped_instance_reaches_the_base();
	test_a_class_body_holds_constants_and_static_methods();
	test_an_untracked_instance_answers_is();
	test_a_class_without_a_base_is_still_refcounted();
	test_a_method_that_returns_self_keeps_the_type();
	test_a_chain_merges_into_one_program();
	test_an_override_keeps_the_base_copy();
	test_an_overridden_setter_keeps_direct_storage_access();
	test_super_of_the_enclosing_function();
	test_a_base_static_and_enum_are_reachable();
	test_what_a_chain_refuses();

	if (failures != 0) {
		std::cerr << std::endl << failures << " class test(s) failed" << std::endl;
		return 1;
	}
	std::cout << std::endl << "All inner class tests passed!" << std::endl;
	return 0;
}
