#pragma once
#include "function_signature.h"
#include <algorithm>
#include <functional>

namespace gdscript {
// Supplied by the compiler's engine ancestry table or the host's ClassDB.
// Without a registry, distinct native names are conservatively incompatible.
using TraitNativeSubtype = std::function<bool(const std::string &, const std::string &)>;

inline std::vector<std::string> trait_type_members(const std::string &name) {
	std::vector<std::string> members;
	size_t begin = 0;
	int depth = 0;
	for (size_t i = 0; i <= name.size(); ++i) {
		if (i < name.size()) {
			if (name[i] == '[') ++depth;
			if (name[i] == ']') --depth;
		}
		if (i != name.size() && (name[i] != '|' || depth != 0)) continue;
		auto part = name.substr(begin, i - begin);
		const auto first = part.find_first_not_of(" \t");
		if (first != std::string::npos) {
			part = part.substr(first, part.find_last_not_of(" \t") - first + 1);
			if (part.back() == '?') { part.pop_back(); members.push_back("null"); }
			members.push_back(part);
		}
		begin = i + 1;
	}
	return members;
}
// One rule shared by source validation and ELF loading. Parameters are
// contravariant, returns covariant; nominal types never collapse to OBJECT.
inline bool trait_accepts(const DeclaredType &w, const DeclaredType &n,
		const TraitNativeSubtype &native_subtype = {}) {
	if (w.name.empty() || w.name == "Variant") return true;
	if (n.name.empty() || n.name == "Variant") return false;
	if (w.name == n.name) return true;
	if (!w.mask || !n.mask || (n.mask & ~w.mask)) return false;
	constexpr uint64_t object = uint64_t(1) << 24, nil = 1;
	const bool containers = w.name.find('[') != std::string::npos || n.name.find('[') != std::string::npos;
	if (!containers && !w.nominal && !n.nominal && !((w.mask | n.mask) & object)) return true;
	const auto wide = trait_type_members(w.name), narrow = trait_type_members(n.name);
	// Object accepts every native object, including unions of native classes.
	if (!(n.mask & ~(object | nil)) && std::find(wide.begin(), wide.end(), "Object") != wide.end()) return true;
	for (const auto &member : narrow) {
		const bool accepted = std::any_of(wide.begin(), wide.end(), [&](const auto &candidate) {
			if (candidate == member) return true;
			// Mutable containers are invariant. Never erase their element types
			// to ARRAY/DICTIONARY, including containers nested in other containers.
			return !w.nominal && !n.nominal && member.find('[') == std::string::npos &&
				candidate.find('[') == std::string::npos && native_subtype && native_subtype(member, candidate);
		});
		if (!accepted) return false;
	}
	return true;
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
		const FunctionSignature &required, const TraitNativeSubtype &native_subtype = {}) {
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
		if (!trait_accepts(a, required.parameters[i].declared_type, native_subtype)) return "parameter '" + required.parameters[i].name + "' has incompatible type '" + a.name + "'; trait declares '" + required.parameters[i].declared_type.name + "'";
	}
	const auto a = actual->has_declaration ? actual->declared_return : legacy(actual->return_type, actual->return_class_name);
	if (!required.declared_return.name.empty() && (a.name.empty() || (!actual->has_declaration && actual->return_type < 0))) return "has an untyped return; trait declares '" + required.declared_return.name + "'";
	if (!required.declared_return.name.empty() && !trait_accepts(required.declared_return, a, native_subtype)) return "returns '" + a.name + "'; trait declares '" + required.declared_return.name + "'";
	return {};
}
inline std::vector<std::string> trait_conformance(const std::vector<FunctionSignature> &functions,
		const ClassSignature &trait, const TraitNativeSubtype &native_subtype = {}) {
	std::vector<std::string> errors;
	if (!trait.is_trait) { errors.push_back("'" + trait.name + "' is not a trait"); return errors; }
	for (const auto &required : trait.trait_methods) {
		const auto it = std::find_if(functions.begin(), functions.end(), [&](const auto &s) { return s.name == required.name; });
		const auto error = validate_trait_signature(it == functions.end() ? nullptr : &*it, required, native_subtype);
		if (!error.empty()) errors.push_back(trait_method_signature(trait.name, required) + ": " + error);
	}
	return errors;
}
// A callback implementation is checked in the same direction as ordinary
// obligations: it must accept the host's arguments and return what it expects.
inline std::vector<std::string> required_host_hooks(const std::vector<FunctionSignature> &functions,
		const ClassSignature &host, const TraitNativeSubtype &native_subtype = {}) {
	std::vector<std::string> errors;
	for (const auto &hook : functions) {
		if (!hook.requires_host_hook) continue;
		const std::string label = "Required host hook '" + host.name + "." + hook.name + "'";
		const auto offered = std::find_if(host.trait_methods.begin(), host.trait_methods.end(),
			[&](const auto &method) { return method.name == hook.name; });
		if (!host.is_trait || offered == host.trait_methods.end()) {
			errors.push_back(label + " is not supported by this game (the mod may target a newer API or a removed hook)");
		} else if (hook.is_static || hook.is_abstract || offered->is_static) {
			errors.push_back(label + " requires an instance implementation and an instance host declaration");
		} else {
			const auto error = validate_trait_signature(&hook, *offered, native_subtype);
			if (!error.empty()) errors.push_back(label + " is incompatible: " + error);
		}
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
