#pragma once

#include "syntax_highlighter_safegdscript.h"

#include <godot_cpp/classes/editor_context_menu_plugin.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>

using namespace godot;

// "Convert to SafeGDScript" / "Convert to GDScript" and "Run Tests" in the
// right-click menus of the Scene dock, the FileSystem dock, the script list and
// the code area. One instance per slot; the engine binds a plugin to a single
// slot.
class SafeGDScriptContextMenu : public EditorContextMenuPlugin {
	GDCLASS(SafeGDScriptContextMenu, EditorContextMenuPlugin);

	ContextMenuSlot slot = CONTEXT_SLOT_FILESYSTEM;

	void on_selected(const Variant &p_selection, bool p_to_safe);
	// p_at_cursor runs only the @test the caret sits in.
	void on_run_tests(const Variant &p_selection, bool p_at_cursor);
	void on_profile(const Variant &p_selection, bool p_enable);

protected:
	static void _bind_methods() {}

public:
	void set_slot(ContextMenuSlot p_slot) { slot = p_slot; }
	virtual void _popup_menu(const PackedStringArray &p_paths) override;

	// Script paths behind a selection: file paths, nodes, or one Script.
	static PackedStringArray script_paths_of(const Variant &p_selection);
	PackedStringArray slot_paths(const Variant &p_selection) const;
	// Converts each path whose direction matches, then brings the editor
	// along: open tab, autoloads, dependent open scenes.
	static void convert(const PackedStringArray &p_paths, bool p_to_safe);
	// Runs the tests of each path and reports through the Output dock and a
	// toast, jumping to the first failure.
	static void run(const PackedStringArray &p_paths, const PackedStringArray &p_only);
	static void profile(const PackedStringArray &p_paths, bool p_enable);
	// The caret line in the visible code editor, or 0.
	static int32_t caret_line();
};

class SafeGDScriptEditorPlugin : public EditorPlugin {
	GDCLASS(SafeGDScriptEditorPlugin, EditorPlugin);

	Ref<SafeGDScriptContextMenu> menus[4];
	Ref<SafeGDScriptSyntaxHighlighter> highlighter;

protected:
	static void _bind_methods() {}

public:
	virtual void _enter_tree() override;
	virtual void _exit_tree() override;
	virtual String _get_plugin_name() const override { return "SafeGDScript"; }

	static void register_types();
	static void unregister_types();
};
