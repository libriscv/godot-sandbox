#include "editor_analysis_safegdscript.h"

#include <algorithm>

EditorSymbolResolver::EditorSymbolResolver(const gdscript::SourceModel &p_model,
		const String &p_source_path, Object *p_owner) :
		model(p_model), source_path(p_source_path), owner(p_owner) {}

int32_t EditorSymbolResolver::containing_function(uint32_t p_line) const {
	int32_t best = -1;
	for (size_t i = 0; i < model.declarations.size(); i++) {
		const gdscript::SourceDeclaration &declaration = model.declarations[i];
		if (declaration.kind == gdscript::DeclarationKind::FUNCTION &&
				declaration.lexical_scope.start_line <= p_line &&
				p_line <= declaration.lexical_scope.end_line) {
			best = int32_t(i);
		}
	}
	return best;
}

std::vector<const gdscript::SourceDeclaration *> EditorSymbolResolver::visible_declarations(
		uint32_t p_line) const {
	const int32_t function = containing_function(p_line);
	std::vector<const gdscript::SourceDeclaration *> result;
	for (const gdscript::SourceDeclaration &declaration : model.declarations) {
		const bool local = declaration.parent >= 0 &&
				(declaration.kind == gdscript::DeclarationKind::VARIABLE ||
				 declaration.kind == gdscript::DeclarationKind::CONSTANT ||
				 declaration.kind == gdscript::DeclarationKind::PARAMETER);
		if (local) {
			if (declaration.parent != function || declaration.declaration.start_line > p_line ||
					p_line > declaration.lexical_scope.end_line) {
				continue;
			}
		} else if (declaration.parent >= 0 && declaration.parent != function) {
			continue;
		}
		result.push_back(&declaration);
	}
	std::stable_sort(result.begin(), result.end(), [function](const auto *a, const auto *b) {
		const bool a_local = a->parent == function;
		const bool b_local = b->parent == function;
		if (a_local != b_local) return a_local;
		return a->declaration.start_line > b->declaration.start_line;
	});
	return result;
}

EditorResolvedSymbol EditorSymbolResolver::resolve(const StringName &p_name, uint32_t p_line) const {
	const String wanted(p_name);
	for (const gdscript::SourceDeclaration *declaration : visible_declarations(p_line)) {
		if (String::utf8(declaration->name.c_str(), declaration->name.size()) != wanted) continue;
		EditorResolvedSymbol result;
		result.declaration = declaration;
		result.script_path = source_path;
		result.resolved_type = String::utf8(
				(declaration->resolved_type.empty() ? declaration->declared_type : declaration->resolved_type).c_str());
		result.line = int32_t(declaration->declaration.start_line);
		return result;
	}
	return {};
}
