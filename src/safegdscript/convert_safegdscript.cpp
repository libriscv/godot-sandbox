#include "script_language_safegdscript.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>

// .gd <-> .sgd conversion. The two languages share their source text, so a
// conversion is a rename: the file, its UID sidecar, and every text scene or
// resource naming the old path. Mirrors what FileSystemDock does on rename,
// minus ResourceLoader::rename_dependencies, which GDExtension cannot reach.

String SafeGDScriptLanguage::converted_script_path(const String &p_path) {
	const String ext = p_path.get_extension().to_lower();
	if (ext == "gd") {
		return p_path.get_basename() + ".sgd";
	}
	if (ext == "sgd" || ext == "safegd") {
		return p_path.get_basename() + ".gd";
	}
	return String();
}

static void collect_project_files(const String &p_dir, PackedStringArray &r_files) {
	if (FileAccess::file_exists(p_dir.path_join(".gdignore"))) {
		return;
	}
	Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return;
	}
	dir->list_dir_begin();
	for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
		if (name.begins_with(".")) {
			continue;
		}
		const String path = p_dir.path_join(name);
		if (dir->current_is_dir()) {
			collect_project_files(path, r_files);
		} else {
			r_files.push_back(path);
		}
	}
	dir->list_dir_end();
}

// Text scenes and resources carry the script path next to its UID. The UID
// keeps them loading after the rename; the path is rewritten so files without
// a UID, and the editor's dependency view, agree with disk.
static void rewrite_references(const String &p_from, const String &p_to,
		PackedStringArray &r_scenes, PackedStringArray &r_scripts) {
	PackedStringArray files;
	collect_project_files("res://", files);

	const String needle = "path=\"" + p_from + "\"";
	const String replacement = "path=\"" + p_to + "\"";
	const String quoted = "\"" + p_from + "\"";
	const String single_quoted = "'" + p_from + "'";

	for (int64_t i = 0; i < files.size(); i++) {
		const String file = files[i];
		const String ext = file.get_extension().to_lower();
		const bool is_scene = ext == "tscn" || ext == "tres";
		const bool is_script = ext == "gd" || ext == "sgd" || ext == "safegd";
		if (!is_scene && !is_script) {
			continue;
		}
		const String text = FileAccess::get_file_as_string(file);
		if (is_scene) {
			if (!text.contains(needle)) {
				continue;
			}
			Ref<FileAccess> out = FileAccess::open(file, FileAccess::WRITE);
			if (out.is_null()) {
				ERR_PRINT("SafeGDScript conversion: cannot rewrite " + file);
				continue;
			}
			out->store_string(text.replace(needle, replacement));
			r_scenes.push_back(file);
		} else if (text.contains(quoted) || text.contains(single_quoted)) {
			// preload()/load()/extends by path: Godot's own rename leaves these too.
			r_scripts.push_back(file);
		}
	}
}

static bool retarget_autoloads(const String &p_from, const String &p_to) {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	TypedArray<Dictionary> properties = settings->get_property_list();
	bool changed = false;
	for (int64_t i = 0; i < properties.size(); i++) {
		const String name = Dictionary(properties[i])["name"];
		if (!name.begins_with("autoload/")) {
			continue;
		}
		const String value = settings->get_setting(name);
		// A leading "*" marks a singleton autoload.
		const bool singleton = value.begins_with("*");
		if ((singleton ? value.substr(1) : value) != p_from) {
			continue;
		}
		settings->set_setting(name, (singleton ? "*" : "") + p_to);
		changed = true;
	}
	if (changed) {
		settings->save();
	}
	return changed;
}

Dictionary SafeGDScriptLanguage::convert_script_path(const String &p_path) {
	Dictionary result;
	const String to = converted_script_path(p_path);
	ERR_FAIL_COND_V_MSG(to.is_empty(), result, "Not a GDScript or SafeGDScript file: " + p_path);
	ERR_FAIL_COND_V_MSG(!FileAccess::file_exists(p_path), result, "No such script: " + p_path);
	ERR_FAIL_COND_V_MSG(FileAccess::file_exists(to), result, "Refusing to overwrite " + to);

	// @test is not a GDScript annotation; its parser errors on an unknown one.
	// Warned about, not refused, the same as a preload() naming the old path.
	if (to.get_extension().to_lower() == "gd" &&
			FileAccess::get_file_as_string(p_path).contains("@test")) {
		WARN_PRINT(p_path + String(" uses @test, which GDScript does not have; remove it from ") +
				to + String(" by hand."));
	}

	const Error err = DirAccess::rename_absolute(p_path, to);
	ERR_FAIL_COND_V_MSG(err != OK, result, "Cannot rename " + p_path + " to " + to);

	// Scenes resolve an ext_resource by UID before path, so a moved sidecar
	// keeps every reference alive, binary scenes included.
	const String uid_from = p_path + String(".uid");
	const String uid_to = to + String(".uid");
	if (FileAccess::file_exists(uid_from)) {
		if (FileAccess::file_exists(uid_to)) {
			DirAccess::remove_absolute(uid_to);
		}
		DirAccess::rename_absolute(uid_from, uid_to);
	}

	PackedStringArray scenes;
	PackedStringArray scripts;
	rewrite_references(p_path, to, scenes, scripts);
	const bool autoload = retarget_autoloads(p_path, to);

	if (Engine::get_singleton()->is_editor_hint() && EditorInterface::get_singleton() != nullptr) {
		EditorFileSystem *fs = EditorInterface::get_singleton()->get_resource_filesystem();
		if (fs != nullptr) {
			// Removal first: it drops the UID that the new file re-registers.
			fs->update_file(p_path);
			fs->update_file(to);
		}
	}

	for (int64_t i = 0; i < scripts.size(); i++) {
		WARN_PRINT(String(scripts[i]) + " names " + p_path + " in a string; update it to " + to + " by hand.");
	}

	result["path"] = to;
	result["scenes"] = scenes;
	result["scripts"] = scripts;
	result["autoload"] = autoload;
	return result;
}
