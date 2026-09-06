#include "../compiler.h"
#include "../gdsmeta.h"
#include "../trait_conformance.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;

namespace {

std::vector<uint8_t> section_bytes(const std::vector<uint8_t> &elf, const std::string &name) {
	auto u16 = [&](size_t off) { uint16_t v; std::memcpy(&v, elf.data() + off, 2); return v; };
	auto u32 = [&](size_t off) { uint32_t v; std::memcpy(&v, elf.data() + off, 4); return v; };
	auto u64 = [&](size_t off) { uint64_t v; std::memcpy(&v, elf.data() + off, 8); return v; };

	assert(elf.size() > 64 && "not an ELF");
	const uint64_t shoff = u64(0x28);
	const uint16_t shentsize = u16(0x3a);
	const uint16_t shnum = u16(0x3c);
	const uint16_t shstrndx = u16(0x3e);

	const uint64_t shstr_hdr = shoff + uint64_t(shstrndx) * shentsize;
	const uint64_t shstr_off = u64(shstr_hdr + 0x18);

	for (uint16_t i = 0; i < shnum; i++) {
		const uint64_t hdr = shoff + uint64_t(i) * shentsize;
		const uint32_t sh_name = u32(hdr + 0x00);
		const char *sname = reinterpret_cast<const char *>(elf.data() + shstr_off + sh_name);
		if (name == sname) {
			const uint64_t off = u64(hdr + 0x18);
			const uint64_t size = u64(hdr + 0x20);
			return std::vector<uint8_t>(elf.begin() + off, elf.begin() + off + size);
		}
	}
	return {};
}

void test_codec_round_trip() {
	std::cout << "Testing metadata codec round-trip..." << std::endl;

	ScriptMetadata meta;
	meta.double_precision = true;
	meta.is_tool = true;
	meta.base_is_path = false;
	meta.class_name = "Enemy";
	meta.base_class = "Node2D";

	FunctionSignature scaled;
	scaled.name = "scaled";
	scaled.return_type = 3;
	scaled.line = 8;
	scaled.description = "Doubles the speed.";
	scaled.required_arguments = 0;
	FunctionParameter mult;
	mult.name = "mult";
	mult.type = FunctionParameter::ANY_TYPE;
	mult.default_kind = FunctionParameter::DefaultKind::INT;
	mult.default_value = int64_t(2);
	scaled.parameters.push_back(mult);
	meta.functions.push_back(scaled);

	FunctionSignature sig;
	sig.name = "hit";
	meta.signals.push_back(sig);
	PropertySignature property; property.name = "speed"; property.type = 3; property.is_member = true;
	meta.properties.push_back(property);
	RPCConfig rpc; rpc.name = "scaled"; rpc.channel = 2;
	meta.rpc_configs.push_back(rpc);

	meta.line_table.entries = { { 0, 8 }, { 16, 9 }, { 32, 11 } };

	const std::vector<uint8_t> blob = encode_script_metadata(meta);

	ScriptMetadata out;
	assert(decode_script_metadata(blob.data(), blob.size(), out));
	assert(out.double_precision == true);
	assert(out.is_tool == true);
	assert(out.base_is_path == false);
	assert(out.class_name == "Enemy");
	assert(out.base_class == "Node2D");
	assert(out.functions.size() == 1);
	assert(out.functions[0].name == "scaled");
	assert(out.functions[0].line == 8);
	assert(out.functions[0].description == "Doubles the speed.");
	assert(out.functions[0].parameters.size() == 1);
	assert(out.functions[0].parameters[0].name == "mult");
	assert(out.functions[0].parameters[0].default_kind == FunctionParameter::DefaultKind::INT);
	assert(out.signals.size() == 1 && out.signals[0].name == "hit");
	assert(out.properties.size() == 1 && out.properties[0].name == "speed" && out.properties[0].is_member);
	assert(out.rpc_configs.size() == 1 && out.rpc_configs[0].name == "scaled" && out.rpc_configs[0].channel == 2);
	assert(out.line_table.entries.size() == 3);
	assert(out.line_table.entries[1].address == 16 && out.line_table.entries[1].line == 9);

	std::cout << "  Round-trip OK (" << blob.size() << " bytes)" << std::endl;
}

void test_rejects_bad_input() {
	std::cout << "Testing rejection of malformed blobs..." << std::endl;

	ScriptMetadata out;
	assert(!decode_script_metadata(nullptr, 0, out));
	const uint8_t garbage[] = { 'X', 'X', 'X', 'X', 1, 0, 0, 0 };
	assert(!decode_script_metadata(garbage, sizeof(garbage), out));

	ScriptMetadata meta;
	meta.class_name = "A";
	meta.base_class = "RefCounted";
	const std::vector<uint8_t> blob = encode_script_metadata(meta);
	for (size_t n = 0; n < blob.size(); n++) {
		out = meta;
		assert(!decode_script_metadata(blob.data(), n, out) && "truncated blob accepted");
		assert(encode_script_metadata(out) == encode_script_metadata(ScriptMetadata{}) && "failure retained partial metadata");
	}
	assert(decode_script_metadata(blob.data(), blob.size(), out));
	auto old = blob; old[4] = 1;
	assert(!decode_script_metadata(old.data(), old.size(), out));
	for (size_t n = 8; n + 4 <= blob.size(); ++n) {
		if (std::memcmp(blob.data() + n, "CNST", 4) && std::memcmp(blob.data() + n, "TRAT", 4) && std::memcmp(blob.data() + n, "PRPC", 4)) continue;
		auto corrupt = blob; corrupt[n] ^= 0xff;
		out = meta;
		assert(!decode_script_metadata(corrupt.data(), corrupt.size(), out));
		assert(encode_script_metadata(out) == encode_script_metadata(ScriptMetadata{}));
	}

	std::cout << "  Rejection OK" << std::endl;
}

void test_compiled_elf_carries_metadata() {
	std::cout << "Testing that a compiled ELF carries .gdsmeta..." << std::endl;

	const std::string source =
		"class_name Enemy\n"
		"extends Node2D\n"
		"\n"
		"@export var speed: float = 1.5\n"
		"\n"
		"## Doubles the speed.\n"
		"func scaled(mult: int = 2) -> float:\n"
		"\treturn speed * mult\n"
		"\n"
		"func _ready() -> void:\n"
		"\tprint(\"ready\")\n";

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;
	const std::vector<uint8_t> elf = compiler.compile(source, options);
	assert(!elf.empty() && "compile failed");

	const std::vector<uint8_t> comment = section_bytes(elf, ".comment");
	assert(!comment.empty());
	const std::string comment_str(comment.begin(), comment.end());
	assert(comment_str.find("Godot GDScript") != std::string::npos);

	const std::vector<uint8_t> blob = section_bytes(elf, ".gdsmeta");
	assert(!blob.empty() && ".gdsmeta section missing");

	ScriptMetadata meta;
	assert(decode_script_metadata(blob.data(), blob.size(), meta) && ".gdsmeta failed to decode");

	assert(meta.properties.size() == 1 && meta.properties[0].name == "speed");
	assert(meta.class_name == compiler.get_class_name());
	assert(meta.base_class == compiler.get_base_class());
	assert(meta.base_is_path == compiler.base_is_path());
	assert(meta.is_tool == compiler.is_tool());

	const auto &declared = compiler.get_function_signatures();
	assert(meta.functions.size() == declared.size());
	for (size_t i = 0; i < declared.size(); i++) {
		assert(meta.functions[i].name == declared[i].name);
		assert(meta.functions[i].line == declared[i].line);
		assert(meta.functions[i].parameters.size() == declared[i].parameters.size());
	}

	assert(meta.line_table.entries.size() == compiler.get_line_table().entries.size());

	bool found_scaled = false;
	for (const FunctionSignature &fn : meta.functions) {
		if (fn.name == "scaled") {
			found_scaled = true;
			assert(fn.description == "Doubles the speed.");
			assert(fn.parameters.size() == 1 && fn.parameters[0].name == "mult");
			assert(fn.required_arguments == 0 && "mult has a default, so it is optional");
		}
	}
	assert(found_scaled);

	std::cout << "  Compiled ELF carries matching .gdsmeta ("
			  << blob.size() << " bytes, " << meta.functions.size() << " functions)" << std::endl;
}

void test_typed_contract() {
	Compiler compiler;
	CompilerOptions options;
	options.base_sources.push_back({"API", "api.sgd", "trait_name API\n@abstract func time() -> float\n", true});
	options.base_sources.push_back({"Brain", "brain.sgd", "trait_name Brain\n@abstract func mod_init(api: API) -> void\n@abstract func think(delta: float) -> Vector2\n", true});
	const std::string source = "uses Brain\nconst COUNT = 42\nenum State { IDLE, RUN }\nfunc mod_init(api: API) -> void: pass\nfunc think(delta: float) -> Vector2: return Vector2.ZERO\n";
	auto elf = compiler.compile(source, options);
	assert(!elf.empty());
	auto blob = section_bytes(elf, ".gdsmeta");
	ScriptMetadata meta;
	assert(decode_script_metadata(blob.data(), blob.size(), meta));
	assert(meta.has_trait_metadata && metadata_uses(meta.uses, meta.classes, "Brain"));
	assert(meta.constants.size() == 2);
	assert(std::any_of(meta.constants.begin(), meta.constants.end(), [](const auto &c) { return c.kind == ScriptConstant::Kind::ENUM && c.members.size() == 2; }));
	const auto it = std::find_if(meta.classes.begin(), meta.classes.end(), [](const auto &c) { return c.name == "Brain"; });
	assert(it != meta.classes.end());
	assert(trait_conformance(meta.functions, *it).empty());
	auto functions = meta.functions;
	functions.erase(std::remove_if(functions.begin(), functions.end(), [](const auto &f) { return f.name == "think"; }), functions.end());
	auto errors = trait_conformance(functions, *it);
	assert(errors.size() == 1 && errors[0] == "Brain.think(delta: float) -> Vector2: does not implement abstract method 'think()'");
	functions = meta.functions;
	auto method = std::find_if(functions.begin(), functions.end(), [](const auto &f) { return f.name == "think"; });
	method->is_static = true;
	assert(trait_conformance(functions, *it)[0].find("is static; the trait declares an instance method") != std::string::npos);
	method->is_static = false;
	method->parameters[0].declared_type = {"String", uint64_t(1) << 4, false};
	assert(trait_conformance(functions, *it)[0].find("parameter 'delta' has incompatible type 'String'") != std::string::npos);
	method->parameters[0].declared_type = {"Variant", 0, false};
	assert(trait_conformance(functions, *it).empty());
	method->declared_return = {};
	assert(trait_conformance(functions, *it)[0].find("untyped return") != std::string::npos);
	// The nominal spellings survive the published OBJECT type.
	const auto init = std::find_if(meta.functions.begin(), meta.functions.end(), [](const auto &f) { return f.name == "mod_init"; });
	assert(init->parameters[0].declared_type.name == "API" && init->parameters[0].declared_type.nominal);
	for (size_t n = 0; n < blob.size(); ++n) assert(!decode_script_metadata(blob.data(), n, meta));
}

} // namespace

int main() {
	std::cout << "=== Self-describing ELF metadata ===" << std::endl;

	test_codec_round_trip();
	test_typed_contract();
	test_rejects_bad_input();
	test_compiled_elf_carries_metadata();

	std::cout << "All .gdsmeta tests passed!" << std::endl;
	return 0;
}
