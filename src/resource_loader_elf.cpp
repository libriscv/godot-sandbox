#include "resource_loader_whisper.h"
#include "resource_whisper.h"

Variant ResourceFormatLoaderELF::_load(const String &p_path, const String &original_path, bool use_sub_threads, int32_t cache_mode) const {
	Ref<ELFResource> whisper_model = memnew(ELFResource);
	whisper_model->set_file(p_path);
	return whisper_model;
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
