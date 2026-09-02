#pragma once

#include <godot_cpp/classes/editor_context_menu_plugin.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>

using namespace godot;

// "Convert to SafeGDScript" / "Convert to GDScript" in the right-click menus
// of the Scene dock, the FileSystem dock and the script list. One instance per
// slot; the engine binds a plugin to a single slot.
class SafeGDScriptConvertMenu : public EditorContextMenuPlugin {
	GDCLASS(SafeGDScriptConvertMenu, EditorContextMenuPlugin);

	ContextMenuSlot slot = CONTEXT_SLOT_FILESYSTEM;

	void on_selected(const Variant &p_selection, bool p_to_safe);

protected:
	static void _bind_methods() {}

public:
	void set_slot(ContextMenuSlot p_slot) { slot = p_slot; }
	virtual void _popup_menu(const PackedStringArray &p_paths) override;

	// Script paths behind a selection: file paths, nodes, or one Script.
	static PackedStringArray script_paths_of(const Variant &p_selection);
	// Converts each path whose direction matches, then brings the editor
	// along: open tab, autoloads, dependent open scenes.
	static void convert(const PackedStringArray &p_paths, bool p_to_safe);
};

class SafeGDScriptEditorPlugin : public EditorPlugin {
	GDCLASS(SafeGDScriptEditorPlugin, EditorPlugin);

	Ref<SafeGDScriptConvertMenu> menus[3];

protected:
	static void _bind_methods() {}

public:
	virtual void _enter_tree() override;
	virtual void _exit_tree() override;
	virtual String _get_plugin_name() const override { return "SafeGDScript"; }

	static void register_types();
	static void unregister_types();
};
