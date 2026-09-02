#include "editor_plugin_safegdscript.h"
#include "script_language_safegdscript.h"

#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_plugin_registration.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/file_system_dock.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/script_editor_base.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/core/class_db.hpp>

static bool is_gdscript_path(const String &p_path) {
	return p_path.get_extension().to_lower() == "gd";
}

static bool is_safegdscript_path(const String &p_path) {
	const String ext = p_path.get_extension().to_lower();
	return ext == "sgd" || ext == "safegd";
}

PackedStringArray SafeGDScriptConvertMenu::script_paths_of(const Variant &p_selection) {
	PackedStringArray paths;
	switch (p_selection.get_type()) {
		case Variant::PACKED_STRING_ARRAY:
			return p_selection;
		case Variant::ARRAY: {
			const Array nodes = p_selection;
			for (int64_t i = 0; i < nodes.size(); i++) {
				Node *node = Object::cast_to<Node>(nodes[i]);
				Ref<Script> script = node ? Ref<Script>(node->get_script()) : Ref<Script>();
				if (script.is_valid()) {
					paths.push_back(script->get_path());
				}
			}
			break;
		}
		case Variant::OBJECT: {
			Script *script = Object::cast_to<Script>(p_selection);
			if (script != nullptr) {
				paths.push_back(script->get_path());
			}
			break;
		}
		default:
			break;
	}
	return paths;
}

void SafeGDScriptConvertMenu::_popup_menu(const PackedStringArray &p_paths) {
	PackedStringArray paths = p_paths;
	if (slot == CONTEXT_SLOT_SCENE_TREE) {
		// p_paths are node paths; the selection itself is cheaper to read.
		paths = script_paths_of(EditorInterface::get_singleton()->get_selection()->get_selected_nodes());
	}
	bool to_safe = false;
	bool to_gd = false;
	for (int64_t i = 0; i < paths.size(); i++) {
		// A built-in script ("res://s.tscn::GDScript_x") has no file to rename.
		if (paths[i].contains("::")) {
			continue;
		}
		to_safe = to_safe || is_gdscript_path(paths[i]);
		to_gd = to_gd || is_safegdscript_path(paths[i]);
	}
	if (!to_safe && !to_gd) {
		return;
	}
	Ref<Theme> theme = EditorInterface::get_singleton()->get_editor_theme();
	if (to_safe) {
		add_context_menu_item("Convert to SafeGDScript",
				callable_mp(this, &SafeGDScriptConvertMenu::on_selected).bind(true),
				theme->get_icon("SafeGDScript", "EditorIcons"));
	}
	if (to_gd) {
		add_context_menu_item("Convert to GDScript",
				callable_mp(this, &SafeGDScriptConvertMenu::on_selected).bind(false),
				theme->get_icon("GDScript", "EditorIcons"));
	}
}

void SafeGDScriptConvertMenu::on_selected(const Variant &p_selection, bool p_to_safe) {
	convert(script_paths_of(p_selection), p_to_safe);
}

// The text of the visible tab. Script::source_code lags behind it for @tool
// scripts, which the editor only writes back on save.
static String live_source(ScriptEditor *p_editor, const Ref<Script> &p_script) {
	if (p_editor->get_current_script() == p_script) {
		TypedArray<ScriptEditorBase> editors = p_editor->get_open_script_editors();
		for (int64_t i = 0; i < editors.size(); i++) {
			ScriptEditorBase *base = Object::cast_to<ScriptEditorBase>(editors[i]);
			TextEdit *text = base ? Object::cast_to<TextEdit>(base->get_base_editor()) : nullptr;
			if (text != nullptr && base->is_visible()) {
				return text->get_text();
			}
		}
	}
	return p_script->get_source_code();
}

void SafeGDScriptConvertMenu::convert(const PackedStringArray &p_paths, bool p_to_safe) {
	EditorInterface *editor = EditorInterface::get_singleton();
	ScriptEditor *script_editor = editor->get_script_editor();
	ResourceLoader *loader = ResourceLoader::get_singleton();
	const PackedStringArray open_scenes = editor->get_open_scenes();
	Node *edited_root = editor->get_edited_scene_root();
	const String edited_scene = edited_root ? edited_root->get_scene_file_path() : String();

	PackedStringArray done;
	PackedStringArray reload;
	for (int64_t i = 0; i < p_paths.size(); i++) {
		const String from = p_paths[i];
		if (done.has(from) || from.contains("::")) {
			continue;
		}
		if (p_to_safe ? !is_gdscript_path(from) : !is_safegdscript_path(from)) {
			continue;
		}
		done.push_back(from);

		// Unsaved edits travel with the file: the tab is closed without a prompt below.
		Ref<Script> old_script;
		String live;
		bool was_open = false;
		if (loader->has_cached(from)) {
			old_script = loader->load(from);
			was_open = old_script.is_valid() && script_editor->get_open_scripts().has(old_script);
			if (was_open) {
				live = live_source(script_editor, old_script);
			}
		}

		const Dictionary result = SafeGDScriptLanguage::convert_script_path(from);
		if (result.is_empty()) {
			continue;
		}
		const String to = result["path"];
		if (was_open && live != FileAccess::get_file_as_string(to)) {
			Ref<FileAccess> file = FileAccess::open(to, FileAccess::WRITE);
			if (file.is_valid()) {
				file->store_string(live);
			}
		}

		// A stale resource under the new path (from an earlier round trip)
		// would otherwise be handed back by load().
		if (loader->has_cached(to)) {
			Ref<Script> cached = loader->load(to);
			if (cached.is_valid()) {
				cached->set_source_code(FileAccess::get_file_as_string(to));
				cached->reload(true);
			}
		}

		// What FileSystemDock emits after a delete; ScriptEditor closes the tab.
		editor->get_file_system_dock()->emit_signal("file_removed", from);
		if (was_open) {
			Ref<Script> new_script = loader->load(to);
			if (new_script.is_valid()) {
				editor->edit_script(new_script);
			}
		}

		const PackedStringArray scenes = result["scenes"];
		for (int64_t j = 0; j < scenes.size(); j++) {
			if (open_scenes.has(scenes[j]) && !reload.has(scenes[j])) {
				reload.push_back(scenes[j]);
			}
		}
	}

	// The edited scene first, as FileSystemDock does (GH-82652).
	if (reload.has(edited_scene)) {
		editor->reload_scene_from_path(edited_scene);
	}
	for (int64_t i = 0; i < reload.size(); i++) {
		if (reload[i] != edited_scene) {
			editor->reload_scene_from_path(reload[i]);
		}
	}
}

void SafeGDScriptLanguage::editor_convert_scripts(const PackedStringArray &p_paths, bool p_to_safe) {
	ERR_FAIL_NULL_MSG(EditorInterface::get_singleton(), "editor_convert_scripts() needs the editor.");
	SafeGDScriptConvertMenu::convert(p_paths, p_to_safe);
}

void SafeGDScriptEditorPlugin::_enter_tree() {
	static constexpr EditorContextMenuPlugin::ContextMenuSlot slots[3] = {
		EditorContextMenuPlugin::CONTEXT_SLOT_SCENE_TREE,
		EditorContextMenuPlugin::CONTEXT_SLOT_FILESYSTEM,
		EditorContextMenuPlugin::CONTEXT_SLOT_SCRIPT_EDITOR,
	};
	for (int i = 0; i < 3; i++) {
		menus[i].instantiate();
		menus[i]->set_slot(slots[i]);
		add_context_menu_plugin(slots[i], menus[i]);
	}
}

void SafeGDScriptEditorPlugin::_exit_tree() {
	for (int i = 0; i < 3; i++) {
		if (menus[i].is_valid()) {
			remove_context_menu_plugin(menus[i]);
			menus[i].unref();
		}
	}
}

void SafeGDScriptEditorPlugin::register_types() {
	ClassDB::register_internal_class<SafeGDScriptConvertMenu>();
	ClassDB::register_internal_class<SafeGDScriptEditorPlugin>();
	EditorPlugins::add_by_type<SafeGDScriptEditorPlugin>();
}

void SafeGDScriptEditorPlugin::unregister_types() {
	EditorPlugins::remove_by_type<SafeGDScriptEditorPlugin>();
}
