#include "resource_saver_cpp.h"
#include "script_cpp.h"

Error ResourceFormatSaverCPP::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	return Error::OK;
}
Error ResourceFormatSaverCPP::_set_uid(const String &p_path, int64_t p_uid) {
	return Error::OK;
}
bool ResourceFormatSaverCPP::_recognize(const Ref<Resource> &p_resource) const {
	return false;
}
PackedStringArray ResourceFormatSaverCPP::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	return PackedStringArray();
}
bool ResourceFormatSaverCPP::_recognize_path(const Ref<Resource> &p_resource, const String &p_path) const {
	return false;
}