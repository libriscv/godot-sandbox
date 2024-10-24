#include "sandbox.h"

#include "guest_datatypes.h"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

static constexpr bool VERBOSE = false;
static String *current_generated_api = nullptr;

static const char *cpp_compatible_variant_type(int type) {
	switch (type) {
		case Variant::NIL:
			return "void";
		case Variant::BOOL:
			return "bool";
		case Variant::INT:
			return "int64_t";
		case Variant::FLOAT:
			return "double";
		case Variant::STRING:
		case Variant::NODE_PATH:
		case Variant::STRING_NAME:
			return "String";

		case Variant::VECTOR2:
			return "Vector2";
		case Variant::VECTOR2I:
			return "Vector2i";
		case Variant::RECT2:
			return "Rect2";
		case Variant::RECT2I:
			return "Rect2i";
		case Variant::VECTOR3:
			return "Vector3";
		case Variant::VECTOR3I:
			return "Vector3i";
		case Variant::VECTOR4:
			return "Vector4";
		case Variant::VECTOR4I:
			return "Vector4i";
		case Variant::COLOR:
			return "Color";

		case Variant::PLANE:
			return "Plane";
		case Variant::QUATERNION:
			return "Quaternion";
		case Variant::AABB:
			return "Variant";
		case Variant::TRANSFORM2D:
			return "Transform2D";
		case Variant::TRANSFORM3D:
			return "Transform3D";
		case Variant::BASIS:
			return "Basis";
		case Variant::PROJECTION:
			return "Variant";
		case Variant::RID:
			return "::RID";

		case Variant::OBJECT:
			return "Object";
		case Variant::DICTIONARY:
			return "Dictionary";
		case Variant::ARRAY:
			return "Array";
		case Variant::CALLABLE:
			return "Callable";
		case Variant::SIGNAL:
			return "Variant";

		case Variant::PACKED_BYTE_ARRAY:
			return "PackedArray<uint8_t>";
		case Variant::PACKED_INT32_ARRAY:
			return "PackedArray<int32_t>";
		case Variant::PACKED_INT64_ARRAY:
			return "PackedArray<int64_t>";
		case Variant::PACKED_FLOAT32_ARRAY:
			return "PackedArray<float>";
		case Variant::PACKED_FLOAT64_ARRAY:
			return "PackedArray<double>";
		case Variant::PACKED_STRING_ARRAY:
			return "PackedArray<std::string>";
		case Variant::PACKED_VECTOR2_ARRAY:
			return "PackedArray<Vector2>";
		case Variant::PACKED_VECTOR3_ARRAY:
			return "PackedArray<Vector3>";
		case Variant::PACKED_COLOR_ARRAY:
			return "PackedArray<Color>";
		default:
			throw std::runtime_error("Unknown variant type.");
	}
}

// TODO: Invalidate the API if any classes change or new classes are registered.
String Sandbox::generate_api(String language, String header) {
	if (current_generated_api == nullptr) {
		Sandbox::generate_runtime_cpp_api();
	}
	if (current_generated_api == nullptr) {
		return String();
	}
	return header + *current_generated_api;
}

static String emit_class(ClassDBSingleton *class_db, const HashSet<String> &cpp_keywords, const HashSet<String> &singletons, const String &class_name) {
	// Generate a simple API for each class using METHOD() and PROPERTY() macros to a string.
	if constexpr (VERBOSE) {
		UtilityFunctions::print("* Currently generating: " + class_name);
	}
	String parent_name = class_db->get_parent_class(class_name);

	String api = "struct " + class_name + " : public " + parent_name + " {\n";

	// Here we need to use an existing constructor, to inherit from the correct class.
	// eg. if it's a Node, we need to inherit from Node.
	// If it's a Node2D, we need to inherit from Node2D. etc.
	api += "    using " + parent_name + "::" + parent_name + ";\n";
	// We just need the names of the properties and methods.
	TypedArray<Dictionary> properties = class_db->class_get_property_list(class_name, true);
	Vector<String> property_names;
	for (int j = 0; j < properties.size(); j++) {
		Dictionary property = properties[j];
		String property_name = property["name"];
		const int type = int(property["type"]);
		// Properties that are likely just groups or categories.
		if (type == Variant::NIL) {
			continue;
		}
		// Skip properties with spaces in the name. Yes, this is a thing.
		if (property_name.is_empty() || property_name.contains(" ") || property_name.contains("/") || property_name.contains("-")) {
			continue;
		}
		// Yes, this is a thing too.
		if (property_name == class_name) {
			continue;
		}
		// If matching C++ keywords, capitalize the first letter.
		if (cpp_keywords.has(property_name.to_lower())) {
			property_name = property_name.capitalize();
		}

		String property_type = cpp_compatible_variant_type(type);
		if (property_type == "Variant") {
			api += String("    PROPERTY(") + property_name + ");\n";
		} else {
			api += String("    TYPED_PROPERTY(") + property_name + ", " + property_type + ");\n";
		}
		property_names.push_back(property_name);
	}
	TypedArray<Dictionary> methods = class_db->class_get_method_list(class_name, true);
	for (int j = 0; j < methods.size(); j++) {
		Dictionary method = methods[j];
		String method_name = method["name"];
		Dictionary return_value = method["return"];
		const int type = int(return_value["type"]);
		// Skip methods that are empty, and methods with '/' and '-' in the name.
		if (method_name.is_empty() || method_name.contains("/") || method_name.contains("-")) {
			continue;
		}
		// Skip methods that are set/get property-methods, or empty.
		if (method_name.begins_with("set_") || method_name.begins_with("get_")) {
			// But only if we know it's a property.
			if (property_names.has(method_name.substr(4)))
				continue;
		}
		if (method_name.begins_with("is_")) {
			// But only if we know it's a property.
			if (property_names.has(method_name.substr(3)))
				continue;
		}
		// If matching C++ keywords, capitalize the first letter.
		if (cpp_keywords.has(method_name.to_lower())) {
			method_name = method_name.capitalize();
		}

		api += String("    TYPED_METHOD(") + cpp_compatible_variant_type(type) + ", " + method_name + ");\n";
	}

	if (singletons.has(class_name)) {
		api += "    static " + class_name + " get_singleton() { return " + class_name + "(Object(\"" + class_name + "\").address()); }\n";
	}

	api += "};\n";
	return api;
}

void Sandbox::generate_runtime_cpp_api() {
	// 1. Get all classes currently registered with the engine.
	// 2. Get all methods and properties for each class.
	// 3. Generate a simple API for each class using METHOD() and PROPERTY() macros to a string.
	// 4. Print the generated API to the console.
	if constexpr (VERBOSE) {
		UtilityFunctions::print("* Generating C++ run-time API");
	}
	if (current_generated_api != nullptr) {
		delete current_generated_api;
	}
	current_generated_api = new String("#pragma once\n\n#include <api.hpp>\n#define GENERATED_API 1\n\n");

	HashSet<String> cpp_keywords;
	cpp_keywords.insert("class");
	cpp_keywords.insert("operator");
	cpp_keywords.insert("new");
	cpp_keywords.insert("delete");
	cpp_keywords.insert("this");
	cpp_keywords.insert("virtual");
	cpp_keywords.insert("override");
	cpp_keywords.insert("final");
	cpp_keywords.insert("public");
	cpp_keywords.insert("protected");
	cpp_keywords.insert("private");
	cpp_keywords.insert("static");
	cpp_keywords.insert("const");

	// 1. Get all classes currently registered with the engine.
	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	Array classes = class_db->get_class_list();

	HashSet<String> emitted_classes;
	HashMap<String, TypedArray<String>> waiting_classes;

	// 2. Insert all pre-existing classes into the emitted_classes set.
	emitted_classes.insert("Object");
	emitted_classes.insert("Node");
	emitted_classes.insert("CanvasItem");
	emitted_classes.insert("Node2D");
	emitted_classes.insert("Node3D");
	// Also skip some classes we simply don't want to expose.
	emitted_classes.insert("ClassDB");
	// Finally, add singleton getters to certain classes.
	HashSet<String> singletons;
	singletons.insert("Engine");
	singletons.insert("Time");
	singletons.insert("Input");

	// 3. Get all methods and properties for each class.
	for (int i = 0; i < classes.size(); i++) {
		String class_name = classes[i];

		// 4. Emit classes
		// Check if the class is already emitted.
		if (emitted_classes.has(class_name)) {
			continue;
		}

		// Check if the parent class has been emitted, and if not, add it to the waiting_classes map.
		String parent_name = class_db->get_parent_class(class_name);
		if (!emitted_classes.find(parent_name)) { // Not emitted yet.
			TypedArray<String> *waiting = waiting_classes.getptr(parent_name);
			if (waiting == nullptr) {
				waiting_classes.insert(parent_name, TypedArray<String>());
				waiting = waiting_classes.getptr(parent_name);
			}
			waiting->push_back(class_name);
			continue;
		}
		// Emit the class.
		*current_generated_api += emit_class(class_db, cpp_keywords, singletons, class_name);
		emitted_classes.insert(class_name);
	}

	// 5. Emit waiting classes.
	while (!waiting_classes.is_empty()) {
		const int initial_waiting_classes = waiting_classes.size();

		for (auto it = waiting_classes.begin(); it != waiting_classes.end(); ++it) {
			const String &parent_name = it->key;
			if (emitted_classes.has(parent_name)) {
				const TypedArray<String> &waiting = it->value;
				for (int i = 0; i < waiting.size(); i++) {
					String class_name = waiting[i];
					*current_generated_api += emit_class(class_db, cpp_keywords, singletons, class_name);
					emitted_classes.insert(class_name);
				}
				waiting_classes.erase(parent_name);
				break;
			}
		}

		const int remaining_waiting_classes = waiting_classes.size();
		if (remaining_waiting_classes == initial_waiting_classes) {
			// We have a circular dependency.
			// This is a bug in the engine, and should be reported.
			// We can't continue, so we'll just break out of the loop.
			ERR_PRINT("Circular dependency detected in class inheritance");
			for (auto it = waiting_classes.begin(); it != waiting_classes.end(); ++it) {
				const String &parent_name = it->key;
				const TypedArray<String> &waiting = it->value;
				for (int i = 0; i < waiting.size(); i++) {
					ERR_PRINT("* Waiting class " + String(waiting[i]) + " with parent " + parent_name);
				}
			}
			break;
		}
	}

	if constexpr (VERBOSE) {
		UtilityFunctions::print("* Finished generating " + itos(classes.size()) + " classes");
	}
}
