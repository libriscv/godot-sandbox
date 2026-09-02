#pragma once

#include <godot_cpp/classes/editor_syntax_highlighter.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/variant/color.hpp>

#include <string>
#include <unordered_map>
#include <vector>

using namespace godot;

namespace godot {
class TextEdit;
}

// set_text_edit is not in the GDExtension API.
class SafeGDScriptHighlightRules {
	struct Region {
		String start;
		String end;
		Color color;
		bool line_only = false;
	};

	std::vector<Region> regions;
	std::unordered_map<std::string, Color> keywords;
	std::unordered_map<std::string, Color> member_keywords;
	std::unordered_map<int, int> open_region;

	Color font_color = Color(0.67, 0.67, 0.67);
	Color symbol_color = Color(0.73, 0.87, 1.0);
	Color number_color = Color(0.92, 0.58, 0.2);
	Color function_color = Color(0.4, 0.64, 0.81);
	Color member_variable_color = Color(0.9, 0.31, 0.35);
	Color function_definition_color = Color(0.4, 0.9, 1.0);
	Color annotation_color = Color(1.0, 0.7, 0.45);
	Color node_path_color = Color(0.72, 0.77, 0.49);
	Color node_ref_color = Color(0.39, 0.76, 0.35);
	Color string_name_color = Color(1.0, 0.76, 0.65);

	int match_region(const String &p_text, int p_from) const;
	int scan_line(const String &p_text, int p_entry_region, Dictionary *r_map) const;
	int region_at_line_start(TextEdit *p_text_edit, int p_line);

public:
	void update(TextEdit *p_text_edit, const Ref<Script> &p_edited);
	void clear() { open_region.clear(); }
	Dictionary highlight_line(TextEdit *p_text_edit, int p_line);
};

class SafeGDScriptCodeHighlighter : public SyntaxHighlighter {
	GDCLASS(SafeGDScriptCodeHighlighter, SyntaxHighlighter);

	mutable SafeGDScriptHighlightRules rules;

protected:
	static void _bind_methods() {}

public:
	virtual Dictionary _get_line_syntax_highlighting(int32_t p_line) const override;
	virtual void _clear_highlighting_cache() override;
	virtual void _update_cache() override;
};

// Answers "SafeGD" so ScriptEditor selects it.
class SafeGDScriptSyntaxHighlighter : public EditorSyntaxHighlighter {
	GDCLASS(SafeGDScriptSyntaxHighlighter, EditorSyntaxHighlighter);

	mutable SafeGDScriptHighlightRules rules;

protected:
	static void _bind_methods() {}

public:
	virtual String _get_name() const override { return "SafeGDScript"; }
	virtual PackedStringArray _get_supported_languages() const override;
	virtual Ref<EditorSyntaxHighlighter> _create() const override;

	virtual Dictionary _get_line_syntax_highlighting(int32_t p_line) const override;
	virtual void _clear_highlighting_cache() override;
	virtual void _update_cache() override;
};
