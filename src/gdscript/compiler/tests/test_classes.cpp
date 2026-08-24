#include "../codegen.h"
#include "../compiler.h"
#include "../compiler_exception.h"
#include "../ir_optimizer.h"
#include "../lexer.h"
#include "../parser.h"
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
		check(std::get<int64_t>(instr.operands[1].value) == 2,
			"an instance of Derived holds both v and extra");
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

	const std::string missing_base = compile_error(
		"class A extends Nope:\n"
		"\tvar x = 1\n"
		"func test():\n\treturn 1\n");
	check(missing_base.find("does not declare") != std::string::npos,
		"extending a class the script does not declare is refused: " + missing_base);

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
		"\tconst X = 1\n"
		"func test():\n\treturn 1\n");
	check(body.find("field and function") != std::string::npos,
		"a const in a class body is refused: " + body);

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

	if (failures != 0) {
		std::cerr << std::endl << failures << " class test(s) failed" << std::endl;
		return 1;
	}
	std::cout << std::endl << "All inner class tests passed!" << std::endl;
	return 0;
}
