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

std::vector<std::string> called_names(const IRFunction& func) {
	std::vector<std::string> names;
	for (const IRInstruction& instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL) {
			names.push_back(std::get<std::string>(instr.operands[0].value));
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
		check(std::get<int64_t>(instr.operands[1].value) == 3,
			"an instance of Derived holds v, extra and the class it was made from");
	}

	const std::vector<std::string> calls = called_names(*test);
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
		const std::vector<std::string> calls = called_names(*derived_greet);
		check(calls.size() == 1 && calls[0] == "@Base.greet",
			"super.greet() calls the base's greet");
	}

	const IRFunction* derived_init = find_function(inherited, "@Derived._init");
	if (derived_init != nullptr) {
		const std::vector<std::string> calls = called_names(*derived_init);
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
		const std::vector<std::string> calls = called_names(*other);
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
		check(count_opcode(*bump, IROpcode::DICT_SET) == 1, "n += k writes the Dictionary");
	}

	const IRFunction* test = find_function(ir, "test");
	if (test != nullptr) {
		check(count_opcode(*test, IROpcode::DICT_SET) == 1, "c.n = 3 writes the Dictionary");
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
		check(count_opcode(*over_global, IROpcode::DICT_SET) == 1,
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
		"\tfunc f(a, b, c, d, e, g, h):\n"
		"\t\treturn a\n"
		"func test():\n\treturn 1\n");
	check(slots.find("at most 6") != std::string::npos,
		"a method needing eight slots is refused: " + slots);

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
		const std::vector<std::string> calls = called_names(*func);
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
			std::get<int64_t>(instr.operands[1].value) == number)
		{
			count++;
		}
	}
	return count;
}

bool vcalls(const IRFunction& func, const std::string& name) {
	for (const IRInstruction& instr : func.instructions) {
		if (instr.opcode == IROpcode::VCALL &&
			std::get<std::string>(instr.operands[2].value) == name)
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
			check(std::get<int64_t>(instr.operands[1].value) == 3,
				"the instance Dictionary holds the declared field, the base and the class");
		}
	}

	std::cout << "  ✓ The instance is a Dictionary that carries its engine object"
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

	check(count_opcode(*hurt, IROpcode::DICT_SET) == 1,
		"'hp' is the instance Dictionary, not a property of the base");

	check(count_opcode(*hurt, IROpcode::VSET) == 1,
		"'position' is a property set on the base");
	check(count_opcode(*hurt, IROpcode::VGET) == 1,
		"'rotation' is a property get on the base");
	check(vcalls(*hurt, "move_local_x"), "an undeclared call goes to the base");
	check(vcalls(*hurt, "get_index"), "so does one whose value is used");
	check(called_names(*hurt).empty(), "neither became a call to a lifted method");

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
	check(f != nullptr && called_names(*f) == std::vector<std::string>{ "helper" },
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
	check(method != nullptr && vcalls(*method, "get_index"),
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
	check(bf != nullptr && vcalls(*bf, "get_index"),
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
	check(own_test != nullptr && !vcalls(*own_test, "is_class"),
		"an instance answers its own class name without asking the engine");

	const IRProgram other = compile_to_ir(
		"class Sprite extends Node2D:\n"
		"\tvar hp = 3\n"
		"class Other extends Node2D:\n"
		"\tvar hp = 3\n"
		"func test():\n"
		"\treturn Sprite.new() is Other\n");
	const IRFunction* other_test = find_function(other, "test");
	check(other_test != nullptr && !vcalls(*other_test, "is_class"),
		"and answers a sibling class without asking either");

	const IRProgram base = compile_to_ir(
		"class Sprite extends Node2D:\n"
		"\tvar hp = 3\n"
		"func test():\n"
		"\treturn Sprite.new() is Node\n");
	const IRFunction* base_test = find_function(base, "test");
	check(base_test != nullptr && vcalls(*base_test, "is_class"),
		"what the engine knows is asked of the engine");
	check(base_test != nullptr && count_syscall(*base_test, ECALL_DICTIONARY_OPS) >= 1,
		"and it is asked of the object the instance holds, not of the Dictionary");

	const IRProgram inherited = compile_to_ir(
		"class Base extends Node2D:\n"
		"\tvar hp = 3\n"
		"class Derived extends Base:\n"
		"\tvar extra = 1\n"
		"func test():\n"
		"\treturn Derived.new() is Base\n");
	const IRFunction* inherited_test = find_function(inherited, "test");
	check(inherited_test != nullptr && !vcalls(*inherited_test, "is_class"),
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
				&& std::get<int64_t>(instr.operands[1].value) == 40) {
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
		const std::vector<std::string> names = called_names(*statics_test);
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
		check(!vcalls(*take, "is_class") && !vcalls(*take, "get_script"),
			"the file declares the chain, so the engine is not asked");
		check(count_syscall(*take, ECALL_DICTIONARY_OPS) == 1,
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
	check(test != nullptr && !vcalls(*test, "is_class"),
		"the declaration settles it without asking the engine");
	if (test != nullptr) {
		std::vector<int64_t> answers;
		for (const IRInstruction& instr : test->instructions) {
			if (instr.opcode == IROpcode::LOAD_BOOL) {
				answers.push_back(std::get<int64_t>(instr.operands[1].value));
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
				&& std::get<int64_t>(instr.operands[1].value) == 1) {
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
		const std::vector<std::string> names = called_names(*test);
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

} // namespace

int main() {
	std::cout << "=== Inner Class Tests ===" << std::endl << std::endl;

	test_a_method_is_a_lifted_function();
	test_the_instance_is_a_dictionary();
	test_a_method_call_is_a_direct_call();
	test_a_field_is_the_instance_dictionary();
	test_super_at_script_level();
	test_what_is_refused();
	test_a_class_type_travels();
	test_a_lambda_in_a_method_sees_the_class();
	test_a_qualified_base_is_refused();
	test_a_native_base_is_constructed_with_the_instance();
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

	if (failures != 0) {
		std::cerr << std::endl << failures << " class test(s) failed" << std::endl;
		return 1;
	}
	std::cout << std::endl << "All inner class tests passed!" << std::endl;
	return 0;
}
