#include "docker.h"

#include "sandbox_project_settings.h"
#include <godot_cpp/classes/os.hpp>
static constexpr bool VERBOSE_CMD = true;
using namespace godot;

static bool ContainerIsAlreadyRunning(String container_name) {
	godot::OS *OS = godot::OS::get_singleton();
	PackedStringArray arguments = { "container", "inspect", "-f", "{{.State.Running}}", container_name };
	Array output;
	if constexpr (VERBOSE_CMD) {
		UtilityFunctions::print(SandboxProjectSettings::get_docker_path(), arguments);
	}
	const int res = OS->execute(SandboxProjectSettings::get_docker_path(), arguments, output);
	if (res != 0) {
		return false;
	}
	const String running = output[0];
	return running.contains("true");
}

bool Docker::ContainerPullLatest(String image_name, Array &output) {
	godot::OS *OS = godot::OS::get_singleton();
	PackedStringArray arguments = { "pull", image_name };
	if constexpr (VERBOSE_CMD) {
		UtilityFunctions::print(SandboxProjectSettings::get_docker_path(), arguments);
	}
	const int res = OS->execute(SandboxProjectSettings::get_docker_path(), arguments, output);
	return res == 0;
}

bool Docker::ContainerStart(String container_name, String image_name, Array &output) {
	if (ContainerIsAlreadyRunning(container_name)) {
		return true;
	}
	// The container is not running. Try to pull the latest image.
	Array dont_care; // We don't care about the output of the image pull (for now).
	if (ContainerPullLatest(image_name, dont_care)) {
		// Delete the container if it exists. It's not running, but it might be stopped.
		ContainerDelete(container_name, dont_care);
	} else {
		WARN_PRINT("Sandbox: Failed to pull the latest container image: " + image_name);
	}
	// Start the container, even if the image pull failed. It might be locally available.
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
	PackedStringArray arguments = { "exec", "-t", container_name, "bash" };
	for (int i = 0; i < p_arguments.size(); i++) {
		arguments.push_back(p_arguments[i]);
	}
	if constexpr (VERBOSE_CMD) {
		UtilityFunctions::print(SandboxProjectSettings::get_docker_path(), arguments);
	}
	const int res = OS->execute(SandboxProjectSettings::get_docker_path(), arguments, output);
	return res == 0;
}

int Docker::ContainerVersion(String container_name, const PackedStringArray &p_arguments) {
	// Execute --version in the container.
	Array output;
	if (ContainerExecute(container_name, p_arguments, output)) {
		// Docker container responds with a number, eg "1" (ASCII)
		return output[0].operator String().to_int();
	}
	return -1;
}

bool Docker::ContainerDelete(String container_name, Array &output) {
	godot::OS *OS = godot::OS::get_singleton();
	PackedStringArray arguments = { "rm", container_name };
	if constexpr (VERBOSE_CMD) {
		UtilityFunctions::print(SandboxProjectSettings::get_docker_path(), arguments);
	}
	const int res = OS->execute(SandboxProjectSettings::get_docker_path(), arguments, output);
	return res == 0;
}
