#include "resource_saver_cpp.h"
#include "../elf/script_elf.h"
#include "../elf/script_language_elf.h"
#include "../register_types.h"
#include "../sandbox_project_settings.h"
#include "script_cpp.h"
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/script_editor_base.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

// New Game Project$ docker run --name godot-cpp-compiler -dv .:/usr/src ghcr.io/libriscv/compiler

Error ResourceFormatSaverCPP::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	CPPScript *script = Object::cast_to<CPPScript>(p_resource.ptr());
	if (script != nullptr) {
		Ref<FileAccess> handle = FileAccess::open(p_path, FileAccess::ModeFlags::WRITE);
		if (handle.is_valid()) {
			handle->store_string(script->_get_source_code());
			handle->close();
			// Get the absolute path without the file name
			String path = handle->get_path().get_base_dir().replace("res://", "") + "/";
			String inpname = path + "*.cpp";
			String foldername = handle->get_path().get_base_dir().replace("res://", "");
			String outname = path + foldername + String(".elf");

			// Lazily start the docker container
			CPPScript::DockerContainerStart();

			auto builder = [inpname = std::move(inpname), outname = std::move(outname)] {
				// Invoke docker to compile the file
				Array output;
				CPPScript::DockerContainerExecute({ "-o", outname, inpname }, output);
				for (int i = 0; i < output.size(); i++) {
					UtilityFunctions::print(output[i]);
				}
			};
			builder();
			EditorInterface::get_singleton()->get_resource_filesystem()->scan();
			auto open_scripts = EditorInterface::get_singleton()->get_script_editor()->get_open_scripts();
			for (int i = 0; i < open_scripts.size(); i++) {
				auto elf_script = Object::cast_to<ELFScript>(open_scripts[i]);
				if (elf_script) {
					elf_script->reload(false);
					elf_script->emit_changed();
				}
			}
			return Error::OK;
		} else {
			return Error::ERR_FILE_CANT_OPEN;
		}
	}
	return Error::ERR_SCRIPT_FAILED;
}
Error ResourceFormatSaverCPP::_set_uid(const String &p_path, int64_t p_uid) {
	return Error::OK;
}
bool ResourceFormatSaverCPP::_recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<CPPScript>(p_resource.ptr()) != nullptr;
}
PackedStringArray ResourceFormatSaverCPP::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	PackedStringArray array;
	array.push_back("cpp");
	array.push_back("cc");
	array.push_back("hh");
	array.push_back("h");
	array.push_back("hpp");
	return array;
}
bool ResourceFormatSaverCPP::_recognize_path(const Ref<Resource> &p_resource, const String &p_path) const {
	return Object::cast_to<CPPScript>(p_resource.ptr()) != nullptr;
}
