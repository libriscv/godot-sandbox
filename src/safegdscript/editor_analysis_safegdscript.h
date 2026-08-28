#pragma once

#include "../gdscript/compiler/source_model.h"

#include <godot_cpp/core/object.hpp>

using namespace godot;

struct EditorResolvedSymbol {
	const gdscript::SourceDeclaration *declaration = nullptr;
	String script_path;
	String resolved_type;
	int32_t line = -1;
};

class EditorSymbolResolver {
	const gdscript::SourceModel &model;
	String source_path;
	Object *owner = nullptr;

	int32_t containing_function(uint32_t p_line) const;

public:
	EditorSymbolResolver(const gdscript::SourceModel &p_model, const String &p_source_path,
			Object *p_owner = nullptr);
	EditorResolvedSymbol resolve(const StringName &p_name, uint32_t p_line) const;
	std::vector<const gdscript::SourceDeclaration *> visible_declarations(uint32_t p_line) const;
};
