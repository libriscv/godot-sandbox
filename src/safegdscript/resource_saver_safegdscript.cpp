#include "resource_saver_safegdscript.h"
#include "script_safegdscript.h"
#include <godot_cpp/classes/file_access.hpp>

static Ref<ResourceFormatSaverSafeGDScript> saver;

void ResourceFormatSaverSafeGDScript::init() {
	saver.instantiate();
	ResourceSaver::get_singleton()->add_resource_format_saver(saver, true);
}

void ResourceFormatSaverSafeGDScript::deinit() {
	if (saver.is_valid()) {
		ResourceSaver::get_singleton()->remove_resource_format_saver(saver);
		saver.unref();
	}
}

Error ResourceFormatSaverSafeGDScript::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	SafeGDScript *script = Object::cast_to<SafeGDScript>(p_resource.ptr());
	if (script == nullptr) {
		ERR_PRINT("ResourceFormatSaverSafeGDScript::_save: Resource is not a SafeGDScript.");
		return ERR_FILE_CANT_WRITE;
	}

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::ModeFlags::WRITE);
	if (file.is_null()) {
		ERR_PRINT("ResourceFormatSaverSafeGDScript::_save: Failed to open file for writing: " + p_path);
		return ERR_FILE_CANT_OPEN;
	}

	file->store_string(script->_get_source_code());
	return Error::OK;
}

Error ResourceFormatSaverSafeGDScript::_set_uid(const String &p_path, int64_t p_uid) {
	return Error::OK;
}

bool ResourceFormatSaverSafeGDScript::_recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<SafeGDScript>(p_resource.ptr()) != nullptr;
}

PackedStringArray ResourceFormatSaverSafeGDScript::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	PackedStringArray array;
	if (Object::cast_to<SafeGDScript>(p_resource.ptr()) == nullptr)
		return array;
	array.push_back("sgd");
	return array;
}

bool ResourceFormatSaverSafeGDScript::_recognize_path(const Ref<Resource> &p_resource, const String &p_path) const {
	return Object::cast_to<SafeGDScript>(p_resource.ptr()) != nullptr;
}
