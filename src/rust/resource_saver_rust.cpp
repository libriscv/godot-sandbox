#include "resource_saver_rust.h"
#include "../elf/script_elf.h"
#include "../elf/script_language_elf.h"
#include "../../register_types.h"
#include "../sandbox_project_settings.h"
#include "script_rust.h"
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/script_editor_base.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

static Ref<ResourceFormatSaverRust> rust_saver;
static constexpr bool VERBOSE_CMD = false;

void ResourceFormatSaverRust::init() {
	rust_saver.instantiate();
	ResourceSaver::get_singleton()->add_resource_format_saver(rust_saver);
}

void ResourceFormatSaverRust::deinit() {
	ResourceSaver::get_singleton()->remove_resource_format_saver(rust_saver);
	rust_saver.unref();
}

Error ResourceFormatSaverRust::GODOT_CPP_FUNC (save)(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	RustScript *script = Object::cast_to<RustScript>(p_resource.ptr());
	if (script != nullptr) {
		Ref<FileAccess> handle = FileAccess::open(p_path, FileAccess::ModeFlags::WRITE);
		if (handle.is_valid()) {
			handle->store_string(script->_get_source_code());
			handle->close();
			// Get the absolute path without the file name
			String path = handle->get_path().get_base_dir().replace("res://", "") + "/";
			String inpname = path + "*.rs";
			String foldername = Docker::GetFolderName(handle->get_path().get_base_dir());
			String outname = path + foldername + String(".elf");

			// Lazily start the docker container
			RustScript::DockerContainerStart();

			auto builder = [inpname = std::move(inpname), outname = std::move(outname)] {
				// Invoke docker to compile the file
				Array output;
				RustScript::DockerContainerExecute({ "/usr/project/build.sh", "-o", outname, inpname }, output);
				if (!output.is_empty() && !output[0].operator String().is_empty()) {
					for (int i = 0; i < output.size(); i++) {
						String line = output[i].operator String();
						if constexpr (VERBOSE_CMD)
							ERR_PRINT(line);
						// Remove (most) console color codes
						line = line.replace("\033[0;31m", "");
						line = line.replace("\033[0;32m", "");
						line = line.replace("\033[0;33m", "");
						line = line.replace("\033[0;34m", "");
						line = line.replace("\033[0;35m", "");
						line = line.replace("\033[0;36m", "");
						line = line.replace("\033[0;37m", "");
						line = line.replace("\033[01;31m", "");
						line = line.replace("\033[01;32m", "");
						line = line.replace("\033[01;33m", "");
						line = line.replace("\033[01;34m", "");
						line = line.replace("\033[01;35m", "");
						line = line.replace("\033[01;36m", "");
						line = line.replace("\033[01;37m", "");
						line = line.replace("\033[m", "");
						line = line.replace("\033[0m", "");
						line = line.replace("\033[01m", "");
						line = line.replace("[K", "");
						line = line.replace("[1m", "");
						line = line.replace("[38;5;14m", "");
						line = line.replace("[38;5;12m", "");
						line = line.replace("[38;5;10m", "");
						line = line.replace("[38;5;9m", "");
						line = line.replace("[32m", "");
						line = line.replace("[33m", "");
						WARN_PRINT(line);
					}
				}
			};
			builder();
			// EditorInterface::get_singleton()->get_editor_settings()->set("text_editor/behavior/files/auto_reload_scripts_on_external_change", true);
			EditorInterface::get_singleton()->get_resource_filesystem()->scan();
			TypedArray<Script> open_scripts = EditorInterface::get_singleton()->get_script_editor()->get_open_scripts();
			for (int i = 0; i < open_scripts.size(); i++) {
				ELFScript *elf_script = Object::cast_to<ELFScript>(open_scripts[i]);
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
Error ResourceFormatSaverRust::GODOT_CPP_FUNC (set_uid)(const String &p_path, int64_t p_uid) {
	return Error::OK;
}
bool ResourceFormatSaverRust::GODOT_CPP_FUNC (recognize)(const Ref<Resource> &p_resource) const {
	return Object::cast_to<RustScript>(p_resource.ptr()) != nullptr;
}
PackedStringArray ResourceFormatSaverRust::GODOT_CPP_FUNC (get_recognized_extensions)(const Ref<Resource> &p_resource) const {
	PackedStringArray array;
	if (Object::cast_to<RustScript>(p_resource.ptr()) == nullptr)
		return array;
	array.push_back("rs");
	return array;
}
bool ResourceFormatSaverRust::GODOT_CPP_FUNC (recognize_path)(const Ref<Resource> &p_resource, const String &p_path) const {
	return Object::cast_to<RustScript>(p_resource.ptr()) != nullptr;
}
