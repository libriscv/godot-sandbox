#pragma once

#include <godot_cpp/classes/resource_format_saver.hpp>
#include <godot_cpp/classes/resource_saver.hpp>

GODOT_NAMESPACE

class ResourceFormatSaverZig : public ResourceFormatSaver {
	GDCLASS(ResourceFormatSaverZig, ResourceFormatSaver);

protected:
	static void GODOT_CPP_FUNC (bind_methods)() {}

public:
	static void init();
	static void deinit();
	virtual Error GODOT_CPP_FUNC (save)(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) override;
	virtual Error GODOT_CPP_FUNC (set_uid)(const String &p_path, int64_t p_uid) override;
	virtual bool GODOT_CPP_FUNC (recognize)(const Ref<Resource> &p_resource) const override;
	virtual PackedStringArray GODOT_CPP_FUNC (get_recognized_extensions)(const Ref<Resource> &p_resource) const override;
	virtual bool GODOT_CPP_FUNC (recognize_path)(const Ref<Resource> &p_resource, const String &p_path) const override;
};
