#include "syntax_highlighter_safegdscript.h"
#include "script_language_safegdscript.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/text_edit.hpp>

#include <algorithm>

static bool is_ident_start(char32_t p_char) {
	return p_char == '_' || (p_char >= 'a' && p_char <= 'z') || (p_char >= 'A' && p_char <= 'Z') ||
			p_char > 127;
}

static bool is_ident_char(char32_t p_char) {
	return is_ident_start(p_char) || (p_char >= '0' && p_char <= '9');
}

static bool is_digit_char(char32_t p_char) {
	return p_char >= '0' && p_char <= '9';
}

static bool is_space_char(char32_t p_char) {
	return p_char == ' ' || p_char == '\t' || p_char == '\r';
}

static void put_color(Dictionary *r_map, int p_column, const Color &p_color) {
	if (r_map == nullptr) {
		return;
	}
	Dictionary entry;
	entry["color"] = p_color;
	(*r_map)[p_column] = entry;
}

static int find_region_end(const String &p_text, int p_from, const String &p_end) {
	const int length = p_text.length();
	const int end_length = p_end.length();
	if (end_length == 0) {
		return -1;
	}
	for (int i = p_from; i + end_length <= length; i++) {
		if (p_text[i] == '\\') {
			i++;
			continue;
		}
		bool match = true;
		for (int k = 0; k < end_length; k++) {
			if (p_text[i + k] != p_end[k]) {
				match = false;
				break;
			}
		}
		if (match) {
			return i;
		}
	}
	return -1;
}

static int skip_quoted(const String &p_text, int p_from) {
	const int length = p_text.length();
	const char32_t quote = p_text[p_from];
	for (int i = p_from + 1; i < length; i++) {
		if (p_text[i] == '\\') {
			i++;
			continue;
		}
		if (p_text[i] == quote) {
			return i + 1;
		}
	}
	return length;
}

static char32_t previous_visible(const String &p_text, int p_index) {
	for (int i = p_index - 1; i >= 0; i--) {
		if (!is_space_char(p_text[i])) {
			return p_text[i];
		}
	}
	return 0;
}

static bool follows_value(const String &p_text, int p_index) {
	const char32_t previous = previous_visible(p_text, p_index);
	return is_ident_char(previous) || previous == ')' || previous == ']' || previous == '}' ||
			previous == '"' || previous == '\'';
}

static Color editor_theme_color(const Ref<EditorSettings> &p_settings, const char *p_key,
		const Color &p_fallback) {
	const String setting = String("text_editor/theme/highlighting/") + String(p_key);
	if (p_settings.is_null() || !p_settings->has_setting(setting)) {
		return p_fallback;
	}
	const Variant value = p_settings->get_setting(setting);
	return value.get_type() == Variant::COLOR ? Color(value) : p_fallback;
}

void SafeGDScriptHighlightRules::update(TextEdit *p_text_edit, const Ref<Script> &p_edited) {
	keywords.clear();
	member_keywords.clear();
	regions.clear();
	open_region.clear();

	// godot-cpp release binds the singleton even outside the editor.
	Ref<EditorSettings> settings;
	if (Engine::get_singleton()->is_editor_hint()) {
		if (EditorInterface *editor = EditorInterface::get_singleton()) {
			settings = editor->get_editor_settings();
		}
	}
	symbol_color = editor_theme_color(settings, "symbol_color", symbol_color);
	number_color = editor_theme_color(settings, "number_color", number_color);
	function_color = editor_theme_color(settings, "function_color", function_color);
	member_variable_color = editor_theme_color(settings, "member_variable_color", member_variable_color);
	font_color = editor_theme_color(settings, "text_color", font_color);
	function_definition_color = editor_theme_color(settings, "gdscript/function_definition_color", function_definition_color);
	annotation_color = editor_theme_color(settings, "gdscript/annotation_color", annotation_color);
	node_path_color = editor_theme_color(settings, "gdscript/node_path_color", node_path_color);
	node_ref_color = editor_theme_color(settings, "gdscript/node_reference_color", node_ref_color);
	string_name_color = editor_theme_color(settings, "gdscript/string_name_color", string_name_color);
	if (p_text_edit != nullptr) {
		font_color = p_text_edit->get_theme_color("font_color");
	}

	const Color keyword_color = editor_theme_color(settings, "keyword_color", Color(1.0, 1.0, 0.7));
	const Color control_flow_color = editor_theme_color(settings, "control_flow_keyword_color", Color(1.0, 0.85, 0.7));
	const Color base_type_color = editor_theme_color(settings, "base_type_color", Color(0.64, 1.0, 0.83));
	const Color engine_type_color = editor_theme_color(settings, "engine_type_color", Color(0.51, 0.83, 1.0));
	const Color user_type_color = editor_theme_color(settings, "user_type_color", Color(0.42, 0.67, 0.93));
	const Color comment_color = editor_theme_color(settings, "comment_color", Color(0.4, 0.4, 0.4));
	const Color doc_comment_color = editor_theme_color(settings, "doc_comment_color", Color(0.5, 0.6, 0.7));
	const Color string_color = editor_theme_color(settings, "string_color", Color(0.94, 0.43, 0.75));

	SafeGDScriptLanguage *language = SafeGDScriptLanguage::get_singleton();

	// Reserved words first so types in both tables read as types.
	if (language != nullptr) {
		for (const String &word : language->_get_reserved_words()) {
			keywords[std::string(word.utf8().get_data())] =
					language->_is_control_flow_keyword(word) ? control_flow_color : keyword_color;
		}
	}
	for (int type = 1; type < Variant::VARIANT_MAX; type++) {
		if (type == Variant::OBJECT) {
			continue;
		}
		keywords[std::string(Variant::get_type_name(Variant::Type(type)).utf8().get_data())] = base_type_color;
	}
	keywords["Variant"] = base_type_color;
	keywords["void"] = base_type_color;
	if (ClassDBSingleton *class_db = ClassDBSingleton::get_singleton()) {
		for (const String &type : class_db->get_class_list()) {
			keywords[std::string(type.utf8().get_data())] = engine_type_color;
		}
	}
	if (ProjectSettings *project = ProjectSettings::get_singleton()) {
		for (const Dictionary &entry : project->get_global_class_list()) {
			const String name = entry.get("class", String());
			if (!name.is_empty()) {
				keywords[std::string(name.utf8().get_data())] = user_type_color;
			}
		}
		for (const Dictionary &entry : project->get_property_list()) {
			const String setting = entry.get("name", String());
			if (setting.begins_with("autoload/")) {
				keywords[std::string(setting.trim_prefix("autoload/").utf8().get_data())] = user_type_color;
			}
		}
	}

	const StringName instance_base = p_edited.is_valid() ? p_edited->get_instance_base_type() : StringName();
	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	if (class_db != nullptr && instance_base != StringName()) {
		for (const Dictionary &entry : class_db->class_get_property_list(instance_base)) {
			const int64_t usage = entry.get("usage", 0);
			if (usage & (PROPERTY_USAGE_CATEGORY | PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP)) {
				continue;
			}
			const String name = entry.get("name", String());
			if (!name.contains("/")) {
				member_keywords[std::string(name.utf8().get_data())] = member_variable_color;
			}
		}
		for (const String &constant : class_db->class_get_integer_constant_list(instance_base)) {
			member_keywords[std::string(constant.utf8().get_data())] = member_variable_color;
		}
	}

	if (language != nullptr) {
		struct DelimiterSet {
			PackedStringArray delimiters;
			Color color;
		};
		const DelimiterSet sets[3] = {
			{ language->_get_doc_comment_delimiters(), doc_comment_color },
			{ language->_get_comment_delimiters(), comment_color },
			{ language->_get_string_delimiters(), string_color },
		};
		for (const DelimiterSet &set : sets) {
			for (const String &delimiter : set.delimiters) {
				Region region;
				region.start = delimiter.get_slicec(' ', 0);
				region.end = delimiter.get_slice_count(" ") > 1 ? delimiter.get_slicec(' ', 1) : String();
				region.color = set.color;
				region.line_only = region.end.is_empty();
				if (!region.start.is_empty()) {
					regions.push_back(region);
				}
			}
		}
		// Longest key first so "##" beats "#".
		std::stable_sort(regions.begin(), regions.end(), [](const Region &a, const Region &b) {
			return a.start.length() > b.start.length();
		});
	}
}

int SafeGDScriptHighlightRules::match_region(const String &p_text, int p_from) const {
	const int length = p_text.length();
	for (size_t index = 0; index < regions.size(); index++) {
		const String &start = regions[index].start;
		const int start_length = start.length();
		if (p_from + start_length > length) {
			continue;
		}
		bool match = true;
		for (int k = 0; k < start_length; k++) {
			if (p_text[p_from + k] != start[k]) {
				match = false;
				break;
			}
		}
		if (match) {
			return int(index);
		}
	}
	return -1;
}

int SafeGDScriptHighlightRules::scan_line(const String &p_text, int p_entry_region, Dictionary *r_map) const {
	const int length = p_text.length();
	int i = 0;
	std::string previous_word;

	if (p_entry_region >= 0 && p_entry_region < int(regions.size())) {
		const Region &region = regions[p_entry_region];
		put_color(r_map, 0, region.color);
		const int end = find_region_end(p_text, 0, region.end);
		if (end < 0) {
			return p_entry_region;
		}
		i = end + region.end.length();
	}

	while (i < length) {
		const char32_t current = p_text[i];
		if (is_space_char(current)) {
			i++;
			continue;
		}

		const int region_index = match_region(p_text, i);
		if (region_index >= 0) {
			const Region &region = regions[region_index];
			put_color(r_map, i, region.color);
			if (region.line_only) {
				return -1;
			}
			const int end = find_region_end(p_text, i + region.start.length(), region.end);
			if (end < 0) {
				return region_index;
			}
			i = end + region.end.length();
			previous_word.clear();
			continue;
		}

		if (current == '@' && i + 1 < length && is_ident_start(p_text[i + 1])) {
			int j = i + 1;
			while (j < length && is_ident_char(p_text[j])) {
				j++;
			}
			put_color(r_map, i, annotation_color);
			previous_word.clear();
			i = j;
			continue;
		}

		if (current == '$' || (current == '%' && !follows_value(p_text, i))) {
			const Color color = current == '$' ? node_path_color : node_ref_color;
			int j = i + 1;
			if (j < length && (p_text[j] == '"' || p_text[j] == '\'')) {
				j = skip_quoted(p_text, j);
			} else {
				while (j < length && (is_ident_char(p_text[j]) || p_text[j] == '/' ||
									  (current == '$' && p_text[j] == '%'))) {
					j++;
				}
			}
			if (j > i + 1) {
				put_color(r_map, i, color);
				previous_word.clear();
				i = j;
				continue;
			}
		}

		if (current == '&' && i + 1 < length && !follows_value(p_text, i) &&
				(p_text[i + 1] == '"' || p_text[i + 1] == '\'' || is_ident_start(p_text[i + 1]))) {
			int j = i + 1;
			if (p_text[j] == '"' || p_text[j] == '\'') {
				j = skip_quoted(p_text, j);
			} else {
				while (j < length && is_ident_char(p_text[j])) {
					j++;
				}
			}
			put_color(r_map, i, string_name_color);
			previous_word.clear();
			i = j;
			continue;
		}

		if (is_ident_start(current)) {
			int j = i;
			while (j < length && is_ident_char(p_text[j])) {
				j++;
			}
			const std::string word(p_text.substr(i, j - i).utf8().get_data());
			Color color = font_color;
			if (previous_visible(p_text, i) == '.') {
				color = member_variable_color;
			} else if (previous_word == "func") {
				color = function_definition_color;
			} else if (const auto keyword = keywords.find(word); keyword != keywords.end()) {
				color = keyword->second;
			} else if (const auto member = member_keywords.find(word); member != member_keywords.end()) {
				color = member->second;
			} else {
				int after = j;
				while (after < length && is_space_char(p_text[after])) {
					after++;
				}
				if (after < length && p_text[after] == '(') {
					color = function_color;
				}
			}
			put_color(r_map, i, color);
			previous_word = word;
			i = j;
			continue;
		}

		if (is_digit_char(current) ||
				(current == '.' && i + 1 < length && is_digit_char(p_text[i + 1]) &&
						!is_ident_char(previous_visible(p_text, i)))) {
			int j = i;
			while (j < length) {
				const char32_t digit = p_text[j];
				if (is_digit_char(digit) || is_ident_start(digit) || digit == '_' || digit == '.') {
					j++;
					continue;
				}
				if ((digit == '+' || digit == '-') && j > i &&
						(p_text[j - 1] == 'e' || p_text[j - 1] == 'E')) {
					j++;
					continue;
				}
				break;
			}
			put_color(r_map, i, number_color);
			previous_word.clear();
			i = j;
			continue;
		}

		put_color(r_map, i, symbol_color);
		previous_word.clear();
		i++;
	}
	return -1;
}

int SafeGDScriptHighlightRules::region_at_line_start(TextEdit *p_text_edit, int p_line) {
	if (p_text_edit == nullptr || p_line <= 0) {
		return -1;
	}
	int first = p_line;
	while (first > 0 && open_region.find(first - 1) == open_region.end()) {
		first--;
	}
	int entry = first > 0 ? open_region[first - 1] : -1;
	for (int line = first; line < p_line; line++) {
		entry = scan_line(p_text_edit->get_line(line), entry, nullptr);
		open_region[line] = entry;
	}
	return entry;
}

Dictionary SafeGDScriptHighlightRules::highlight_line(TextEdit *p_text_edit, int p_line) {
	Dictionary color_map;
	if (p_text_edit == nullptr || p_line < 0 || p_line >= p_text_edit->get_line_count()) {
		return color_map;
	}
	const int entry = region_at_line_start(p_text_edit, p_line);
	open_region[p_line] = scan_line(p_text_edit->get_line(p_line), entry, &color_map);
	return color_map;
}

Dictionary SafeGDScriptCodeHighlighter::_get_line_syntax_highlighting(int32_t p_line) const {
	return rules.highlight_line(get_text_edit(), p_line);
}

void SafeGDScriptCodeHighlighter::_clear_highlighting_cache() {
	rules.clear();
}

void SafeGDScriptCodeHighlighter::_update_cache() {
	rules.update(get_text_edit(), Ref<Script>());
}

PackedStringArray SafeGDScriptSyntaxHighlighter::_get_supported_languages() const {
	PackedStringArray languages;
	languages.push_back("SafeGD");
	return languages;
}

Ref<EditorSyntaxHighlighter> SafeGDScriptSyntaxHighlighter::_create() const {
	Ref<SafeGDScriptSyntaxHighlighter> highlighter;
	highlighter.instantiate();
	return highlighter;
}

Dictionary SafeGDScriptSyntaxHighlighter::_get_line_syntax_highlighting(int32_t p_line) const {
	return rules.highlight_line(get_text_edit(), p_line);
}

void SafeGDScriptSyntaxHighlighter::_clear_highlighting_cache() {
	rules.clear();
}

void SafeGDScriptSyntaxHighlighter::_update_cache() {
	// _get_edited_resource is absent from godot-cpp.
	Ref<Script> edited;
	if (has_method("_get_edited_resource")) {
		edited = call("_get_edited_resource");
	}
	rules.update(get_text_edit(), edited);
}
