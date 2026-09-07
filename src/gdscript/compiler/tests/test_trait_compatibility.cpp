#include "../compiler.h"
#include "../trait_conformance.h"
#include <cassert>
#include <iostream>

using namespace gdscript;

static CompilerOptions options() {
	CompilerOptions result;
	result.engine_ancestry = {{"Node", "Object"}, {"Node2D", "CanvasItem,Node,Object"},
		{"Node3D", "Node,Object"}};
	return result;
}

static void parameter(const std::string &required, const std::string &actual, bool compatible) {
	Compiler compiler;
	const auto elf = compiler.compile("uses Contract\ntrait Contract:\n\t@abstract func take(value: " + required +
		") -> void\nfunc take(value: " + actual + ") -> void: pass\n", options());
	if (compatible != !elf.empty()) std::cerr << required << " <- " << actual << ": " << compiler.get_error() << '\n';
	assert(compatible == !elf.empty());
	if (!compatible) {
		assert(compiler.get_error().find("parameter 'value' has incompatible type") != std::string::npos);
		assert(compiler.get_error_info().line > 0 && compiler.get_error_info().column > 0);
	}
}

static void returns(const std::string &required, const std::string &actual, bool compatible) {
	Compiler compiler;
	const auto elf = compiler.compile("uses Contract\ntrait Contract:\n\t@abstract func get_value() -> " + required +
		"\nfunc get_value() -> " + actual + ": return " + (actual.find("Array[") == 0 ? "[]" : "null") + "\n", options());
	if (compatible != !elf.empty()) std::cerr << required << " <- return " << actual << ": " << compiler.get_error() << '\n';
	assert(compatible == !elf.empty());
	if (!compatible) assert(compiler.get_error().find("returns '") != std::string::npos);
}

static ClassSignature api(const std::string &methods) {
	Compiler compiler;
	const auto elf = compiler.compile("trait_name API\n" + methods, options());
	if (elf.empty()) std::cerr << methods << ": " << compiler.get_error() << '\n';
	assert(!elf.empty());
	const auto classes = compiler.get_class_signatures();
	// Exercise the declarations as an independently compiled ELF stores them.
	const auto blob = encode_class_signatures(classes);
	std::vector<ClassSignature> decoded;
	assert(decode_class_signatures(blob.data(), blob.size(), decoded));
	for (const auto &c : decoded) if (c.is_trait && c.name == "API") return c;
	assert(false);
	return {};
}

static void offered_api() {
	const auto old = api("@abstract func log(text: String) -> int | float\n");
	const auto expanded = api("@abstract func extra() -> void\n@abstract func log(message: Variant) -> int\n");
	assert(trait_conformance(expanded.trait_methods, old).empty());
	assert(!trait_conformance(old.trait_methods, expanded).empty());
	for (const auto &declaration : {
		"@abstract func log() -> int\n", "@abstract func log(text: int) -> int\n",
		"@abstract func log(text: String) -> String\n", "@abstract func other(text: String) -> int\n",
		"static func log(text: String) -> int: return 0\n"
	}) {
		const auto changed = api(declaration);
		assert(!trait_conformance(changed.trait_methods, old).empty());
	}
}

int main() {
	parameter("Array[int]", "Array[String]", false);
	parameter("Array[int]", "Array", false);
	parameter("Array", "Array[int]", false);
	parameter("Dictionary[String, int]", "Dictionary[String, String]", false);
	parameter("Dictionary[String, int]", "Dictionary[int, int]", false);
	parameter("Array[Array[int]]", "Array[Array[String]]", false);
	parameter("Array[int]?", "Array[String]?", false);
	parameter("Array[int]", "Array[int]?", true);
	parameter("Array[int]?", "Array[int]", false);
	parameter("Array[int]", "Array[int]", true);
	parameter("Dictionary[String, int]", "Dictionary[String, int]", true);
	parameter("Array[int]", "Variant", true);
	parameter("Node", "Node2D", false);
	parameter("Node2D", "Node3D", false);
	parameter("Node2D", "Node", true);
	parameter("Node2D", "Object", true);
	parameter("Node2D?", "Node", false);
	parameter("Node2D", "Node?", true);
	parameter("Node2D | int", "Node | int", true);
	parameter("Node | int", "Node2D | int", false);
	parameter("Node | int", "int | Node", true);
	parameter("int", "int | float", true);
	parameter("int | float", "int", false);
	returns("Node", "Node2D", true);
	returns("Node2D", "Node", false);
	returns("Array[int]", "Array[String]", false);
	returns("Array[int]", "Array[int]", true);
	offered_api();
	std::cout << "PASS trait compatibility: containers, native variance, recorded API signatures\n";
}
