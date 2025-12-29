#include "resource_loader_safegdscript.h"
#include "script_safegdscript.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

static Ref<ResourceFormatLoaderSafeGDScript> loader;

void ResourceFormatLoaderSafeGDScript::init() {
	loader.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(loader, true);
}

void ResourceFormatLoaderSafeGDScript::deinit() {
	if (loader.is_valid()) {
		ResourceLoader::get_singleton()->remove_resource_format_loader(loader);
		loader.unref();
	}
}

Variant ResourceFormatLoaderSafeGDScript::_load(const String &p_path, const String &p_original_path, bool p_use_sub_threads, int32_t p_cache_mode) const {
	Ref<SafeGDScript> script;
	script.instantiate();
	script->set_path(p_path);
	return script;
}

PackedStringArray ResourceFormatLoaderSafeGDScript::_get_recognized_extensions() const {
	PackedStringArray array;
	array.push_back("sgd");
	array.push_back("safegd");
	return array;
}

bool ResourceFormatLoaderSafeGDScript::_handles_type(const StringName &p_type) const {
	return p_type == StringName("SafeGDScript") || p_type == StringName("Script");
}

String ResourceFormatLoaderSafeGDScript::_get_resource_type(const String &p_path) const {
	String el = p_path.get_extension().to_lower();
	if (el == "sgd" || el == "safegd") {
		return "SafeGDScript";
	}
	return "";
}
