#include "editor_plugin_safegdscript.h"
#include "script_language_safegdscript.h"
#include "script_safegdscript.h"

#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
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

PackedStringArray SafeGDScriptContextMenu::script_paths_of(const Variant &p_selection) {
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

// The caret in the visible script editor. The code-area slot right-clicks
// inside the text, so "the test under the cursor" is a line number away.
int32_t SafeGDScriptContextMenu::caret_line() {
	EditorInterface *editor = EditorInterface::get_singleton();
	ScriptEditor *script_editor = editor ? editor->get_script_editor() : nullptr;
	if (script_editor == nullptr) {
		return 0;
	}
	const TypedArray<ScriptEditorBase> editors = script_editor->get_open_script_editors();
	for (int64_t i = 0; i < editors.size(); i++) {
		ScriptEditorBase *base = Object::cast_to<ScriptEditorBase>(editors[i]);
		TextEdit *text = base ? Object::cast_to<TextEdit>(base->get_base_editor()) : nullptr;
		if (text != nullptr && base->is_visible()) {
			return text->get_caret_line() + 1;
		}
	}
	return 0;
}

void SafeGDScriptContextMenu::_popup_menu(const PackedStringArray &p_paths) {
	PackedStringArray paths = slot_paths(p_paths);
	// The code area right-clicks inside one open script; every other slot acts
	// on a selection, where a caret means nothing.
	const int32_t caret = slot == CONTEXT_SLOT_SCRIPT_EDITOR_CODE ? caret_line() : 0;
	const PackedStringArray items = SafeGDScriptLanguage::menu_items_for(paths, caret);
	if (items.is_empty()) {
		return;
	}
	Ref<Theme> theme = EditorInterface::get_singleton()->get_editor_theme();
	for (int64_t i = 0; i < items.size(); i++) {
		const String item = items[i];
		if (item == "Convert to SafeGDScript") {
			add_context_menu_item(item,
					callable_mp(this, &SafeGDScriptContextMenu::on_selected).bind(true),
					theme->get_icon("SafeGDScript", "EditorIcons"));
		} else if (item == "Convert to GDScript") {
			add_context_menu_item(item,
					callable_mp(this, &SafeGDScriptContextMenu::on_selected).bind(false),
					theme->get_icon("GDScript", "EditorIcons"));
		} else if (item == "Run Test at Cursor") {
			add_context_menu_item(item,
					callable_mp(this, &SafeGDScriptContextMenu::on_run_tests).bind(true),
					theme->get_icon("Play", "EditorIcons"));
		} else if (item == "Run Tests") {
			add_context_menu_item(item,
					callable_mp(this, &SafeGDScriptContextMenu::on_run_tests).bind(false),
					theme->get_icon("Play", "EditorIcons"));
		} else if (item == "Profile Script" || item == "Stop Profiling") {
			add_context_menu_item(item,
					callable_mp(this, &SafeGDScriptContextMenu::on_profile)
							.bind(item == "Profile Script"),
					theme->get_icon("Time", "EditorIcons"));
		}
	}
}

PackedStringArray SafeGDScriptContextMenu::slot_paths(const Variant &p_selection) const {
	EditorInterface *editor = EditorInterface::get_singleton();
	if (slot == CONTEXT_SLOT_SCRIPT_EDITOR_CODE) {
		PackedStringArray paths;
		ScriptEditor *script_editor = editor ? editor->get_script_editor() : nullptr;
		Ref<Script> current = script_editor ? script_editor->get_current_script() : Ref<Script>();
		if (current.is_valid() && !current->get_path().is_empty()) {
			paths.push_back(current->get_path());
		}
		return paths;
	}
	if (slot == CONTEXT_SLOT_SCENE_TREE) {
		return script_paths_of(editor->get_selection()->get_selected_nodes());
	}
	return script_paths_of(p_selection);
}

void SafeGDScriptContextMenu::on_selected(const Variant &p_selection, bool p_to_safe) {
	convert(slot_paths(p_selection), p_to_safe);
}

void SafeGDScriptContextMenu::on_run_tests(const Variant &p_selection, bool p_at_cursor) {
	const PackedStringArray paths = slot_paths(p_selection);
	PackedStringArray only;
	if (p_at_cursor && paths.size() == 1) {
		const String name = SafeGDScriptLanguage::test_at_line(
				FileAccess::get_file_as_string(paths[0]), caret_line());
		if (!name.is_empty()) {
			only.push_back(name);
		}
	}
	run(paths, only);
}

void SafeGDScriptContextMenu::on_profile(const Variant &p_selection, bool p_enable) {
	profile(slot_paths(p_selection), p_enable);
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

void SafeGDScriptContextMenu::convert(const PackedStringArray &p_paths, bool p_to_safe) {
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

// Runs the tests and brings the editor along: the Output dock already has the
// per-test lines from the runner's own summary, so what is added here is a
// toast and a jump to the first failure.
void SafeGDScriptContextMenu::run(const PackedStringArray &p_paths,
		const PackedStringArray &p_only) {
	EditorInterface *editor = EditorInterface::get_singleton();
	ScriptEditor *script_editor = editor ? editor->get_script_editor() : nullptr;
	ResourceLoader *loader = ResourceLoader::get_singleton();

	PackedStringArray paths;
	for (int64_t i = 0; i < p_paths.size(); i++) {
		const String path = p_paths[i];
		if (path.contains("::") || paths.has(path)) {
			continue;
		}
		const String ext = path.get_extension().to_lower();
		if (ext != "sgd" && ext != "safegd") {
			continue;
		}
		paths.push_back(path);

		// Tests run against what the user sees, not what is on disk.
		if (script_editor != nullptr && loader->has_cached(path)) {
			Ref<Script> open = loader->load(path);
			if (open.is_valid() && script_editor->get_open_scripts().has(open)) {
				const String live = live_source(script_editor, open);
				if (live != open->get_source_code()) {
					open->set_source_code(live);
					open->reload(true);
				}
			}
		}
	}
	if (paths.is_empty()) {
		return;
	}

	const Dictionary total = SafeGDScriptLanguage::run_tests(paths, p_only);
	const int64_t passed = total.get("passed", int64_t(0));
	const int64_t failed = total.get("failed", int64_t(0));
	const int64_t errors = total.get("errors", int64_t(0));

	// The first failing row decides where the editor lands.
	String first_message;
	String jump_path;
	int32_t jump_line = -1;
	const Array scripts = total.get("scripts", Array());
	for (int64_t i = 0; i < scripts.size() && jump_path.is_empty(); i++) {
		const Dictionary report = scripts[i];
		const Array rows = report.get("tests", Array());
		for (int64_t j = 0; j < rows.size(); j++) {
			const Dictionary row = rows[j];
			const String status = row.get("status", String());
			if (status == "passed") {
				continue;
			}
			first_message = row.get("message", String());
			jump_path = report.get("path", String());
			// "res://player.sgd:14" when the line table knew; otherwise the
			// test's own declaration line.
			const String location = row.get("location", String());
			const int colon = location.rfind(":");
			const String suffix = colon > 0 ? location.substr(colon + 1) : String();
			jump_line = suffix.is_valid_int() ? suffix.to_int() : int32_t(row.get("line", 0));
			break;
		}
	}

	const String summary = (paths.size() == 1 ? paths[0].get_file() : itos(paths.size()) +
			String(" scripts")) + ": " + itos(passed) + " passed, " + itos(failed) +
			" failed" + (errors > 0 ? String(", ") + itos(errors) + " errors" : String());
	EditorToaster *toaster = editor != nullptr ? editor->get_editor_toaster() : nullptr;
	if (toaster != nullptr) {
		toaster->push_toast(summary, failed > 0 || errors > 0
				? EditorToaster::SEVERITY_ERROR : EditorToaster::SEVERITY_INFO,
				first_message);
	}

	if (!jump_path.is_empty() && editor != nullptr) {
		Ref<Script> script = loader->load(jump_path, "SafeGDScript");
		if (script.is_valid()) {
			editor->edit_script(script, jump_line > 0 ? jump_line : -1);
		}
	}
}

void SafeGDScriptContextMenu::profile(const PackedStringArray &p_paths, bool p_enable) {
	EditorInterface *editor = EditorInterface::get_singleton();
	ResourceLoader *loader = ResourceLoader::get_singleton();
	PackedStringArray done;
	PackedStringArray refused;
	for (int64_t i = 0; i < p_paths.size(); i++) {
		const String path = p_paths[i];
		const String ext = path.get_extension().to_lower();
		if (path.contains("::") || done.has(path) || (ext != "sgd" && ext != "safegd")) {
			continue;
		}
		done.push_back(path);
		Ref<SafeGDScript> script = loader->load(path, "SafeGDScript");
		if (script.is_null() || !script->set_profiling(p_enable)) {
			refused.push_back(path.get_file());
		}
	}
	if (done.is_empty()) {
		return;
	}
	EditorToaster *toaster = editor != nullptr ? editor->get_editor_toaster() : nullptr;
	if (toaster == nullptr) {
		return;
	}
	if (!refused.is_empty()) {
		toaster->push_toast("Could not build " + String(", ").join(refused) +
				" with profiling.", EditorToaster::SEVERITY_ERROR);
		return;
	}
	const String what = done.size() == 1 ? done[0].get_file() : itos(done.size()) + " scripts";
	toaster->push_toast(what + (p_enable ? String(": profiling on") : String(": profiling off")),
			EditorToaster::SEVERITY_INFO,
			p_enable ? String("Open Debugger > Profiler and press Start to collect.")
					: String());
}

void SafeGDScriptLanguage::editor_convert_scripts(const PackedStringArray &p_paths, bool p_to_safe) {
	ERR_FAIL_NULL_MSG(EditorInterface::get_singleton(), "editor_convert_scripts() needs the editor.");
	SafeGDScriptContextMenu::convert(p_paths, p_to_safe);
}

void SafeGDScriptEditorPlugin::_enter_tree() {
	static constexpr EditorContextMenuPlugin::ContextMenuSlot slots[4] = {
		EditorContextMenuPlugin::CONTEXT_SLOT_SCENE_TREE,
		EditorContextMenuPlugin::CONTEXT_SLOT_FILESYSTEM,
		EditorContextMenuPlugin::CONTEXT_SLOT_SCRIPT_EDITOR,
		// Right-click inside the code: one path, and a caret to read.
		EditorContextMenuPlugin::CONTEXT_SLOT_SCRIPT_EDITOR_CODE,
	};
	for (int i = 0; i < 4; i++) {
		menus[i].instantiate();
		menus[i]->set_slot(slots[i]);
		add_context_menu_plugin(slots[i], menus[i]);
	}
	if (ScriptEditor *editor = EditorInterface::get_singleton()->get_script_editor()) {
		highlighter.instantiate();
		editor->register_syntax_highlighter(highlighter);
	}
}

void SafeGDScriptEditorPlugin::_exit_tree() {
	for (int i = 0; i < 4; i++) {
		if (menus[i].is_valid()) {
			remove_context_menu_plugin(menus[i]);
			menus[i].unref();
		}
	}
	if (highlighter.is_valid()) {
		if (ScriptEditor *editor = EditorInterface::get_singleton()->get_script_editor()) {
			editor->unregister_syntax_highlighter(highlighter);
		}
		highlighter.unref();
	}
}

void SafeGDScriptEditorPlugin::register_types() {
	ClassDB::register_class<SafeGDScriptSyntaxHighlighter>();
	ClassDB::register_internal_class<SafeGDScriptContextMenu>();
	ClassDB::register_internal_class<SafeGDScriptEditorPlugin>();
	EditorPlugins::add_by_type<SafeGDScriptEditorPlugin>();
}

void SafeGDScriptEditorPlugin::unregister_types() {
	EditorPlugins::remove_by_type<SafeGDScriptEditorPlugin>();
}
