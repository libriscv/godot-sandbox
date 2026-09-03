extends Node2D

@onready var report: RichTextLabel = $Report
@onready var summary: Label = $Summary
@onready var rerun: Button = $Rerun

func _ready() -> void:
	rerun.pressed.connect(_run)
	_run()

func _run() -> void:
	var total: Dictionary = SafeGDScriptLanguage.run_tests(
		PackedStringArray(["res://inventory.sgd"]), PackedStringArray(), true)
	report.clear()
	for script in total["scripts"]:
		report.append_text("[b]%s[/b]\n" % script["path"])
		for row in script["tests"]:
			var passed: bool = row["status"] == "passed"
			report.append_text("  [color=%s]%s[/color] %s  [color=gray]%.2f ms[/color]\n" % [
				"green" if passed else "red", "PASS" if passed else "FAIL", row["name"],
				float(row["elapsed_usec"]) / 1000.0])
			if not passed:
				report.append_text("      %s\n" % row["message"])
	summary.text = "%d passed, %d failed, %d errors" % [
		total["passed"], total["failed"], total["errors"]]
