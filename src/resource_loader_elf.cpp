#include "resource_loader_elf.h"
#include "resource_elf.h"

Variant ResourceFormatLoaderELF::_load(const String &p_path, const String &original_path, bool use_sub_threads, int32_t cache_mode) const {
	Ref<ELFResource> elf_model = memnew(ELFResource);
	elf_model->set_file(p_path);
	return elf_model;
}
PackedStringArray ResourceFormatLoaderELF::_get_recognized_extensions() const {
	PackedStringArray array;
	array.push_back("elf");
	return array;
}
bool ResourceFormatLoaderELF::_handles_type(const StringName &type) const {
	return ClassDB::is_parent_class(type, "ELFResource");
}
String ResourceFormatLoaderELF::_get_resource_type(const String &p_path) const {
	String el = p_path.get_extension().to_lower();
	if (el == "elf") {
		return "ELFResource";
	}
	return "";
}
