#include "docker.h"

#include "sandbox_project_settings.h"
#include <godot_cpp/classes/os.hpp>
static constexpr bool VERBOSE_CMD = true;
using namespace godot;

bool Docker::ContainerStart(String container_name, String image_name, Array &output) {
	godot::OS *OS = godot::OS::get_singleton();
	PackedStringArray arguments = { "run", "--name", container_name, "-dv", ".:/usr/src", image_name };
	if constexpr (VERBOSE_CMD) {
		UtilityFunctions::print(SandboxProjectSettings::get_docker_path(), arguments);
	}
	const int res = OS->execute(SandboxProjectSettings::get_docker_path(), arguments, output);
	return res == 0;
}

Array Docker::ContainerStop(String container_name) {
	godot::OS *OS = godot::OS::get_singleton();
	PackedStringArray arguments = { "stop", container_name, "--time", "1" };
	if constexpr (VERBOSE_CMD) {
		UtilityFunctions::print(SandboxProjectSettings::get_docker_path(), arguments);
	}
	Array output;
	OS->execute(SandboxProjectSettings::get_docker_path(), arguments, output);
	return output;
}

bool Docker::ContainerExecute(String container_name, const PackedStringArray &p_arguments, Array &output) {
	godot::OS *OS = godot::OS::get_singleton();
	PackedStringArray arguments = { "exec", "-t", container_name, "bash", "/usr/api/build.sh" };
	for (int i = 0; i < p_arguments.size(); i++) {
		arguments.push_back(p_arguments[i]);
	}
	if constexpr (VERBOSE_CMD) {
		UtilityFunctions::print(SandboxProjectSettings::get_docker_path(), arguments);
	}
	const int res = OS->execute(SandboxProjectSettings::get_docker_path(), arguments, output);
	return res == 0;
}
