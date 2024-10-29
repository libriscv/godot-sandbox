#include "resource_loader_elf.h"
#include "script_elf.h"
#include <godot_cpp/classes/file_access.hpp>

Variant ResourceFormatLoaderELF::GODOT_CPP_FUNC (load)(const String &p_path, const String &original_path, bool use_sub_threads, int32_t cache_mode) const {
	Ref<ELFScript> elf_model = memnew(ELFScript);
	elf_model->set_file(p_path);
	elf_model->reload(false);
	return elf_model;
}
PackedStringArray ResourceFormatLoaderELF::GODOT_CPP_FUNC (get_recognized_extensions)() const {
	PackedStringArray array;
	array.push_back("elf");
	return array;
}
bool ResourceFormatLoaderELF::GODOT_CPP_FUNC (recognize_path)(const String &path, const StringName &type) const {
	String el = path.get_extension().to_lower();
	if (el == "elf") {
		return true;
	}
	return false;
}
bool ResourceFormatLoaderELF::GODOT_CPP_FUNC (handles_type)(const StringName &type) const {
	String type_str = type;
	return type_str == "ELFScript" || type_str == "Script";
}
String ResourceFormatLoaderELF::GODOT_CPP_FUNC (get_resource_type)(const String &p_path) const {
	String el = p_path.get_extension().to_lower();
	if (el == "elf") {
		return "ELFScript";
	}
	return "";
}
