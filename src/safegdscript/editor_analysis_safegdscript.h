#pragma once

#include "../gdscript/compiler/source_model.h"

#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/core/object.hpp>

using namespace godot;

struct EditorResolvedSymbol {
	const gdscript::SourceDeclaration *declaration = nullptr;
	String script_path;
	String resolved_type;
	int32_t line = -1;
};

// `meta` = the type itself (Vector2.), not a value.
struct EditorTypeRef {
	enum Kind : uint8_t {
		NONE,
		ENGINE_CLASS,
		BUILTIN,
		SCRIPT,
		STRUCT,
		NESTED_CLASS,
		TRAIT,
		SCRIPT_ENUM,
	};
	Kind kind = NONE;
	String name;
	Ref<Script> script;
	int32_t declaration = -1;
	bool meta = false;

	bool is_valid() const { return kind != NONE; }
};

struct EditorMemberTarget {
	enum Kind : uint8_t { NONE, CLASS, METHOD, PROPERTY, SIGNAL, CONSTANT, ENUM };
	Kind kind = NONE;
	String class_name;
	String class_member;
	String script_path;
	String doc_type;
	String description;
	int32_t line = -1;

	bool is_valid() const { return kind != NONE; }
};

class EditorSymbolResolver {
	const gdscript::SourceModel &model;
	String source_path;
	Object *owner = nullptr;

	int32_t containing_function(uint32_t p_line) const;
	EditorTypeRef step(const EditorTypeRef &p_current, const String &p_segment) const;
	EditorTypeRef resolve_head(const String &p_segment, uint32_t p_line) const;

public:
	EditorSymbolResolver(const gdscript::SourceModel &p_model, const String &p_source_path,
			Object *p_owner = nullptr);
	EditorResolvedSymbol resolve(const StringName &p_name, uint32_t p_line) const;
	std::vector<const gdscript::SourceDeclaration *> visible_declarations(uint32_t p_line) const;

	int32_t find_member(const String &p_name, gdscript::DeclarationKind p_kind) const;
	EditorTypeRef resolve_type_name(const String &p_name) const;
	EditorTypeRef resolve_receiver(const String &p_chain, uint32_t p_line) const;

	EditorMemberTarget member_of_type(const EditorTypeRef &p_type, const String &p_symbol,
			const String &p_doc_class_name) const;
	static EditorMemberTarget member_of_script(const Ref<Script> &p_script, const String &p_symbol);
	static EditorMemberTarget member_of_engine_class(const String &p_class, const String &p_symbol);
	static EditorMemberTarget member_of_builtin(const String &p_type, const String &p_symbol);
	static String doc_name_of_script(const Ref<Script> &p_script);
	// get_member_line is not in the GDExtension API.
	static int32_t member_line_of_script(const Ref<Script> &p_script, const String &p_symbol);

	static PackedStringArray split_chain(const String &p_chain);
	static Ref<Script> script_for_global_class(const String &p_name);
	static Ref<Script> script_for_autoload(const String &p_name);
};
