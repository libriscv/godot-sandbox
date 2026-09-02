extends GutTest

const SOURCE := """@export var speed := 1.0
func run(count: int) -> void:
	print(count) # done
	var label := "text"
	var target := $Player
"""

func _lines() -> Array[Dictionary]:
	var highlighter := SafeGDScriptCodeHighlighter.new()
	var edit := CodeEdit.new()
	edit.text = SOURCE
	edit.syntax_highlighter = highlighter
	var lines: Array[Dictionary] = []
	for line in edit.get_line_count():
		lines.push_back(highlighter.get_line_syntax_highlighting(line))
	edit.syntax_highlighter = null
	edit.free()
	return lines

# The colour a column inherits: the entry at or before it.
func _color_at(line: Dictionary, column: int) -> Color:
	var best := -1
	for key in line:
		if int(key) <= column and int(key) > best:
			best = int(key)
	if best < 0:
		return Color()
	return line[best]["color"]

func test_the_highlighter_colours_an_annotation_apart_from_plain_text():
	var text := SOURCE.get_slice("\n", 0)
	var line: Dictionary = _lines()[0]
	assert_gt(line.size(), 0)
	assert_ne(_color_at(line, text.find("@export")), _color_at(line, text.find("speed")))

func test_a_function_definition_differs_from_its_keyword_and_parameters():
	var text := SOURCE.get_slice("\n", 1)
	var line: Dictionary = _lines()[1]
	assert_ne(_color_at(line, text.find("func")), _color_at(line, text.find("run")))
	assert_ne(_color_at(line, text.find("run")), _color_at(line, text.find("count")))

func test_a_comment_differs_from_the_code_before_it():
	var text := SOURCE.get_slice("\n", 2)
	var line: Dictionary = _lines()[2]
	assert_ne(_color_at(line, text.find("#")), _color_at(line, text.find("print")))

func test_a_string_literal_is_one_colour():
	var text := SOURCE.get_slice("\n", 3)
	var line: Dictionary = _lines()[3]
	assert_eq(_color_at(line, text.find("\"")), _color_at(line, text.find("text")))
	assert_ne(_color_at(line, text.find("\"")), _color_at(line, text.find("label")))

func test_a_node_path_is_coloured_as_one_token():
	var text := SOURCE.get_slice("\n", 4)
	var line: Dictionary = _lines()[4]
	assert_eq(_color_at(line, text.find("$")), _color_at(line, text.find("Player")))
	assert_ne(_color_at(line, text.find("$")), _color_at(line, text.find("target")))
