@tool
extends EditorPlugin

var popup_window: Window
var bintr_export_plugin: EditorExportPlugin

func _enter_tree():
	# Load your popup scene
	var popup_scene = preload("res://addons/godot_sandbox/downloader.tscn")
	popup_window = popup_scene.instantiate()
	popup_window.close_requested.connect(popup_window.hide)
	
	# Add it to the editor's main screen
	get_editor_interface().get_base_control().add_child(popup_window)
	
	# Optional: Add a menu item to trigger it
	add_tool_menu_item("Godot Sandbox Dependencies...", show_popup)
	add_tool_menu_item("Bake SafeGDScript Translations", bake_translations)
	bintr_export_plugin = preload("res://addons/godot_sandbox/bintr_export_plugin.gd").new()
	add_export_plugin(bintr_export_plugin)

func _exit_tree():
	remove_tool_menu_item("Godot Sandbox Dependencies...")
	remove_tool_menu_item("Bake SafeGDScript Translations")
	if bintr_export_plugin:
		remove_export_plugin(bintr_export_plugin)
		bintr_export_plugin = null
	if popup_window:
		popup_window.queue_free()

func show_popup():
	popup_window.popup_centered()

func bake_translations():
	var result := SafeGDScriptLanguage.new().bake_all_translations()
	var baked: Array = result.get("baked", [])
	var failed: PackedStringArray = result.get("failed", PackedStringArray())
	print("SafeGDScript translations: %d baked, %d failed" % [baked.size(), failed.size()])
	for failure in failed:
		push_error(failure)
