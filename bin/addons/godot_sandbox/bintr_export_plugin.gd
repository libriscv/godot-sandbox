@tool
extends EditorExportPlugin

func _export_begin(features: PackedStringArray, _is_debug: bool, _path: String, _flags: int) -> void:
	if not Sandbox.has_feature_binary_translation():
		return
	var target := " ".join(features).to_lower()
	var host := OS.get_name().to_lower()
	var same_host := (host == "linux" and "linux" in target) \
			or (host == "windows" and "windows" in target) \
			or (host == "macos" and ("macos" in target or "osx" in target))
	if not same_host:
		push_warning("SafeGDScript native translations were not exported: cross-platform baking requires a target-addon translation-defines sidecar.")
		return

	var result := SafeGDScriptLanguage.new().bake_all_translations()
	for entry: Dictionary in result.get("baked", []):
		add_shared_object(entry.path, PackedStringArray(), "bintr")
	for failure: String in result.get("failed", PackedStringArray()):
		push_warning("SafeGDScript translation was not exported: " + failure)
