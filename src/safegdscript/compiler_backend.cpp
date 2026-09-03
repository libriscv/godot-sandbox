#include "compiler_backend.h"
#include "../sandbox_project_settings.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <cstring>

#ifndef SAFEGDSCRIPT_COMPILER_POLICY
#define SAFEGDSCRIPT_COMPILER_POLICY 0
#endif

namespace gdscript_compiler {

Policy policy() {
	static const Policy effective = []() {
		constexpr Policy requested = Policy(SAFEGDSCRIPT_COMPILER_POLICY);
		if (requested != Policy::ALWAYS_SANDBOXED && direct_compiler_backend() == nullptr) {
			ERR_PRINT("SafeGDScript: this build has no in-process GDScript compiler; "
					  "compiling every script in the compiler sandbox instead.");
			return Policy::ALWAYS_SANDBOXED;
		}
		return requested;
	}();
	return effective;
}

const char *policy_name() {
	switch (policy()) {
		case Policy::ALWAYS_SANDBOXED:
			return "always-sandboxed";
		case Policy::SANDBOX_RESTRICTED:
			return "sandbox-restricted";
		case Policy::ALWAYS_DIRECT:
			return "always-direct";
	}
	return "unknown";
}

GDScriptCompilerBackend &backend_for(bool p_restricted) {
	switch (policy()) {
		case Policy::ALWAYS_SANDBOXED:
			break;
		case Policy::SANDBOX_RESTRICTED:
			if (!p_restricted) {
				return *direct_compiler_backend();
			}
			break;
		case Policy::ALWAYS_DIRECT:
			return *direct_compiler_backend();
	}
	return sandboxed_compiler_backend();
}

static PackedStringArray project_autoload_names() {
	PackedStringArray names;
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (settings == nullptr) {
		return names;
	}
	const TypedArray<Dictionary> properties = settings->get_property_list();
	for (int i = 0; i < properties.size(); i++) {
		const Dictionary property = properties[i];
		const String name = property.get("name", String());
		if (name.begins_with("autoload/")) {
			names.push_back(name.substr(strlen("autoload/")));
		}
	}
	return names;
}

static PackedStringArray project_global_classes() {
	PackedStringArray pairs;
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (settings == nullptr) {
		return pairs;
	}
	const TypedArray<Dictionary> classes = settings->get_global_class_list();
	for (int i = 0; i < classes.size(); i++) {
		const Dictionary entry = classes[i];
		const String class_name = entry.get("class", String());
		const String path = entry.get("path", String());
		if (class_name.is_empty() || path.is_empty()) {
			continue;
		}
		pairs.push_back(class_name);
		pairs.push_back(path);
	}
	return pairs;
}

static PackedStringArray engine_ancestry() {
	PackedStringArray pairs;
	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	if (class_db == nullptr) return pairs;
	for (const String &name : class_db->get_class_list()) {
		String ancestors;
		String current = name;
		for (int depth = 0; depth < 256; depth++) {
			const String parent = class_db->get_parent_class(current);
			if (parent.is_empty() || parent == current) break;
			if (!ancestors.is_empty()) ancestors += ",";
			ancestors += parent;
			current = parent;
		}
		if (!ancestors.is_empty()) {
			pairs.push_back(name);
			pairs.push_back(ancestors);
		}
	}
	return pairs;
}

void prepare(GDScriptCompilerBackend &p_backend, bool p_restricted,
		const PackedStringArray &p_base_sources, const String &p_source_path,
		bool p_emit_tests) {
	p_backend.set_restricted(p_restricted);
	p_backend.set_source_path(p_source_path);
	p_backend.set_autoloads(project_autoload_names());
	p_backend.set_global_classes(project_global_classes());
	p_backend.set_base_sources(p_base_sources);
	p_backend.set_engine_ancestry(engine_ancestry());
	p_backend.set_trait_structural_fallback(SandboxProjectSettings::trait_structural_fallback());
	p_backend.set_emit_tests(p_emit_tests);
}

} // namespace gdscript_compiler
