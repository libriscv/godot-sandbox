#pragma once

#include "../docker.h"
#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/classes/script_language.hpp>
#include <godot_cpp/templates/hash_set.hpp>

using namespace godot;
class CPPScriptInstance;
class ELFScript;
class ELFScriptInstance;

class CPPScript : public ScriptExtension {
	GDCLASS(CPPScript, ScriptExtension);

protected:
	static void _bind_methods() {}
	String source_code;

public:
	virtual bool _editor_can_reload_from_file() override;
	virtual void _placeholder_erased(void *p_placeholder) override;
	virtual bool _can_instantiate() const override;
	virtual Ref<Script> _get_base_script() const override;
	virtual StringName _get_global_name() const override;
	virtual bool _inherits_script(const Ref<Script> &p_script) const override;
	virtual StringName _get_instance_base_type() const override;
	virtual void *_instance_create(Object *p_for_object) const override;
	virtual void *_placeholder_instance_create(Object *p_for_object) const override;
	virtual bool _instance_has(Object *p_object) const override;
	virtual bool _has_source_code() const override;
	virtual String _get_source_code() const override;
	virtual void _set_source_code(const String &p_code) override;
	virtual Error _reload(bool p_keep_state) override;
	virtual TypedArray<Dictionary> _get_documentation() const override;
	virtual String _get_class_icon_path() const override;
	virtual bool _has_method(const StringName &p_method) const override;
	virtual bool _has_static_method(const StringName &p_method) const override;
	virtual Dictionary _get_method_info(const StringName &p_method) const override;
	virtual bool _is_tool() const override;
	virtual bool _is_valid() const override;
	virtual bool _is_abstract() const override;
	virtual ScriptLanguage *_get_language() const override;
	virtual bool _has_script_signal(const StringName &p_signal) const override;
	virtual TypedArray<Dictionary> _get_script_signal_list() const override;
	virtual bool _has_property_default_value(const StringName &p_property) const override;
	virtual Variant _get_property_default_value(const StringName &p_property) const override;
	virtual void _update_exports() override;
	virtual TypedArray<Dictionary> _get_script_method_list() const override;
	virtual TypedArray<Dictionary> _get_script_property_list() const override;
	virtual int32_t _get_member_line(const StringName &p_member) const override;
	virtual Dictionary _get_constants() const override;
	virtual TypedArray<StringName> _get_members() const override;
	virtual bool _is_placeholder_fallback_enabled() const override;
	virtual Variant _get_rpc_config() const override;

	/// @brief Detects if the project is a CMake or SCons project,
	/// in which case Docker usage is not necessary.
	/// @return true if the project is a CMake or SCons project.
	static bool DetectCMakeOrSConsProject();

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

	void set_file(const String &p_path);
	CPPScriptInstance *get_cpp_script_instance() const;
	const String &get_path() const { return path; }
	bool detect_script_instance();
	void remove_instance(CPPScriptInstance *p_instance);

	static String PathToGlobalName(const String &p_path) {
		return "CPPScript_" + p_path.get_basename().replace("res://", "").replace("/", "_").replace("-", "_").capitalize().replace(" ", "");
	}

	CPPScript();
	~CPPScript();

private:
	static inline bool docker_container_started = false;
	static inline int docker_container_version = 0;
	static inline const char *docker_container_name = "godot-cpp-compiler";
	static inline const char *docker_image_name = "ghcr.io/libriscv/cpp_compiler";

	String path;
	mutable HashSet<CPPScriptInstance *> instances;
	Ref<ELFScript> elf_script;
	friend class CPPScriptInstance;
};
