// Built-in type constructors. Verifies codegen.cpp lowering rules.
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../riscv_codegen.h"
#include "../compiler_exception.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace gdscript;

// -= Helpers =-

static IRProgram compile_to_ir(const std::string& source, bool optimize = false) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
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

static bool refuses(const std::string& source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException&) {
		return true;
	}
	return false;
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

// Single instruction of `opcode` in `func`.
static const IRInstruction& only(const IRFunction& func, IROpcode opcode) {
	const IRInstruction* found = nullptr;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == opcode) {
			assert(found == nullptr && "expected exactly one");
			found = &instr;
		}
	}
	assert(found != nullptr);
	return *found;
}

// Float immediate loaded into `reg`, searched backwards.
static double float_immediate(const IRFunction& func, int reg) {
	for (size_t i = func.instructions.size(); i-- > 0; ) {
		const IRInstruction& instr = func.instructions[i];
		if (instr.opcode == IROpcode::LOAD_FLOAT_IMM &&
			instr.operands[0].reg_index() == reg) {
			return instr.operands[1].float_number();
		}
	}
	assert(false && "no LOAD_FLOAT_IMM for that register");
	return 0.0;
}

static int64_t int_immediate(const IRFunction& func, int reg) {
	for (size_t i = func.instructions.size(); i-- > 0; ) {
		const IRInstruction& instr = func.instructions[i];
		if (instr.opcode == IROpcode::LOAD_IMM &&
			instr.operands[0].reg_index() == reg) {
			return instr.operands[1].immediate();
		}
	}
	assert(false && "no LOAD_IMM for that register");
	return 0;
}

static bool machine_code_builds(const std::string& source) {
	IRProgram ir = compile_to_ir(source, true);
	RISCVCodeGen backend;
	return !backend.generate(ir).empty();
}

// -= Nothing reaches a CALL =-

static void test_no_constructor_becomes_a_call() {
	std::cout << "Testing that a constructor never lowers to a function call..." << std::endl;

	static const char* forms[] = {
		"Vector2()", "Vector2(1, 2)",
		"Vector3()", "Vector3(1, 2, 3)",
		"Vector4()", "Vector4(1, 2, 3, 4)",
		"Vector2i()", "Vector2i(1, 2)",
		"Vector3i()", "Vector3i(1, 2, 3)",
		"Vector4i()", "Vector4i(1, 2, 3, 4)",
		"Rect2()", "Rect2(1, 2, 3, 4)", "Rect2(Vector2(1, 2), Vector2(3, 4))",
		"Rect2i()", "Rect2i(1, 2, 3, 4)", "Rect2i(Vector2i(1, 2), Vector2i(3, 4))",
		"Plane()", "Plane(1, 2, 3, 4)", "Plane(Vector3(0, 1, 0))", "Plane(Vector3(0, 1, 0), 2)",
		"Color()", "Color(1, 0, 0)", "Color(1, 0, 0, 1)", "Color(Color(1, 0, 0), 0.5)",
		"Array()", "Dictionary()",
		"PackedByteArray()", "PackedByteArray([1, 2])",
		"PackedInt32Array()", "PackedInt32Array([1, 2])",
		"PackedInt64Array()", "PackedFloat32Array()", "PackedFloat64Array()",
		"PackedStringArray()", "PackedVector2Array()", "PackedVector3Array()",
		"PackedColorArray()", "PackedVector4Array()",
	};

	for (const char* form : forms) {
		const std::string source = std::string("func test():\n\treturn ") + form + "\n";
		const IRProgram ir = compile_to_ir(source);
		const IRFunction& test = find_function(ir, "test");
		if (count_opcode(test, IROpcode::CALL) != 0) {
			std::cerr << "  " << form << " lowered to a CALL" << std::endl;
			assert(false);
		}
		// Survives optimizer, verifier, backend.
		assert(machine_code_builds(source));
	}

	std::cout << "  ✓ " << (sizeof(forms) / sizeof(forms[0]))
		<< " constructor forms lower without a call" << std::endl;
}

// -= Defaults =-

static void test_zero_argument_forms() {
	std::cout << "Testing the zero-argument forms..." << std::endl;

	// Vector2() -> MAKE_VECTOR2(0, 0).
	const IRProgram v2 = compile_to_ir("func test():\n\treturn Vector2()\n");
	const IRInstruction& make_v2 = only(find_function(v2, "test"), IROpcode::MAKE_VECTOR2);
	assert(make_v2.operands.size() == 3);
	for (int i = 1; i <= 2; i++) {
		assert(float_immediate(find_function(v2, "test"),
			make_v2.operands[i].reg_index()) == 0.0);
	}

	// Color() defaults to opaque black (0, 0, 0, 1).
	const IRProgram color = compile_to_ir("func test():\n\treturn Color()\n");
	const IRFunction& color_fn = find_function(color, "test");
	const IRInstruction& make_color = only(color_fn, IROpcode::MAKE_COLOR);
	assert(make_color.operands.size() == 5);
	assert(float_immediate(color_fn, make_color.operands[1].reg_index()) == 0.0);
	assert(float_immediate(color_fn, make_color.operands[2].reg_index()) == 0.0);
	assert(float_immediate(color_fn, make_color.operands[3].reg_index()) == 0.0);
	assert(float_immediate(color_fn, make_color.operands[4].reg_index()) == 1.0);

	// Color(r, g, b) defaults alpha to 1.
	const IRProgram rgb = compile_to_ir("func test():\n\treturn Color(0.5, 0.5, 0.5)\n");
	const IRFunction& rgb_fn = find_function(rgb, "test");
	const IRInstruction& make_rgb = only(rgb_fn, IROpcode::MAKE_COLOR);
	assert(float_immediate(rgb_fn, make_rgb.operands[4].reg_index()) == 1.0);

	// Integer types zero-construct with int components.
	const IRProgram v2i = compile_to_ir("func test():\n\treturn Vector2i()\n");
	const IRFunction& v2i_fn = find_function(v2i, "test");
	const IRInstruction& make_v2i = only(v2i_fn, IROpcode::MAKE_VECTOR2I);
	assert(int_immediate(v2i_fn, make_v2i.operands[1].reg_index()) == 0);

	// Typed var without initializer -> same default constructor.
	const IRProgram declared = compile_to_ir(
		"func test():\n\tvar r : Rect2\n\treturn r\n");
	assert(count_opcode(find_function(declared, "test"), IROpcode::MAKE_RECT2) == 1);

	std::cout << "  ✓ zero-argument forms match Godot's defaults" << std::endl;
}

// -= Rect2, Rect2i and Plane =-

static void test_rect_and_plane() {
	std::cout << "Testing Rect2, Rect2i and Plane..." << std::endl;

	// Dedicated MAKE_RECT2, MAKE_RECT2I, MAKE_PLANE opcodes.
	assert(count_opcode(find_function(
		compile_to_ir("func test():\n\treturn Rect2(0, 0, 1, 1)\n"), "test"),
		IROpcode::MAKE_RECT2) == 1);
	assert(count_opcode(find_function(
		compile_to_ir("func test():\n\treturn Rect2i(0, 0, 1, 1)\n"), "test"),
		IROpcode::MAKE_RECT2I) == 1);
	assert(count_opcode(find_function(
		compile_to_ir("func test():\n\treturn Plane(0, 1, 0, 0)\n"), "test"),
		IROpcode::MAKE_PLANE) == 1);

	// Rect2(position, size): four components from two vectors.
	const IRProgram from_vectors = compile_to_ir(
		"func test():\n\treturn Rect2(Vector2(1, 2), Vector2(3, 4))\n");
	const IRFunction& from_vectors_fn = find_function(from_vectors, "test");
	assert(count_opcode(from_vectors_fn, IROpcode::VGET_INLINE) == 4);
	assert(count_opcode(from_vectors_fn, IROpcode::MAKE_RECT2) == 1);

	// Plane(normal): three components from vector, d defaults to 0.
	const IRProgram plane = compile_to_ir(
		"func test():\n\treturn Plane(Vector3(0, 1, 0))\n");
	const IRFunction& plane_fn = find_function(plane, "test");
	assert(count_opcode(plane_fn, IROpcode::VGET_INLINE) == 3);
	const IRInstruction& make_plane = only(plane_fn, IROpcode::MAKE_PLANE);
	assert(float_immediate(plane_fn, make_plane.operands[4].reg_index()) == 0.0);

	std::cout << "  ✓ Rect2, Rect2i and Plane construct inline" << std::endl;
}

// -= Containers =-

static void test_container_constructors() {
	std::cout << "Testing the container constructors..." << std::endl;

	// MAKE_DICTIONARY requires pair count operand.
	const IRProgram dict = compile_to_ir("func test():\n\treturn Dictionary()\n");
	const IRInstruction& make_dict = only(find_function(dict, "test"), IROpcode::MAKE_DICTIONARY);
	assert(make_dict.operands.size() == 2);
	assert(make_dict.operands[1].immediate() == 0);
	ir_verify(dict, "constructor test");

	// Packed array from one Array; host converts.
	const IRProgram packed = compile_to_ir(
		"func test():\n\treturn PackedInt32Array([1, 2])\n");
	const IRInstruction& make_packed =
		only(find_function(packed, "test"), IROpcode::MAKE_PACKED_INT32_ARRAY);
	assert(make_packed.operands[1].immediate() == 1);

	// Element list not accepted; host expects one Array Variant in a2.
	assert(refuses("func test():\n\treturn PackedInt32Array(1, 2)\n"));
	assert(refuses("func test():\n\treturn Array(1, 2)\n"));
	// Array(from) and Dictionary(from) use Godot's Variant constructor so they
	// also work when the value came from a host method call.
	const IRProgram converted = compile_to_ir(
		"extends Node\n"
		"var names = Array($AnimatedSprite2D.sprite_frames.get_animation_names())\n"
		"func array_copy(value):\n\t\treturn Array(value)\n"
		"func dictionary_copy(value):\n\t\treturn Dictionary(value)\n");
	assert(count_opcode(converted.member_init, IROpcode::CONSTRUCT) == 1);
	const IRInstruction& array_construct = only(
		find_function(converted, "array_copy"), IROpcode::CONSTRUCT);
	assert(array_construct.operands[1].immediate() == Variant::ARRAY);
	const IRInstruction& dictionary_construct = only(
		find_function(converted, "dictionary_copy"), IROpcode::CONSTRUCT);
	assert(dictionary_construct.operands[1].immediate() == Variant::DICTIONARY);
	assert(machine_code_builds("func test(value):\n\treturn Array(value)\n"));

	std::cout << "  ✓ containers are built empty or converted by the host" << std::endl;
}

// -= Arity =-

static void test_wrong_arity_is_refused() {
	std::cout << "Testing that a wrong argument count is a compile error..." << std::endl;

	assert(refuses("func test():\n\treturn Vector2(1)\n"));
	assert(refuses("func test():\n\treturn Vector2(1, 2, 3)\n"));
	assert(refuses("func test():\n\treturn Vector3(1, 2)\n"));
	assert(refuses("func test():\n\treturn Vector4i(1, 2, 3)\n"));
	assert(refuses("func test():\n\treturn Rect2(1, 2, 3)\n"));
	assert(refuses("func test():\n\treturn Color(1)\n"));
	assert(refuses("func test():\n\treturn Color(1, 2, 3, 4, 5)\n"));

	std::cout << "  ✓ every accepted form is in the table, and nothing else is"
		<< std::endl;
}

static void test_conversions_reach_the_engine() {
	std::cout << "Testing the one-argument conversions..." << std::endl;

	const IRProgram narrow = compile_to_ir("func test():\n\treturn Vector2(Vector2i(1, 2))\n");
	const IRFunction& narrow_fn = find_function(narrow, "test");
	const IRInstruction& construct = only(narrow_fn, IROpcode::CONSTRUCT);
	assert(construct.operands[1].immediate() == Variant::VECTOR2);
	assert(construct.operands[2].immediate() == 1);

	const IRProgram transform = compile_to_ir(
		"func test():\n\treturn Transform2D(0.5, Vector2(1, 2))\n");
	const IRInstruction& t2d = only(find_function(transform, "test"), IROpcode::CONSTRUCT);
	assert(t2d.operands[1].immediate() == Variant::TRANSFORM2D);
	assert(t2d.operands[2].immediate() == 2);

	assert(machine_code_builds("func test():\n\treturn Quaternion(0, 0, 0, 1)\n"));
	assert(machine_code_builds("func test():\n\treturn RID()\n"));
	assert(machine_code_builds("func test():\n\treturn Basis()\n"));

	std::cout << "  ✓ conversions and payload-less types construct on the host"
		<< std::endl;
}

// -= Constants of the built-in types =-

static void test_builtin_constants() {
	std::cout << "Testing Vector2.ZERO and friends..." << std::endl;

	// Constant folds into MAKE_*, not VGET.
	const IRProgram zero = compile_to_ir("func test():\n\treturn Vector2.ZERO\n");
	const IRFunction& zero_fn = find_function(zero, "test");
	assert(count_opcode(zero_fn, IROpcode::MAKE_VECTOR2) == 1);
	assert(count_opcode(zero_fn, IROpcode::VGET) == 0);
	assert(count_opcode(zero_fn, IROpcode::VCALL) == 0);

	// Values from extension_api.json.
	const IRProgram forward = compile_to_ir("func test():\n\treturn Vector3.FORWARD\n");
	const IRFunction& forward_fn = find_function(forward, "test");
	const IRInstruction& make_forward = only(forward_fn, IROpcode::MAKE_VECTOR3);
	assert(float_immediate(forward_fn, make_forward.operands[1].reg_index()) == 0.0);
	assert(float_immediate(forward_fn, make_forward.operands[2].reg_index()) == 0.0);
	assert(float_immediate(forward_fn, make_forward.operands[3].reg_index()) == -1.0);

	// Integer type constants load as integers.
	const IRProgram max = compile_to_ir("func test():\n\treturn Vector2i.MAX\n");
	const IRFunction& max_fn = find_function(max, "test");
	const IRInstruction& make_max = only(max_fn, IROpcode::MAKE_VECTOR2I);
	assert(int_immediate(max_fn, make_max.operands[1].reg_index()) == 2147483647);

	assert(count_opcode(find_function(
		compile_to_ir("func test():\n\treturn Color.RED\n"), "test"),
		IROpcode::MAKE_COLOR) == 1);
	assert(count_opcode(find_function(
		compile_to_ir("func test():\n\treturn Plane.PLANE_XY\n"), "test"),
		IROpcode::MAKE_PLANE) == 1);

	// Payloads wider than the inline Variant area are assembled from generated
	// extension_api.json components, then handed to Godot's constructor table.
	const IRProgram transform2d = compile_to_ir(
		"func test():\n\treturn Transform2D.IDENTITY\n");
	const IRFunction& transform2d_fn = find_function(transform2d, "test");
	assert(count_opcode(transform2d_fn, IROpcode::MAKE_VECTOR2) == 3);
	const IRInstruction& make_transform2d = only(transform2d_fn, IROpcode::CONSTRUCT);
	assert(make_transform2d.operands[1].immediate() == Variant::TRANSFORM2D);
	assert(make_transform2d.operands[2].immediate() == 3);

	const IRProgram basis = compile_to_ir("func test():\n\treturn Basis.FLIP_Y\n");
	assert(count_opcode(find_function(basis, "test"), IROpcode::MAKE_VECTOR3) == 3);
	const IRInstruction& make_basis = only(find_function(basis, "test"), IROpcode::CONSTRUCT);
	assert(make_basis.operands[1].immediate() == Variant::BASIS);

	const IRProgram quaternion = compile_to_ir(
		"func test():\n\treturn Quaternion.IDENTITY\n");
	const IRInstruction& make_quaternion = only(find_function(quaternion, "test"), IROpcode::CONSTRUCT);
	assert(make_quaternion.operands[1].immediate() == Variant::QUATERNION);
	assert(make_quaternion.operands[2].immediate() == 4);

	const IRProgram projection = compile_to_ir("func test():\n\treturn Projection.ZERO\n");
	assert(count_opcode(find_function(projection, "test"), IROpcode::MAKE_VECTOR4) == 4);
	const IRInstruction& make_projection = only(find_function(projection, "test"), IROpcode::CONSTRUCT);
	assert(make_projection.operands[1].immediate() == Variant::PROJECTION);

	const IRProgram transform3d = compile_to_ir(
		"func test():\n\treturn Transform3D.FLIP_Z\n");
	assert(count_opcode(find_function(transform3d, "test"), IROpcode::CONSTRUCT) == 2);
	assert(machine_code_builds("func test():\n\treturn Transform2D.IDENTITY\n"));
	assert(machine_code_builds("func test():\n\treturn Transform3D.FLIP_X\n"));
	assert(machine_code_builds("func test():\n\treturn Basis.FLIP_Z\n"));
	assert(machine_code_builds("func test():\n\treturn Quaternion.IDENTITY\n"));
	assert(machine_code_builds("func test():\n\treturn Projection.ZERO\n"));

	// Unknown constant name -> compile error.
	assert(refuses("func test():\n\treturn Vector2.BOGUS\n"));
	assert(refuses("func test():\n\treturn Transform2D.BOGUS\n"));

	// Local shadows the type name.
	const IRProgram shadowed = compile_to_ir(
		"func test():\n\tvar Color = {}\n\tColor.RED = 1\n\treturn Color\n");
	assert(count_opcode(find_function(shadowed, "test"), IROpcode::MAKE_COLOR) == 0);

	std::cout << "  ✓ built-in constants fold into their constructor" << std::endl;
}

// -= Color8 =-

static void test_color8_rescales() {
	std::cout << "Testing Color8()..." << std::endl;

	// Color8 divides by 255 then builds MAKE_COLOR; separate from Color lowering.
	const IRProgram ir = compile_to_ir("func test():\n\treturn Color8(255, 128, 0)\n");
	const IRFunction& test = find_function(ir, "test");
	assert(count_opcode(test, IROpcode::MAKE_COLOR) == 1);
	assert(count_opcode(test, IROpcode::DIV) == 3);
	assert(count_opcode(test, IROpcode::CALL) == 0);

	// Four arguments: alpha also divided.
	assert(count_opcode(find_function(
		compile_to_ir("func test():\n\treturn Color8(255, 128, 0, 128)\n"), "test"),
		IROpcode::DIV) == 4);

	// Constant-folded to the resulting Color.
	const IRProgram folded = compile_to_ir("func test():\n\treturn Color8(255, 0, 0)\n", true);
	const IRFunction& folded_fn = find_function(folded, "test");
	const IRInstruction& make = only(folded_fn, IROpcode::MAKE_COLOR);
	assert(float_immediate(folded_fn, make.operands[1].reg_index()) == 1.0);
	assert(float_immediate(folded_fn, make.operands[2].reg_index()) == 0.0);
	assert(float_immediate(folded_fn, make.operands[4].reg_index()) == 1.0);

	assert(refuses("func test():\n\treturn Color8(1, 2)\n"));
	assert(refuses("func test():\n\treturn Color8(1, 2, 3, 4, 5)\n"));
	assert(machine_code_builds("func test():\n\treturn Color8(255, 128, 0)\n"));

	std::cout << "  ✓ Color8 divides by 255 and builds a Color" << std::endl;
}

int main() {
	std::cout << "=== Built-in Type Constructor Tests ===" << std::endl << std::endl;

	try {
		test_no_constructor_becomes_a_call();
		test_zero_argument_forms();
		test_rect_and_plane();
		test_container_constructors();
		test_wrong_arity_is_refused();
		test_conversions_reach_the_engine();
		test_builtin_constants();
		test_color8_rescales();
	} catch (const CompilerException& e) {
		std::cerr << "Unexpected compiler error: " << e.what() << std::endl;
		return 1;
	}

	std::cout << std::endl << "All constructor tests passed." << std::endl;
	return 0;
}
