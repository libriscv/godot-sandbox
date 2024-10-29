#include "resource_saver_elf.h"
#include "../../register_types.h"
#include "script_elf.h"
#include "script_language_elf.h"
#include <godot_cpp/classes/file_access.hpp>

Error ResourceFormatSaverELF::GODOT_CPP_FUNC (save)(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	// Do not save, revert instead
	Ref<ELFScript> elf_model = Object::cast_to<ELFScript>(p_resource.ptr());
	if (elf_model.is_null()) {
		return Error::OK;
	}
	elf_model->set_file(p_path);
	elf_model->reload(true);
	return Error::OK;
}
Error ResourceFormatSaverELF::GODOT_CPP_FUNC (set_uid)(const String &p_path, int64_t p_uid) {
	return Error::OK;
}
bool ResourceFormatSaverELF::GODOT_CPP_FUNC (recognize)(const Ref<Resource> &p_resource) const {
	return Object::cast_to<ELFScript>(p_resource.ptr()) != nullptr;
}
PackedStringArray ResourceFormatSaverELF::GODOT_CPP_FUNC (get_recognized_extensions)(const Ref<Resource> &p_resource) const {
	PackedStringArray array;
	if (Object::cast_to<ELFScript>(p_resource.ptr()) == nullptr)
		return array;
	array.push_back("elf");
	return array;
}
bool ResourceFormatSaverELF::GODOT_CPP_FUNC (recognize_path)(const Ref<Resource> &p_resource, const String &p_path) const {
	return Object::cast_to<ELFScript>(p_resource.ptr()) != nullptr;
}
