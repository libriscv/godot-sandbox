
#include "sandbox_project_settings.h"

#include <godot_cpp/classes/project_settings.hpp>

using namespace godot;

static constexpr char USE_GLOBAL_NAMES[] = "editor/script/use_global_sandbox_names";
static constexpr char USE_GLOBAL_NAMES_HINT[] = "Use customized global names for Sandbox programs";

static constexpr char DOCKER_ENABLED[] = "editor/script/docker_enabled";
static constexpr char DOCKER_ENABLED_HINT[] = "Enable Docker for compilation";
static constexpr char DOCKER_PATH[] = "editor/script/docker";
static constexpr char DOCKER_PATH_HINT[] = "Path to the Docker executable";

static constexpr char ASYNC_COMPILATION[] = "editor/script/async_compilation";
static constexpr char ASYNC_COMPILATION_HINT[] = "Compile scripts asynchronously";
static constexpr char NATIVE_TYPES[] = "editor/script/unboxed_types_for_sandbox_arguments";
static constexpr char NATIVE_TYPES_HINT[] = "Use native types and classes instead of Variants in Sandbox functions where possible";
static constexpr char DEBUG_INFO[] = "editor/script/debug_info";
static constexpr char DEBUG_INFO_HINT[] = "Enable debug information when building ELF files";
static constexpr char GLOBAL_DEFINES[] = "editor/script/global_defines";
static constexpr char GLOBAL_DEFINES_HINT[] = "Global defines used when compiling Sandbox programs";

static constexpr char METHOD_ARGUMENTS[] = "editor/script/runtime_api_method_arguments";
static constexpr char METHOD_ARGUMENTS_HINT[] = "Generate method arguments for the run-time API";

static void register_setting(
		const String &p_name,
		const Variant &p_value,
		bool p_needs_restart,
		PropertyHint p_hint,
		const String &p_hint_string) {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();

	if (!project_settings->has_setting(p_name)) {
		project_settings->set(p_name, p_value);
	}

	Dictionary property_info;
	property_info["name"] = p_name;
	property_info["type"] = p_value.get_type();
	property_info["hint"] = p_hint;
	property_info["hint_string"] = p_hint_string;

	project_settings->add_property_info(property_info);
	project_settings->set_initial_value(p_name, p_value);
	project_settings->set_restart_if_changed(p_name, p_needs_restart);

	// HACK(mihe): We want our settings to appear in the order we register them in, but if we start
	// the order at 0 we end up moving the entire `physics/` group to the top of the tree view, so
	// instead we give it a hefty starting order and increment from there, which seems to give us
	// the desired effect.
	static int32_t order = 1000000;

	project_settings->set_order(p_name, order++);
}

void register_setting_plain(
		const String &p_name,
		const Variant &p_value,
		const String &p_hint_string = "",
		bool p_needs_restart = false) {
	register_setting(p_name, p_value, p_needs_restart, PROPERTY_HINT_NONE, p_hint_string);
}

void SandboxProjectSettings::register_settings() {
	register_setting_plain(USE_GLOBAL_NAMES, true, USE_GLOBAL_NAMES_HINT, true);
	register_setting_plain(DOCKER_ENABLED, true, DOCKER_ENABLED_HINT, true);
#ifdef WIN32
	register_setting_plain(DOCKER_PATH, "C:\\Program Files\\Docker\\Docker\\bin\\", DOCKER_PATH_HINT, true);
#else
	register_setting_plain(DOCKER_PATH, "docker", DOCKER_PATH_HINT, true);
#endif
	register_setting_plain(ASYNC_COMPILATION, true, ASYNC_COMPILATION_HINT, false);
	register_setting_plain(NATIVE_TYPES, true, NATIVE_TYPES_HINT, false);
	register_setting_plain(DEBUG_INFO, false, DEBUG_INFO_HINT, false);
	register_setting_plain(GLOBAL_DEFINES, Array(), GLOBAL_DEFINES_HINT, false);
	register_setting_plain(METHOD_ARGUMENTS, false, METHOD_ARGUMENTS_HINT, false);
}

template <typename TType>
static TType get_setting(const char *p_setting) {
	const ProjectSettings *project_settings = ProjectSettings::get_singleton();
	const Variant setting_value = project_settings->get_setting_with_override(p_setting);
	const Variant::Type setting_type = setting_value.get_type();
	const Variant::Type expected_type = Variant(TType()).get_type();

	ERR_FAIL_COND_V(setting_type != expected_type, Variant());

	return setting_value;
}

bool SandboxProjectSettings::use_global_sandbox_names() {
	return get_setting<bool>(USE_GLOBAL_NAMES);
}

bool SandboxProjectSettings::get_docker_enabled() {
	return get_setting<bool>(DOCKER_ENABLED);
}

String SandboxProjectSettings::get_docker_path() {
	return get_setting<String>(DOCKER_PATH);
}

bool SandboxProjectSettings::async_compilation() {
	return get_setting<bool>(ASYNC_COMPILATION);
}

bool SandboxProjectSettings::use_native_types() {
	return get_setting<bool>(NATIVE_TYPES);
}

bool SandboxProjectSettings::debug_info() {
	return get_setting<bool>(DEBUG_INFO);
}

Array SandboxProjectSettings::get_global_defines() {
	return get_setting<Array>(GLOBAL_DEFINES);
}

bool SandboxProjectSettings::generate_method_arguments() {
	return get_setting<bool>(METHOD_ARGUMENTS);
}
