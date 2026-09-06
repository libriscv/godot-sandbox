#pragma once
#include "function_signature.h"
#include <algorithm>

namespace gdscript {
// One rule shared by source validation and ELF loading. Parameters are
// contravariant, returns covariant; nominal types never collapse to OBJECT.
inline bool trait_accepts(const DeclaredType &w, const DeclaredType &n) {
	if (w.name.empty() || w.name == "Variant") return true;
	if (n.name.empty() || n.name == "Variant") return false;
	if (w.name == n.name) return true;
	if ((w.nominal || n.nominal) && w.name != "Object") return false;
	return w.mask && n.mask && (n.mask & ~w.mask) == 0;
}
inline std::string trait_method_signature(const std::string &trait, const FunctionSignature &s) {
	std::string text = trait + "." + s.name + "(";
	for (size_t i = 0; i < s.parameters.size(); ++i) {
		if (i) text += ", ";
		text += s.parameters[i].name + ": " + (s.parameters[i].declared_type.name.empty() ? "Variant" : s.parameters[i].declared_type.name);
	}
	return text + ") -> " + (s.declared_return.name.empty() ? "Variant" : s.declared_return.name);
}
inline std::string validate_trait_signature(const FunctionSignature *actual,
		const FunctionSignature &required) {
	if (!actual) return required.is_abstract ? "does not implement abstract method '" + required.name + "()'" : "method '" + required.name + "' is not available";
	if (actual->is_static != required.is_static) return actual->is_static ? "is static; the trait declares an instance method" : "is an instance method; the trait declares it static";
	if (actual->parameters.size() != required.parameters.size()) return "takes " + std::to_string(actual->parameters.size()) + " parameters, trait declares " + std::to_string(required.parameters.size());
	// Old ELFs have concrete Variant IDs but no lossless declaration. Reject
	// ambiguous nominal/union contracts instead of silently accepting erasure.
	auto legacy = [](int32_t id, const std::string &name) {
		DeclaredType t;
		t.name = id < 0 ? "Variant" : (name.empty() ? (id == 0 ? "void" : id == 24 ? "Object" : "#" + std::to_string(id)) : name);
		t.mask = id >= 0 && id < 64 ? uint64_t(1) << id : 0;
		t.nominal = !name.empty();
		return t;
	};
	for (size_t i = 0; i < required.parameters.size(); ++i) {
		const auto &p = actual->parameters[i];
		const auto a = actual->has_declaration ? p.declared_type : legacy(p.type, p.class_name);
		if (!trait_accepts(a, required.parameters[i].declared_type)) return "parameter '" + required.parameters[i].name + "' has incompatible type '" + a.name + "'; trait declares '" + required.parameters[i].declared_type.name + "'";
	}
	const auto a = actual->has_declaration ? actual->declared_return : legacy(actual->return_type, actual->return_class_name);
	if (!required.declared_return.name.empty() && (a.name.empty() || (!actual->has_declaration && actual->return_type < 0))) return "has an untyped return; trait declares '" + required.declared_return.name + "'";
	if (!required.declared_return.name.empty() && !trait_accepts(required.declared_return, a)) return "returns '" + a.name + "'; trait declares '" + required.declared_return.name + "'";
	return {};
}
inline std::vector<std::string> trait_conformance(const std::vector<FunctionSignature> &functions,
		const ClassSignature &trait) {
	std::vector<std::string> errors;
	if (!trait.is_trait) { errors.push_back("'" + trait.name + "' is not a trait"); return errors; }
	for (const auto &required : trait.trait_methods) {
		const auto it = std::find_if(functions.begin(), functions.end(), [&](const auto &s) { return s.name == required.name; });
		const auto error = validate_trait_signature(it == functions.end() ? nullptr : &*it, required);
		if (!error.empty()) errors.push_back(trait_method_signature(trait.name, required) + ": " + error);
	}
	return errors;
}
inline bool metadata_uses(const std::vector<std::string> &uses,
		const std::vector<ClassSignature> &classes, const std::string &name) {
	std::vector<std::string> pending = uses, seen;
	while (!pending.empty()) {
		auto next = pending.back(); pending.pop_back();
		if (next == name) return true;
		if (std::find(seen.begin(), seen.end(), next) != seen.end()) continue;
		seen.push_back(next);
		for (const auto &c : classes) if (c.is_trait && c.name == next) pending.insert(pending.end(), c.uses.begin(), c.uses.end());
	}
	return false;
}
} // namespace gdscript
