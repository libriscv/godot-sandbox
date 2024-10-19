#include "resource_loader_elf.h"
#include "script_elf.h"
#include <godot_cpp/classes/file_access.hpp>

Variant ResourceFormatLoaderELF::_load(const String &p_path, const String &original_path, bool use_sub_threads, int32_t cache_mode) const {
	Ref<ELFScript> elf_model = memnew(ELFScript);
	elf_model->set_file(p_path);
	elf_model->reload(false);
	return elf_model;
}
PackedStringArray ResourceFormatLoaderELF::_get_recognized_extensions() const {
	PackedStringArray array;
	array.push_back("elf");
	return array;
}
bool ResourceFormatLoaderELF::_recognize_path(const godot::String &path, const godot::StringName &type) const {
	String el = path.get_extension().to_lower();
	if (el == "elf") {
		return true;
	}
	return false;
}
bool ResourceFormatLoaderELF::_handles_type(const StringName &type) const {
	String type_str = type;
	return type_str == "ELFScript" || type_str == "Script";
}
String ResourceFormatLoaderELF::_get_resource_type(const String &p_path) const {
	String el = p_path.get_extension().to_lower();
	if (el == "elf") {
		return "ELFScript";
	}
	return "";
}
