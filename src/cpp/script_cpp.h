#pragma once

#include "../docker.h"
#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/classes/script_language.hpp>

GODOT_NAMESPACE

class CPPScript : public ScriptExtension {
	GDCLASS(CPPScript, ScriptExtension);

protected:
	static void GODOT_CPP_FUNC (bind_methods)() {}
	String source_code;

public:
	virtual bool GODOT_CPP_FUNC (editor_can_reload_from_file)() override;
	virtual void GODOT_CPP_FUNC (placeholder_erased) (void *p_placeholder) override;
	virtual bool GODOT_CPP_FUNC (can_instantiate)() const override;
	virtual Ref<Script> GODOT_CPP_FUNC (get_base_script)() const override;
	virtual StringName GODOT_CPP_FUNC (get_global_name)() const override;
	virtual bool GODOT_CPP_FUNC (inherits_script)(const Ref<Script> &p_script) const override;
	virtual StringName GODOT_CPP_FUNC (get_instance_base_type)() const override;
	virtual void *_instance_create(Object *p_for_object) const override;
	virtual void *_placeholder_instance_create(Object *p_for_object) const override;
	virtual bool GODOT_CPP_FUNC (instance_has)(Object *p_object) const override;
	virtual bool GODOT_CPP_FUNC (has_source_code)() const override;
	virtual String GODOT_CPP_FUNC (get_source_code)() const override;
	virtual void GODOT_CPP_FUNC (set_source_code)(const String &p_code) override;
	virtual Error GODOT_CPP_FUNC (reload)(bool p_keep_state) override;
	virtual TypedArray<Dictionary> GODOT_CPP_FUNC (get_documentation)() const override;
	virtual String GODOT_CPP_FUNC (get_class_icon_path)() const override;
	virtual bool GODOT_CPP_FUNC (has_method)(const StringName &p_method) const override;
	virtual bool GODOT_CPP_FUNC (has_static_method)(const StringName &p_method) const override;
	virtual Dictionary GODOT_CPP_FUNC (get_method_info)(const StringName &p_method) const override;
	virtual bool GODOT_CPP_FUNC (is_tool)() const override;
	virtual bool GODOT_CPP_FUNC (is_valid)() const override;
	virtual bool GODOT_CPP_FUNC (is_abstract)() const override;
	virtual ScriptLanguage *_get_language() const override;
	virtual bool GODOT_CPP_FUNC (has_script_signal)(const StringName &p_signal) const override;
	virtual TypedArray<Dictionary> GODOT_CPP_FUNC (get_script_signal_list)() const override;
	virtual bool GODOT_CPP_FUNC (has_property_default_value)(const StringName &p_property) const override;
	virtual Variant GODOT_CPP_FUNC (get_property_default_value)(const StringName &p_property) const override;
	virtual void GODOT_CPP_FUNC (update_exports)() override;
	virtual TypedArray<Dictionary> GODOT_CPP_FUNC (get_script_method_list)() const override;
	virtual TypedArray<Dictionary> GODOT_CPP_FUNC (get_script_property_list)() const override;
	virtual int32_t GODOT_CPP_FUNC (get_member_line)(const StringName &p_member) const override;
	virtual Dictionary GODOT_CPP_FUNC (get_constants)() const override;
	virtual TypedArray<StringName> GODOT_CPP_FUNC (get_members)() const override;
	virtual bool GODOT_CPP_FUNC (is_placeholder_fallback_enabled)() const override;
	virtual Variant GODOT_CPP_FUNC (get_rpc_config)() const override;

	static void DockerContainerStart() {
		if (!docker_container_started) {
			Array output;
			if (Docker::ContainerStart(docker_container_name, docker_image_name, output))
				docker_container_started = true;
			else {
				UtilityFunctions::print(output);
				ERR_PRINT("Failed to start Docker container: " + String(docker_container_name));
			}
		}
	}
	static void DockerContainerStop() {
		if (docker_container_started) {
			Docker::ContainerStop(docker_container_name);
			docker_container_started = false;
		}
	}
	static int DockerContainerVersion() {
		DockerContainerStart();
		if (docker_container_version == 0)
			docker_container_version =
					Docker::ContainerVersion(docker_container_name, { "/usr/api/build.sh", "--version" });
		return docker_container_version;
	}
	static bool DockerContainerExecute(const PackedStringArray &p_arguments, Array &output, bool verbose = true) {
		DockerContainerStart();
		return Docker::ContainerExecute(docker_container_name, p_arguments, output, verbose);
	}

	CPPScript();
	~CPPScript() {}

private:
	static inline bool docker_container_started = false;
	static inline int docker_container_version = 0;
	static inline const char *docker_container_name = "godot-cpp-compiler";
	static inline const char *docker_image_name = "ghcr.io/libriscv/cpp_compiler";
};
