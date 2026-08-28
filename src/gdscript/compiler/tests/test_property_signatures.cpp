#include "../compiler.h"
#include "../property_signature.h"

#include <cassert>
#include <cstring>
#include <iostream>

using namespace gdscript;

static const PropertySignature *find_property(const std::vector<PropertySignature> &properties,
		const std::string &name) {
	for (const PropertySignature &property : properties) {
		if (property.name == name) {
			return &property;
		}
	}
	return nullptr;
}

int main() {
	Compiler compiler;
	CompilerOptions options;
	options.output_elf = false;
	compiler.compile(R"(
@export_range(0, 10) var count: int = 3
var ratio = 1.5
var enabled = true
var title = "hello"
var nothing = null
var items = []
var mapping = {}
var untyped
static var shared = 7
var runtime_value = make_value()
func make_value():
	return 9
)", options);
	assert(!compiler.get_error_info().has_error);
	const auto &properties = compiler.get_property_signatures();
	assert(find_property(properties, "count")->default_kind == PropertyDefaultKind::INT);
	assert(find_property(properties, "count")->hint == 1);
	assert(find_property(properties, "count")->hint_string == "0.0,10.0");
	assert(find_property(properties, "ratio")->default_kind == PropertyDefaultKind::FLOAT);
	assert(find_property(properties, "enabled")->default_kind == PropertyDefaultKind::BOOL);
	assert(find_property(properties, "title")->default_kind == PropertyDefaultKind::STRING);
	assert(find_property(properties, "nothing")->default_kind == PropertyDefaultKind::NIL);
	assert(find_property(properties, "items")->default_kind == PropertyDefaultKind::EMPTY_ARRAY);
	assert(find_property(properties, "mapping")->default_kind == PropertyDefaultKind::EMPTY_DICTIONARY);
	assert(find_property(properties, "untyped")->type == -1);
	assert(find_property(properties, "shared")->is_static);
	assert(find_property(properties, "runtime_value")->default_kind == PropertyDefaultKind::NONE);

	const std::vector<uint8_t> bytes = encode_property_signatures(properties);
	std::vector<PropertySignature> decoded;
	assert(decode_property_signatures(bytes.data(), bytes.size(), decoded));
	assert(decoded.size() == properties.size());
	assert(encode_property_signatures(decoded) == bytes);

	for (size_t size = 0; size < bytes.size(); size++) {
		assert(!decode_property_signatures(bytes.data(), size, decoded));
		assert(decoded.empty());
	}
	std::vector<uint8_t> trailing = bytes;
	trailing.push_back(0);
	assert(!decode_property_signatures(trailing.data(), trailing.size(), decoded));
	std::vector<uint8_t> invalid_kind = encode_property_signatures({PropertySignature{"x"}});
	// Header (12), name length and byte (5), type (4), class length (4), hint
	// (4), hint string length (4), usage (4), line (4), member/static (2).
	invalid_kind[43] = 255;
	assert(!decode_property_signatures(invalid_kind.data(), invalid_kind.size(), decoded));

	std::cout << "property signatures passed\n";
	return 0;
}
