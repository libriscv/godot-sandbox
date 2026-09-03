#include <algorithm>
#include "codegen.h"
#include "syscall_numbers.h"
#include "compiler_exception.h"
#include <functional>
#include <map>
#include <stdexcept>
#include <sstream>
#include <cstring>
#include <cmath>

namespace gdscript {

void CodeGenerator::set_engine_ancestry(
	const std::vector<std::pair<std::string, std::string>>& pairs) {
	m_engine_ancestry = pairs;
}

bool CodeGenerator::engine_class_derives_from(const std::string& actual,
	const std::string& required) const {
	if (actual.empty() || required.empty()) return false;
	for (const auto& pair : m_engine_ancestry) {
		if (pair.first != actual) continue;
		size_t begin = 0;
		while (begin <= pair.second.size()) {
			const size_t end = pair.second.find(',', begin);
			const size_t length = (end == std::string::npos ? pair.second.size() : end) - begin;
			if (pair.second.compare(begin, length, required) == 0) return true;
			if (end == std::string::npos) break;
			begin = end + 1;
		}
		return false;
	}
	return false;
}

static IRInstruction::TypeHint type_hint_from_string(const std::string& type_str) {
	const Variant::Type type = Variant::type_from_name(type_str);
	if (type != Variant::VARIANT_MAX) {
		return static_cast<IRInstruction::TypeHint>(type);
	}
	// `var s: Side` is an int, the same as a script's own enum. A declared enum
	// of the same name shadows the global one, but it is an int either way.
	if (is_global_enum(type_str)) {
		return static_cast<IRInstruction::TypeHint>(Variant::INT);
	}
	return IRInstruction::TypeHint_NONE;
}

CodeGenerator::CodeGenerator() {}

TypeSet CodeGenerator::type_set_from(const TypeExpr& expr, int line, int column) const {
	if (expr.empty()) {
		return {};
	}
	if (expr.names.empty()) {
		error_at("'null' alone is not a type", line, column,
			"Add another member, for example 'Object | null'");
	}

	const bool union_type = expr.is_union();
	TypeSet result;
	std::unordered_set<std::string> seen;
	for (const std::string& name : expr.names) {
		if (!seen.insert(name).second) {
			error_at("Union type repeats member '" + name + "'", line, column);
		}
		if (name == "Variant") {
			if (union_type) {
				error_at("'Variant' cannot be a member of a union type", line, column,
					"Variant already accepts every type; remove the other members");
			}
			return {};
		}

		Variant::Type resolved = Variant::type_from_name(name);
		if (resolved == Variant::VARIANT_MAX && name == "Object") {
			resolved = Variant::OBJECT;
		}
		if (resolved == Variant::VARIANT_MAX &&
			(is_global_enum(name) || m_enums.find(name) != m_enums.end())) {
			resolved = Variant::INT;
		}
		if (const StructDecl* structure = find_struct(name)) {
			if (union_type && !(expr.names.size() == 1 && expr.nullable)) {
				error_at("Script " + std::string(structure->is_class ? "class" : "struct") +
					" '" + name + "' cannot be used in a union type yet", line, column);
			}
			resolved = Variant::DICTIONARY;
		}
		if (find_trait(name) != nullptr) {
			result.mask |= uint64_t(1) << Variant::OBJECT;
			result.mask |= uint64_t(1) << Variant::DICTIONARY;
			continue;
		}
		if (resolved == Variant::VARIANT_MAX) {
			if (!name.empty() && name.front() >= 'A' && name.front() <= 'Z') {
				resolved = Variant::OBJECT;
			} else if (union_type) {
				error_at("Unknown type '" + name + "' in union type", line, column);
			} else {
				return {};
			}
		}
		result.mask |= uint64_t(1) << resolved;
	}
	if (expr.nullable) {
		result.mask |= uint64_t(1) << Variant::NIL;
	}
	return result;
}

IRInstruction::TypeHint CodeGenerator::single_type_from(const TypeExpr& type) const {
	if (type.empty() || type.single_name() == "Variant") {
		return IRInstruction::TypeHint_NONE;
	}
	// Preserve the established single-hint behavior: engine/script class names
	// stay untyped storage even though a union resolves them to the OBJECT mask.
	// Only unions use TypeSet as their run-time constraint.
	const IRInstruction::TypeHint legacy = type_hint_from_string(type.single_name());
	if (legacy != IRInstruction::TypeHint_NONE) {
		return legacy;
	}
	return m_enums.find(type.single_name()) != m_enums.end()
		? static_cast<IRInstruction::TypeHint>(Variant::INT)
		: IRInstruction::TypeHint_NONE;
}

int32_t CodeGenerator::published_type_from(const TypeExpr& type) const {
	if (const TraitDecl* trait = find_trait(type.sole_name())) {
		return trait_required_base(*trait).empty() ? int32_t(FunctionParameter::ANY_TYPE)
			: int32_t(Variant::OBJECT);
	}
	const TypeSet set = type_set_from(type);
	if (set.is_nullable_single() && set.non_null().only() == Variant::OBJECT) {
		return int32_t(Variant::OBJECT);
	}
	if (type.is_union()) {
		return int32_t(FunctionParameter::ANY_TYPE);
	}
	if (find_struct(type.single_name()) != nullptr) {
		return int32_t(Variant::DICTIONARY);
	}
	return int32_t(single_type_from(type));
}

namespace {

// A folded const the host can answer with. Containers are deliberately absent:
// GDScript gives a const Array or Dictionary handle identity, and a copy built
// host-side would be a different container than the one the guest sees. NIL and
// RUNTIME initializers stay unpublished too -- null is the answer either way.
void publish_constant(IRProgram& ir_program, const std::string& name,
	const IRGlobalVar& global)
{
	ScriptConstant constant;
	constant.name = name;
	switch (global.init_type) {
		case IRGlobalVar::InitType::INT:
			constant.kind = ScriptConstant::Kind::INT;
			constant.value = std::get<int64_t>(global.init_value);
			break;
		case IRGlobalVar::InitType::FLOAT:
			constant.kind = ScriptConstant::Kind::FLOAT;
			constant.value = std::get<double>(global.init_value);
			break;
		case IRGlobalVar::InitType::BOOL:
			constant.kind = ScriptConstant::Kind::BOOL;
			constant.value = std::get<bool>(global.init_value);
			break;
		case IRGlobalVar::InitType::STRING:
			constant.kind = ScriptConstant::Kind::STRING;
			constant.value = std::get<std::string>(global.init_value);
			break;
		default:
			return;
	}
	ir_program.constants.push_back(std::move(constant));
}

bool constructs_implicitly_from(IRInstruction::TypeHint from, IRInstruction::TypeHint to) {
	switch (to) {
		case Variant::VECTOR2:  return from == Variant::VECTOR2I;
		case Variant::VECTOR2I: return from == Variant::VECTOR2;
		case Variant::VECTOR3:  return from == Variant::VECTOR3I;
		case Variant::VECTOR3I: return from == Variant::VECTOR3;
		case Variant::VECTOR4:  return from == Variant::VECTOR4I;
		case Variant::VECTOR4I: return from == Variant::VECTOR4;
		case Variant::RECT2:    return from == Variant::RECT2I;
		case Variant::RECT2I:   return from == Variant::RECT2;
		case Variant::STRING:      return from == Variant::STRING_NAME || from == Variant::NODE_PATH;
		case Variant::STRING_NAME: return from == Variant::STRING || from == Variant::NODE_PATH;
		case Variant::NODE_PATH:   return from == Variant::STRING || from == Variant::STRING_NAME;
		case Variant::COLOR:       return from == Variant::STRING;
		case Variant::BASIS:       return from == Variant::QUATERNION;
		case Variant::QUATERNION:  return from == Variant::BASIS;
		case Variant::TRANSFORM2D: return from == Variant::TRANSFORM3D;
		case Variant::TRANSFORM3D:
			return from == Variant::TRANSFORM2D || from == Variant::QUATERNION ||
				from == Variant::BASIS || from == Variant::PROJECTION;
		case Variant::PROJECTION:  return from == Variant::TRANSFORM3D;
		case Variant::ARRAY:
			switch (from) {
				case Variant::PACKED_BYTE_ARRAY:
				case Variant::PACKED_INT32_ARRAY:
				case Variant::PACKED_INT64_ARRAY:
				case Variant::PACKED_FLOAT32_ARRAY:
				case Variant::PACKED_FLOAT64_ARRAY:
				case Variant::PACKED_STRING_ARRAY:
				case Variant::PACKED_VECTOR2_ARRAY:
				case Variant::PACKED_VECTOR3_ARRAY:
				case Variant::PACKED_VECTOR4_ARRAY:
				case Variant::PACKED_COLOR_ARRAY:
					return true;
				default: return false;
			}
		default: return false;
	}
}

} // namespace

IRProgram CodeGenerator::generate(const Program& program) {
	IRProgram ir_program;
	ir_program.trait_structural_fallback = m_trait_structural_fallback;
	ir_program.is_tool = program.is_tool;

	if (!program.class_name.empty() && m_restricted) {
		error_at("'class_name " + program.class_name + "' needs a Sandbox that allows engine classes",
			program.class_name_line, program.class_name_column,
			"Registering a global class puts the script in the project's class list, which a "
			"restricted Sandbox has no way to undo. Drop the class_name.");
	}
	if (!program.base_class.empty() && m_restricted) {
		error_at("'extends " + program.base_class + "' needs a Sandbox that allows engine classes",
			program.base_class_line, program.base_class_column,
			"A restricted Sandbox refuses every class, so the base could never be instantiated. "
			"A script with no 'extends' runs under any owner.");
	}
	ir_program.class_name = program.class_name;
	ir_program.base_class = program.base_class;
	ir_program.base_is_path = program.base_is_path;
	ir_program.native_base_class = program.native_base_class;
	ir_program.native_base_is_path = program.native_base_is_path;
	m_script_base_class = program.native_base_class;
	m_chain = program.chain;
	m_current_chain_link = 0;

	// Signals are members; collected first so name collisions are caught below.
	m_signals.clear();
	for (const auto& decl : program.signals) {
		if (m_signals.count(decl.name)) {
			error_at("Signal '" + decl.name + "' is declared more than once",
				decl.line, decl.column);
		}
		m_signals[decl.name] = &decl;
	}

	// Collect local function names and signatures for default-argument filling at call sites.
	m_local_functions.clear();
	m_local_signatures.clear();
	for (const auto& func : program.functions) {
		reject_signal_collision("Function", func.name, func.line, func.column);
		m_local_functions.insert(func.name);
		m_local_signatures[func.name] = &func;
		if (func.is_test) {
			m_test_functions.insert(func.name);
		}
	}

	// Collected before lowering so declaration order does not matter.
	m_saw_breakpoint_statement = false;
	m_traits.clear();
	m_trait_indices.clear();
	for (const TraitDecl& decl : program.traits) {
		if (m_traits.count(decl.name)) {
			error_at("Trait '" + decl.name + "' is declared more than once",
				decl.line, decl.column);
		}
		if (is_global_class(decl.name) || m_local_functions.count(decl.name)) {
			error_at("Trait '" + decl.name + "' has a name that is already taken",
				decl.line, decl.column);
		}
		reject_signal_collision("Trait", decl.name, decl.line, decl.column);
		m_trait_indices[&decl] = m_trait_indices.size();
		m_traits[decl.name] = &decl;
	}
	m_structs.clear();
	m_struct_default_stack.clear();
	for (const auto& decl : program.structs) {
		const char* kind = decl.is_class ? "Class" : "Struct";
		if (m_structs.count(decl.name)) {
			error_at(std::string(kind) + " '" + decl.name + "' is declared more than once",
				decl.line, decl.column);
		}
		if (m_traits.count(decl.name)) {
			error_at(std::string(kind) + " '" + decl.name +
				"' has the name of a trait", decl.line, decl.column);
		}
		if (is_global_class(decl.name)) {
			error_at(std::string(kind) + " '" + decl.name + "' has the name of a Godot singleton",
				decl.line, decl.column,
				"Pick another name, so that '" + decl.name + "' still reaches the singleton");
		}
		if (m_local_functions.count(decl.name)) {
			error_at(std::string(kind) + " '" + decl.name + "' has the name of a function in this script",
				decl.line, decl.column);
		}
		reject_signal_collision(decl.is_class ? "Class" : "Struct", decl.name, decl.line, decl.column);
		m_structs[decl.name] = &decl;
	}
	register_classes(program);

	// Enums resolve to integers here; nothing reaches the IR.
	m_enums.clear();
	m_enum_members.clear();
	for (const TraitDecl& trait : program.traits) {
		for (const EnumDecl& decl : trait.enums) {
			if (decl.name.empty()) continue;
			const std::string qualified = trait.name + "." + decl.name;
			if (!m_enums.emplace(qualified, &decl).second) {
				error_at("Trait enum '" + qualified + "' is declared more than once",
					decl.line, decl.column);
			}
		}
	}
	for (const auto& decl : program.enums) {
		if (!decl.name.empty()) {
			if (m_enums.count(decl.name) || m_structs.count(decl.name) ||
				m_traits.count(decl.name)) {
				error_at("Enum '" + decl.name + "' has a name that is already taken",
					decl.line, decl.column);
			}
			reject_signal_collision("Enum", decl.name, decl.line, decl.column);
			m_enums[decl.name] = &decl;
		}
		for (const auto& member : decl.members) {
			// Unnamed members are file-scope; named members only reachable through the enum.
			if (decl.name.empty()) {
				reject_signal_collision("Enum member", member.name, member.line, member.column);
				auto existing = m_enum_members.find(member.name);
				if (existing != m_enum_members.end() &&
					(existing->second->value != member.value ||
					 existing->second->value_expr != member.value_expr)) {
					error_at("Enum member '" + member.name + "' is declared more than once"
						" with different values", member.line, member.column);
				}
				m_enum_members[member.name] = &member;
			}
		}
	}

	validate_uses(program);
	ir_program.script_uses = program.uses;
	for (const TraitDecl& decl : program.traits) {
		ir_program.trait_signatures.push_back(build_trait_signature(decl));
	}

	// Validate declaration-only type hints now that enums and script types are
	// known. Some of these declarations never need executable lowering (signals,
	// unused records), but malformed unions must still be diagnosed.
	for (const SignalDecl& signal : program.signals) {
		for (const Parameter& parameter : signal.parameters) {
			type_set_from(parameter.type_hint, parameter.line, parameter.column);
		}
	}
	for (const StructDecl& structure : program.structs) {
		for (const StructField& field : structure.fields) {
			type_set_from(field.type_hint, field.line, field.column);
		}
		for (const FunctionDecl& method : structure.methods) {
			type_set_from(method.return_type, method.line, method.column);
			for (const Parameter& parameter : method.parameters) {
				type_set_from(parameter.type_hint, parameter.line, parameter.column);
			}
		}
	}

	// The host reads these off a script instance; the guest folds them and keeps
	// no storage. An engine-constant initializer has no compile-time value
	// (gen_enum_member re-evaluates it per use), so an enum holding one is left
	// unpublished rather than published with a wrong member.
	for (const auto& decl : program.enums) {
		bool foldable = true;
		for (const auto& member : decl.members) {
			if (member.value_expr != nullptr) {
				foldable = false;
				break;
			}
		}
		if (!foldable) {
			continue;
		}
		if (decl.name.empty()) {
			// File-scope members; the enum itself has no name to answer to.
			for (const auto& member : decl.members) {
				ScriptConstant constant;
				constant.name = member.name;
				constant.kind = ScriptConstant::Kind::INT;
				constant.value = member.value;
				ir_program.constants.push_back(std::move(constant));
			}
			continue;
		}
		ScriptConstant constant;
		constant.name = decl.name;
		constant.kind = ScriptConstant::Kind::ENUM;
		for (const auto& member : decl.members) {
			constant.members.push_back({ member.name, member.value });
		}
		ir_program.constants.push_back(std::move(constant));
	}

	// After structs, so a struct parameter type resolves to Dictionary.
	for (const auto& decl : program.signals) {
		ir_program.signals.push_back(build_signal_signature(decl));
	}

	// Constants fold into InitType; everything else evaluates in global_init.
	// Anything reaching neither path is an error: silent NIL caused regressions.
	m_global_variables.clear();
	m_global_consts.clear();
	m_global_const_values.clear();
	m_global_types.clear();
	m_global_sets.clear();
	m_global_type_names.clear();
	m_global_structs.clear();
	m_global_traits.clear();
	m_global_array_element_structs.clear();
	m_global_dictionary_value_structs.clear();
	m_global_array_element_traits.clear();
	m_global_dictionary_value_traits.clear();
	m_global_is_member.clear();
	m_global_holds_object.clear();
	ir_program.globals.resize(program.globals.size());

	// All names registered before any initializer, so initializers can reference each other.
	for (size_t i = 0; i < program.globals.size(); i++) {
		const auto& global = program.globals[i];
		if (m_global_variables.count(global.name)) {
			error_at("Global variable '" + global.name + "' is declared more than once",
				global.line, global.column);
		}
		if (find_struct(global.name) != nullptr) {
			error_at("Global variable '" + global.name + "' has the name of a struct",
				global.line, global.column,
				"'" + global.name + "' would no longer name the struct in this script");
		}
		if (find_trait(global.name) != nullptr) {
			error_at("Global variable '" + global.name + "' has the name of a trait",
				global.line, global.column);
		}
		reject_signal_collision("Variable", global.name, global.line, global.column);
		m_global_variables[global.name] = i;
		if (global.is_const) {
			m_global_consts.insert(global.name);
		}
		m_global_is_member.push_back(!global.is_const && !global.is_static);

		// Struct-typed global: DICTIONARY downstream, struct tracked for field checking.
		const StructDecl* global_struct = find_struct(global.type_hint.sole_name());
		m_global_structs.push_back(global_struct);
		const TraitDecl* global_trait = find_trait(global.type_hint.sole_name());
		m_global_traits.push_back(global_trait);
		const StructDecl* array_element = nullptr;
		const StructDecl* dictionary_value = nullptr;
		const TraitDecl* array_element_trait = nullptr;
		const TraitDecl* dictionary_value_trait = nullptr;
		if (global.type_hint.single_name() == "Array" && global.type_hint.arguments.size() == 1) {
			array_element = find_struct(global.type_hint.arguments[0].single_name());
			array_element_trait = find_trait(global.type_hint.arguments[0].sole_name());
		} else if (global.type_hint.single_name() == "Dictionary" &&
			global.type_hint.arguments.size() == 2) {
			dictionary_value = find_struct(global.type_hint.arguments[1].single_name());
			dictionary_value_trait = find_trait(global.type_hint.arguments[1].sole_name());
		}
		m_global_array_element_structs.push_back(array_element);
		m_global_dictionary_value_structs.push_back(dictionary_value);
		m_global_array_element_traits.push_back(array_element_trait);
		m_global_dictionary_value_traits.push_back(dictionary_value_trait);
		const TypeSet declared = type_set_from(global.type_hint, global.line, global.column);
		// A class hint accepts null without needing `?`, like an engine object hint.
		const bool nullable_script_class = global_struct != nullptr && global_struct->is_class;
		TypeSet global_set = declared;
		if (nullable_script_class) global_set.mask |= uint64_t(1) << Variant::NIL;
		m_global_sets.push_back((global.type_hint.is_union() || nullable_script_class)
			? global_set : TypeSet{});
		m_global_type_names.push_back(global.type_hint.to_string());
		if (global.type_hint.is_union() && !declared.is_nullable_single() && global.is_property) {
			error_at("A union-typed variable cannot be exported", global.line, global.column,
				"The inspector has no property hint for union types yet");
		}
		if (global_struct != nullptr && !nullable_script_class) {
			m_global_types.push_back(Variant::DICTIONARY);
		} else {
			m_global_types.push_back(global.type_hint.is_union()
				? IRInstruction::TypeHint_NONE : single_type_from(global.type_hint));
		}
		m_global_holds_object.push_back(declared.contains(Variant::OBJECT));
	}

	collect_property_accessors(program);

	FunctionContext init_func;
	init_func.ir.name = "__init_globals";
	FunctionContext member_func;
	member_func.ir.name = "__init_members";
	m_current_function = "global initializers";
	m_globals_lowered = 0;
	push_scope(init_func);
	push_scope(member_func);

	for (size_t i = 0; i < program.globals.size(); i++) {
		const auto& global = program.globals[i];
		IRGlobalVar& ir_global = ir_program.globals[i];

		ir_global.name = global.name;
		if (const TraitDecl* trait = find_trait(global.type_hint.sole_name()); trait != nullptr) {
			ir_global.class_name = trait_required_base(*trait);
		} else {
			ir_global.class_name = global.type_hint.sole_name();
		}
		ir_global.declaration_line = global.line > 0 ? uint32_t(global.line) : 0;
		ir_global.is_const = global.is_const;
		ir_global.is_property = global.is_property;
		ir_global.is_static = global.is_static;
		ir_global.export_hint = global.export_hint;
		ir_global.setter_function = m_global_setters[i];
		ir_global.getter_function = m_global_getters[i];
		ir_global.storage = (global.is_const || global.is_static)
			? IRGlobalVar::Storage::Data
			: IRGlobalVar::Storage::Instance;

		m_current_chain_link = global.chain_link;
		FunctionContext& target = ir_global.is_member() ? member_func : init_func;
		bool& target_has_init = ir_global.is_member()
			? ir_program.has_member_init
			: ir_program.has_global_init;
		m_members_in_scope = ir_global.is_member();
		const bool nullable_script_class = m_global_structs[i] != nullptr &&
			m_global_structs[i]->is_class;

		if (!global.type_hint.empty()) {
			ir_global.type_hint = m_global_types[i];
			if (global.type_hint.is_union() || nullable_script_class) {
				ir_global.declared_set = m_global_sets[i].mask;
			}
		}

		if (global.is_onready && global.type_hint.empty() && !global.initializer) {
			ir_global.init_type = IRGlobalVar::InitType::NULL_VAL;
			m_globals_lowered = i + 1;
			continue;
		}

		// Accessor-only property: NIL storage, no type/initializer needed.
		if (global.has_accessors() && global.type_hint.empty() && !global.initializer) {
			ir_global.init_type = IRGlobalVar::InitType::NULL_VAL;
			m_globals_lowered = i + 1;
			continue;
		}

		// An untyped declaration is a Variant initialized to NIL.  Keeping
		// value_type unknown makes later stores take the generic VSTORE path,
		// which can retain containers and change the slot's run-time type.
		if (global.type_hint.empty() && !global.initializer) {
			ir_global.init_type = IRGlobalVar::InitType::NULL_VAL;
			m_globals_lowered = i + 1;
			continue;
		}

		if (!global.initializer) {
			if (global.type_hint.is_union() || nullable_script_class) {
				if (global.type_hint.nullable || nullable_script_class) {
					ir_global.init_type = IRGlobalVar::InitType::NULL_VAL;
					ir_global.value_type = IRInstruction::TypeHint_NONE;
				} else {
					int reg = gen_default_value(global.type_hint, target);
					if (reg < 0) {
						error_at("Union type '" + global.type_hint.to_string() +
							"' has no constructible default", global.line, global.column);
					}
					target.ir.instructions.emplace_back(IROpcode::STORE_GLOBAL,
						IRValue::imm(static_cast<int64_t>(i)), IRValue::reg(reg));
					free_register(target, reg);
					ir_global.init_type = IRGlobalVar::InitType::RUNTIME;
					ir_global.value_type = IRInstruction::TypeHint_NONE;
					target_has_init = true;
				}
				m_globals_lowered = i + 1;
				continue;
			}
			// `var a: BankAccount` — a struct is a value, so the declaration is a
			// fresh instance at startup. A class is an object and stays null, as
			// it does in GDScript; constructing one here would also run its bind
			// while the machine is still loading.
			if (const StructDecl* global_struct = m_global_structs[i];
				global_struct != nullptr && !global_struct->is_class) {
				int reg = gen_struct_construct(*global_struct, {}, NamedArguments{}, target, nullptr);
				target.ir.instructions.emplace_back(IROpcode::STORE_GLOBAL,
					IRValue::imm(static_cast<int64_t>(i)), IRValue::reg(reg));
				free_register(target, reg);
				ir_global.init_type = IRGlobalVar::InitType::RUNTIME;
				ir_global.value_type = Variant::DICTIONARY;
				target_has_init = true;
				m_globals_lowered = i + 1;
				continue;
			}

			// `var a: Array` defaults to empty Array, not NIL.
			apply_default_initializer(ir_global, target, i, target_has_init);
			m_globals_lowered = i + 1;
			continue;
		}

		{
			if (fold_global_initializer(global.initializer.get(), ir_global)) {
				if (m_global_traits[i] != nullptr &&
					!(global.type_hint.nullable &&
						ir_global.init_type == IRGlobalVar::InitType::NULL_VAL)) {
					error_at("The initializer of global '" + global.name +
						"' does not implement '" + m_global_traits[i]->name + "'",
						global.line, global.column);
				}
				if (m_global_structs[i] != nullptr && !m_global_structs[i]->is_class) {
					if (ir_global.init_type != IRGlobalVar::InitType::EMPTY_DICT ||
						!struct_fields(*m_global_structs[i]).empty()) {
						error_at("The initializer of global '" + global.name +
							"' is not a '" + m_global_structs[i]->name + "'", global.line,
							global.column);
					}
				}
				coerce_folded_initializer(ir_global, global.type_hint,
					global.line, global.column);
				ir_global.value_type = ir_global.declared_set != 0
					? IRInstruction::TypeHint_NONE : derive_global_value_type(ir_global);
			} else {
				// Not a compile-time constant: evaluate it at startup.
				const size_t before = target.ir.instructions.size();
				int reg = gen_expr(global.initializer.get(), target);
				if (m_global_traits[i] != nullptr) {
					reg = require_trait_value(reg, *m_global_traits[i],
						"global '" + global.name + "'", target, global.line, global.column,
						global.type_hint.nullable);
				}
				if (m_global_structs[i] != nullptr && !m_global_structs[i]->is_class) {
					reg = require_struct_value(reg, *m_global_structs[i],
						"global '" + global.name + "'", target, global.line, global.column);
				}
				if (global.type_hint.is_union() || nullable_script_class) {
					reg = coerce_to_declared_type(reg, m_global_sets[i], target,
						"global '" + global.name + "'", global.line, global.column,
						global.type_hint.to_string());
				} else if (m_global_structs[i] == nullptr && m_global_traits[i] == nullptr) {
					reg = coerce_to_declared_type(reg, m_global_types[i], target,
						"global '" + global.name + "'", global.line, global.column);
				}
				// ECALL_CALL_GUEST would reset SP/RA on the level-0 state.
				for (size_t k = before; k < target.ir.instructions.size(); k++) {
					if (target.ir.instructions[k].opcode == IROpcode::CALL_HOSTED) {
						error_at("Cannot call the coroutine '" +
							operand_text(target.ir.instructions[k].operands[0]) +
							"' while initializing '" + global.name + "'",
							global.line, global.column,
							"a coroutine can only be called from a function Godot invoked");
					}
				}
				target.ir.instructions.emplace_back(IROpcode::STORE_GLOBAL,
					IRValue::imm(static_cast<int64_t>(i)), IRValue::reg(reg));
				ir_global.init_type = IRGlobalVar::InitType::RUNTIME;
				ir_global.value_type = ir_global.declared_set != 0
					? IRInstruction::TypeHint_NONE
					: ir_global.type_hint != IRInstruction::TypeHint_NONE
					? ir_global.type_hint
					: get_register_type(target, reg);
				if (get_register_type(target, reg) == Variant::OBJECT) {
					mark_global_holds_object(static_cast<int64_t>(i));
				}
				if (m_global_structs[i] == nullptr) {
					m_global_structs[i] = get_register_struct(target, reg);
				}
				free_register(target, reg);
				target_has_init = true;
			}

			if (global.is_const && ir_global.init_type != IRGlobalVar::InitType::RUNTIME) {
				m_global_const_values[global.name] = ir_global;
			}

			if (global.is_const) {
				publish_constant(ir_program, global.name, ir_global);
			}
		}

		m_globals_lowered = i + 1;
	}

	m_current_chain_link = 0;
	if (ir_program.has_global_init) {
		init_func.ir.instructions.emplace_back(IROpcode::RETURN);
		init_func.ir.max_registers = init_func.next_register;
	} else {
		init_func.ir.instructions.clear();
		init_func.ir.max_registers = 0;
	}
	if (ir_program.has_member_init) {
		member_func.ir.instructions.emplace_back(IROpcode::RETURN);
		member_func.ir.max_registers = member_func.next_register;
	} else {
		member_func.ir.instructions.clear();
		member_func.ir.max_registers = 0;
	}
	pop_scope(init_func);
	pop_scope(member_func);
	m_members_in_scope = true;
	ir_program.global_init = std::move(init_func.ir);
	ir_program.member_init = std::move(member_func.ir);

	m_globals_lowered = SIZE_MAX;

	// After the globals, so a class constant may be written in terms of one.
	register_class_constants(program);

	m_pending_lambdas.clear();
	m_next_lambda = 0;

	for (const auto& decl : program.functions) {
		ir_program.signatures.push_back(build_signature(decl));
		ir_program.functions.push_back(generate_function(decl));
		if (decl.rpc_config.has_value() && decl.chain_name.empty()) {
			RPCConfig config = *decl.rpc_config;
			config.name = decl.name;
			ir_program.rpc_configs.push_back(std::move(config));
		}
		if (decl.is_test && decl.chain_name.empty()) {
			ir_program.tests.push_back(ir_program.signatures.back());
		}
	}

	// Inline accessor bodies (`@x_setter`/`@x_getter`, hidden from method list).
	for (const auto& global : program.globals) {
		for (const FunctionDecl* accessor : { global.setter_body.get(), global.getter_body.get() }) {
			if (accessor == nullptr) {
				continue;
			}
			FunctionSignature signature;
			signature.name = accessor->name;
			signature.line = accessor->line;
			ir_program.signatures.push_back(std::move(signature));
			ir_program.functions.push_back(generate_function(*accessor));
		}
	}

	emit_missing_export_accessors(ir_program);

	for (const StructDecl& decl : program.structs) {
		for (const FunctionDecl& method : decl.methods) {
			// The synthetic self slot is not part of the declaration, so the
			// signature the host checks arity against never mentions it.
			FunctionSignature signature = build_signature(method);
			signature.name = lifted_method_name(decl, method.name);
			ir_program.signatures.push_back(std::move(signature));
			ir_program.functions.push_back(generate_function(method, &decl));
		}
		const std::string* engine_base = native_base(decl);
		// Plain nested classes are guest-only and have no Script resource for the
		// host to attach. Structs are the exception: their signature is editor
		// metadata even though their runtime value remains a Dictionary.
		if (!decl.is_class || engine_base != nullptr) {
			ir_program.class_signatures.push_back(build_class_signature(decl,
				engine_base != nullptr ? *engine_base : std::string()));
		}
	}

	// Queue grows while iterating (nested lambdas append).
	for (size_t i = 0; i < m_pending_lambdas.size(); i++) {
		const PendingLambda pending = m_pending_lambdas[i];
		m_current_function = pending.lifted_name;

		FunctionSignature signature;
		signature.name = pending.lifted_name;
		signature.line = pending.decl->line;
		ir_program.signatures.push_back(std::move(signature));

		m_current_class = pending.owner;
		m_current_chain_link = pending.chain_link;
		m_current_chain_function = pending.chain_function;
		m_in_static_function = pending.in_static_function;
		IRFunction lifted = generate_lambda_function(*pending.decl, pending.captures);
		m_in_static_function = false;
		m_current_class = nullptr;
		m_current_chain_link = 0;
		m_current_chain_function.clear();
		lifted.name = pending.lifted_name;
		ir_program.functions.push_back(std::move(lifted));
	}
	m_pending_lambdas.clear();

	for (size_t i = 0; i < ir_program.globals.size() && i < m_global_holds_object.size(); i++) {
		ir_program.globals[i].holds_object = m_global_holds_object[i];
	}

	ir_program.string_constants = m_string_constants;
	ir_program.strings = std::move(m_strings);
	ir_program.has_breakpoint_statement = m_saw_breakpoint_statement;
	return ir_program;
}

IRFunction CodeGenerator::generate_function(const FunctionDecl& decl, const StructDecl* owner) {
	FunctionContext func;
	func.ir.name = owner != nullptr ? lifted_method_name(*owner, decl.name) : decl.name;
	func.ir.is_coroutine = decl.is_coroutine;
	const TypeSet return_set = type_set_from(decl.return_type, decl.line, decl.column);
	func.ir.return_type_hint = decl.return_type.is_union()
		? IRInstruction::TypeHint_NONE : single_type_from(decl.return_type);
	func.ir.return_set = decl.return_type.is_union() ? return_set.mask : 0;
	m_current_function = func.ir.name;
	m_current_chain_link = decl.chain_link;
	m_current_chain_function = decl.declared_name();
	// An overridden base function is emitted under a mangled symbol, but retains
	// its declared name in chain_name.  Accessor recursion rules are based on the
	// declared name: assigning the property from either the base setter or its
	// override must write storage directly.
	enter_accessor_scope(decl.declared_name());

	func.return_type = decl.return_type;
	m_current_class = owner;
	m_in_static_function = decl.is_static;
	m_in_test_function = decl.is_test;

	push_scope(func);

	// A static method belongs to the class, not to an instance: no `self` slot.
	const bool takes_self = owner != nullptr && !decl.is_static;
	const size_t slots = decl.parameters.size() + (takes_self ? 1 : 0);
	if (slots > IRFunction::MAX_PARAMETERS) {
		error_at((owner != nullptr ? "Method '" + owner->name + "." : "Function '") + decl.name +
			"' takes " + std::to_string(decl.parameters.size()) +
			" parameters, but at most " +
			std::to_string(IRFunction::MAX_PARAMETERS - (takes_self ? 1 : 0)) +
			" can be passed",
			decl.line, decl.column,
			"Pass the extra values in an Array or Dictionary instead");
	}
	if (takes_self) {
		func.ir.parameters.push_back("self");
		int self_reg = alloc_register(func);
		declare_variable(func, "self", self_reg, false, nullptr, false, true);
		set_register_type(func, self_reg, Variant::DICTIONARY);
		set_register_struct(func, self_reg, owner);
	}
	for (size_t i = 0; i < decl.parameters.size(); i++) {
		const auto& param = decl.parameters[i];
		func.ir.parameters.push_back(param.name);

		int reg = alloc_register(func);
		declare_variable(func, param.name, reg, false, nullptr, false, true);

		apply_declared_type(reg, param.type_hint, func);
		const TypeSet param_set = type_set_from(param.type_hint, param.line, param.column);
		func.ir.param_sets.push_back(param.type_hint.is_union() ? param_set.mask : 0);
	}
	coerce_parameters(decl.parameters, func);

	for (const auto& stmt : decl.body) {
		const auto* expr_stmt = dynamic_cast<const ExprStmt*>(stmt.get());
		const auto* call = expr_stmt != nullptr
			? dynamic_cast<const CallExpr*>(expr_stmt->expression.get()) : nullptr;
		const int scope_id = call != nullptr && is_local_function(call->function_name)
			? open_scope(func) : -1;
		gen_stmt(stmt.get(), func);
		emit_scope_release(scope_id, func);
	}

	if (func.ir.instructions.empty() ||
	    func.ir.instructions.back().opcode != IROpcode::RETURN) {
		// Bare RETURN reads r0, which aliases the first parameter.
		func.ir.instructions.emplace_back(IROpcode::LOAD_NIL, IRValue::reg(0));
		func.ir.instructions.emplace_back(IROpcode::RETURN);
	}

	// r0 exists even when the body declares no registers.
	func.ir.max_registers = std::max(func.next_register, 1);
	pop_scope(func);
	m_current_class = nullptr;
	m_in_static_function = false;

	return std::move(func.ir);
}

void CodeGenerator::error_at(const std::string& message, int line, int column,
	const std::string& hint) const
{
	std::string file;
	if (size_t(m_current_chain_link) < m_chain.paths.size()) {
		file = m_chain.paths[size_t(m_current_chain_link)];
	}
	throw CompilerException(ErrorType::CODEGEN_ERROR, message, line, column,
		m_current_function, file, "", hint);
}

void CodeGenerator::error_at(const std::string& message, const Expr* expr,
	const std::string& hint) const
{
	error_at(message, expr != nullptr ? expr->line : 0, expr != nullptr ? expr->column : 0, hint);
}

void CodeGenerator::error_at(const std::string& message, const Stmt* stmt,
	const std::string& hint) const
{
	error_at(message, stmt != nullptr ? stmt->line : 0, stmt != nullptr ? stmt->column : 0, hint);
}

void CodeGenerator::gen_stmt(const Stmt* stmt, FunctionContext& func) {
	// Nested statements stamp first; unstamped remainder belongs to the outer.
	const size_t first_instruction = func.ir.instructions.size();
	gen_stmt_dispatch(stmt, func);
	for (size_t i = first_instruction; i < func.ir.instructions.size(); i++) {
		if (func.ir.instructions[i].line == 0) {
			func.ir.instructions[i].line = stmt->line;
		}
	}
}

void CodeGenerator::gen_stmt_dispatch(const Stmt* stmt, FunctionContext& func) {
	if (auto* var_decl = dynamic_cast<const VarDeclStmt*>(stmt)) {
		gen_var_decl(var_decl, func);
	} else if (auto* assign = dynamic_cast<const AssignStmt*>(stmt)) {
		gen_assign(assign, func);
	} else if (auto* ret = dynamic_cast<const ReturnStmt*>(stmt)) {
		gen_return(ret, func);
	} else if (auto* if_stmt = dynamic_cast<const IfStmt*>(stmt)) {
		gen_if(if_stmt, func);
	} else if (auto* while_stmt = dynamic_cast<const WhileStmt*>(stmt)) {
		gen_while(while_stmt, func);
	} else if (auto* for_stmt = dynamic_cast<const ForStmt*>(stmt)) {
		gen_for(for_stmt, func);
	} else if (dynamic_cast<const BreakStmt*>(stmt)) {
		gen_break(nullptr, func);
	} else if (dynamic_cast<const ContinueStmt*>(stmt)) {
		gen_continue(nullptr, func);
	} else if (auto* match_stmt = dynamic_cast<const MatchStmt*>(stmt)) {
		gen_match(match_stmt, func);
	} else if (dynamic_cast<const BreakpointStmt*>(stmt)) {
		m_saw_breakpoint_statement = true;
		auto& stop = func.ir.instructions.emplace_back(IROpcode::BREAKPOINT);
		stop.line = stmt->line;
	} else if (dynamic_cast<const PassStmt*>(stmt)) {
	} else if (auto* expr_stmt = dynamic_cast<const ExprStmt*>(stmt)) {
		gen_expr_stmt(expr_stmt, func);
	} else {
		error_at("This kind of statement is not supported by the compiler yet", stmt);
	}
}

void CodeGenerator::gen_var_decl(const VarDeclStmt* stmt, FunctionContext& func,
	bool conditional_binding) {
	TypeExpr accepted_type = stmt->type_hint;
	// Here T describes the value bound in the successful branch. NIL is the
	// no-binding case, so let it reach the tag test instead of rejecting it at
	// the declaration guard. Variant already admits NIL implicitly.
	if (conditional_binding && !accepted_type.empty() &&
		accepted_type.single_name() != "Variant") {
		accepted_type.nullable = true;
	}
	const StructDecl* declared_struct = find_struct(accepted_type.sole_name());
	const TraitDecl* declared_trait = find_trait(accepted_type.sole_name());
	const TypeSet declared_set = type_set_from(accepted_type, stmt->line, stmt->column);
	const bool nullable_single = declared_set.is_nullable_single();
	int reg = -1;

	if (stmt->initializer) {
		reg = gen_expr(stmt->initializer.get(), func);
	} else if (declared_struct != nullptr && !declared_struct->is_class && !nullable_single) {
		// A struct is a value, so a declaration is an instance. A class is an
		// object, and GDScript leaves 'var a: Inner' null.
		reg = gen_struct_construct(*declared_struct, {}, NamedArguments{}, func, nullptr);
	} else {
		// No initializer: emit the declared type's default value.
		reg = accepted_type.empty() ? -1 : gen_default_value(accepted_type, func);
		if (reg < 0) {
			// Untyped or host-only type: default to NIL.
			reg = alloc_register(func);
			IRInstruction load(IROpcode::LOAD_NIL, IRValue::reg(reg));
			load.type_hint = Variant::NIL;
			func.ir.instructions.push_back(load);
		}
	}

	if (declared_struct != nullptr && !nullable_single) {
		reg = require_struct_value(reg, *declared_struct, "variable '" + stmt->name + "'",
			func, stmt->line, stmt->column);
		declare_variable(func, stmt->name, reg, stmt->is_const, stmt);
		func.declared_structs[reg] = declared_struct;
		return;
	}
	if (declared_trait != nullptr) {
		if (stmt->initializer && m_struct_checks) {
			reg = require_trait_value(reg, *declared_trait,
				"variable '" + stmt->name + "'", func, stmt->line, stmt->column,
				accepted_type.nullable);
		}
		declare_variable(func, stmt->name, reg, stmt->is_const, stmt);
		func.declared_traits[reg].insert(declared_trait);
		func.trait_only_registers.insert(reg);
		// Only a value proves the trait. Without an initializer the slot is null,
		// exactly as GDScript leaves it, so 'is' must not fold true.
		if (stmt->initializer && !accepted_type.nullable) {
			add_register_trait(func, reg, declared_trait);
		}
		if (accepted_type.is_union()) func.declared_sets[reg] = declared_set;
		set_register_type(func, reg, IRInstruction::TypeHint_NONE);
		return;
	}

	const bool untyped_null = accepted_type.empty() && stmt->initializer != nullptr &&
		get_register_type(func, reg) == Variant::NIL;

	const bool declared_variant = accepted_type.single_name() == "Variant" || untyped_null;
	if (declared_variant) {
		// Fresh register: clearing type on the initializer's would reach other uses.
		int untyped_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(untyped_reg),
			IRValue::reg(reg));
		free_register(func, reg);
		reg = untyped_reg;
		set_register_type(func, reg, IRInstruction::TypeHint_NONE);
	} else if (accepted_type.is_union()) {
		if (stmt->initializer) {
			const std::string display = stmt->type_hint.empty()
				? accepted_type.to_string() : stmt->type_hint.to_string();
			reg = coerce_to_declared_type(reg, declared_set, func,
				"variable '" + stmt->name + "'", stmt, display);
		}
		set_register_type(func, reg, IRInstruction::TypeHint_NONE);
	} else if (!accepted_type.empty()) {
		IRInstruction::TypeHint type = single_type_from(accepted_type);
		if (type != IRInstruction::TypeHint_NONE) {
			// Coerce so the Variant payload matches the declared type.
			reg = coerce_to_declared_type(reg, type, func, "variable '" + stmt->name + "'", stmt);
			set_register_type(func, reg, type);
		}
	} else if (stmt->initializer) {
		IRInstruction::TypeHint init_type = get_register_type(func, reg);
		if (init_type != IRInstruction::TypeHint_NONE) {
			set_register_type(func, reg, init_type);
		}
	}

	declare_variable(func, stmt->name, reg, stmt->is_const, stmt, declared_variant);
	if (!accepted_type.arguments.empty()) {
		apply_declared_type(reg, accepted_type, func);
	}
	if (accepted_type.is_union()) {
		func.declared_sets[reg] = declared_set;
		if (declared_struct != nullptr && nullable_single) {
			func.declared_structs[reg] = declared_struct;
		}
	}
	if (accepted_type.empty() && stmt->initializer != nullptr && !declared_variant) {
		func.reclassifiable_registers.insert(reg);
	}
}

void CodeGenerator::gen_assign(const AssignStmt* stmt, FunctionContext& func) {
	int value_reg = gen_expr(stmt->value.get(), func);

	if (stmt->target) {
		gen_store_to(stmt->target.get(), value_reg, func, stmt);
		return;
	}

	gen_store_to_variable(stmt->name, value_reg, func, stmt);
}

void CodeGenerator::gen_store_to_variable(const std::string& name, int value_reg,
	FunctionContext& func, const Stmt* site)
{
	// Locals shadow globals.
	Variable* var = find_variable(func, name);
	if (!var) {
		if (int self_reg = class_field_self(name, func); self_reg >= 0) {
			if (const StructField* field = find_struct_field(*m_current_class, name)) {
				if (const TraitDecl* field_iface = find_trait(field->type_hint.sole_name())) {
					value_reg = require_trait_value(value_reg, *field_iface,
						"field '" + name + "' of '" + m_current_class->name + "'", func,
						site ? site->line : 0, site ? site->column : 0,
						field->type_hint.nullable);
				} else if (const StructDecl* field_struct = find_struct(field->type_hint.single_name());
					field_struct != nullptr && !field_struct->is_class) {
					value_reg = require_struct_value(value_reg, *field_struct,
						"field '" + name + "' of '" + m_current_class->name + "'", func,
						site ? site->line : 0, site ? site->column : 0);
				} else if (field->type_hint.is_union()) {
					value_reg = coerce_to_declared_type(value_reg,
						type_set_from(field->type_hint, field->line, field->column), func,
						"field '" + name + "' of class '" + m_current_class->name + "'", site,
						field->type_hint.to_string());
				}
			}
			gen_member_store(self_reg, name, value_reg, func);
			free_register(func, value_reg);
			return;
		}
		if (is_global_variable(name)) {
			if (is_global_const(name)) {
				error_at("Cannot assign to const variable: " + name, site);
			}
			reject_static_member_access(name, site ? site->line : 0, site ? site->column : 0);
			size_t global_idx = m_global_variables.at(name);
			if (m_global_structs[global_idx] != nullptr &&
				!m_global_structs[global_idx]->is_class) {
				value_reg = require_struct_value(value_reg, *m_global_structs[global_idx],
					"global '" + name + "'", func, site ? site->line : 0,
					site ? site->column : 0);
			}
			if (m_global_traits[global_idx] != nullptr) {
				value_reg = require_trait_value(value_reg, *m_global_traits[global_idx],
					"global '" + name + "'", func, site ? site->line : 0,
					site ? site->column : 0,
					m_global_sets[global_idx].contains(Variant::NIL));
			}
			if (!m_global_sets[global_idx].any()) {
				value_reg = coerce_to_declared_type(value_reg, m_global_sets[global_idx], func,
					"global '" + name + "'", site, m_global_type_names[global_idx]);
			} else {
				value_reg = coerce_to_declared_type(value_reg, m_global_types[global_idx], func,
					"global '" + name + "'", site);
			}
			if (!global_setter(global_idx).empty()) {
				gen_property_set(global_idx, value_reg, func);
				free_register(func, value_reg);
				return;
			}
			if (get_register_type(func, value_reg) == Variant::OBJECT) {
				mark_global_holds_object(static_cast<int64_t>(global_idx));
			}
			func.ir.instructions.emplace_back(IROpcode::STORE_GLOBAL, IRValue::imm(global_idx), IRValue::reg(value_reg));
			free_register(func, value_reg);
			return;
		}
		if (find_signal(name) != nullptr) {
			error_at("Cannot assign to signal '" + name + "'", site,
				"A signal is emitted with '" + name + ".emit(...)', not assigned to");
		}
		if (int base_reg = gen_implicit_base_load(func); base_reg >= 0) {
			gen_vset(base_reg, name, value_reg, func);
			free_register(func, base_reg);
			free_register(func, value_reg);
			return;
		}
		error_at("Undefined variable: " + name, site,
			"Declare it with 'var " + name + " = ...' before assigning to it");
	}

	if (var->is_const) {
		error_at("Cannot assign to const variable: " + name, site);
	}
	const bool assigned_character =
		func.string_character_registers.count(value_reg) != 0;
	const bool assigned_codepoint =
		func.codepoint_value_registers.count(value_reg) != 0;
	func.string_character_registers.erase(var->register_num);
	func.codepoint_value_registers.erase(var->register_num);
	if (assigned_character) {
		func.string_character_registers.insert(var->register_num);
	}
	if (assigned_codepoint) {
		func.codepoint_value_registers.insert(var->register_num);
	}
	if (auto declared_struct = func.declared_structs.find(var->register_num);
		declared_struct != func.declared_structs.end() &&
		!declared_struct->second->is_class) {
		value_reg = require_struct_value(value_reg, *declared_struct->second,
			"variable '" + name + "'", func, site ? site->line : 0,
			site ? site->column : 0);
	}
	if (auto declared = func.declared_traits.find(var->register_num);
		declared != func.declared_traits.end()) {
		const bool nullable = func.declared_sets.count(var->register_num) != 0 &&
			func.declared_sets.at(var->register_num).contains(Variant::NIL);
		for (const TraitDecl* iface : declared->second) {
			value_reg = require_trait_value(value_reg, *iface,
				"variable '" + name + "'", func, site ? site->line : 0,
				site ? site->column : 0, nullable);
		}
		std::unordered_set<const TraitDecl*> assigned_traits;
		if (auto proved = func.register_traits.find(value_reg);
			proved != func.register_traits.end()) assigned_traits = proved->second;
		if (var->register_num != value_reg) {
			func.ir.instructions.emplace_back(IROpcode::MOVE,
				IRValue::reg(var->register_num), IRValue::reg(value_reg));
		}
		func.register_traits.erase(var->register_num);
		func.trait_only_registers.insert(var->register_num);
		if (!nullable) {
			func.register_traits[var->register_num] = declared->second;
		} else if (!assigned_traits.empty()) {
			func.register_traits[var->register_num] = std::move(assigned_traits);
		}
		set_register_type(func, var->register_num, IRInstruction::TypeHint_NONE);
		free_register(func, value_reg);
		return;
	}

	const auto declared_set = func.declared_sets.find(var->register_num);
	if (declared_set != func.declared_sets.end()) {
		const StructDecl* assigned_struct = get_register_struct(func, value_reg);
		value_reg = coerce_to_declared_type(value_reg, declared_set->second, func,
			"variable '" + name + "'", site);
		const IRInstruction::TypeHint assigned_type = get_register_type(func, value_reg);
		if (var->register_num != value_reg) {
			func.ir.instructions.emplace_back(IROpcode::MOVE,
				IRValue::reg(var->register_num), IRValue::reg(value_reg));
		}
		set_register_type(func, var->register_num, assigned_type);
		if (assigned_type == Variant::DICTIONARY && assigned_struct != nullptr) {
			func.register_structs[var->register_num] = assigned_struct;
		} else {
			func.register_structs.erase(var->register_num);
		}
		free_register(func, value_reg);
		return;
	} else if (var->is_variant) {
		set_register_type(func, var->register_num, IRInstruction::TypeHint_NONE);
	} else {
		reject_reclassification(*var, value_reg, func, site);
		value_reg = coerce_to_declared_type(value_reg, get_register_type(func, var->register_num), func,
			"variable '" + name + "'", site);
	}

	if (var->register_num != value_reg) {
		func.ir.instructions.emplace_back(IROpcode::MOVE,
		                               IRValue::reg(var->register_num),
		                               IRValue::reg(value_reg));
	}

	free_register(func, value_reg);
}

void CodeGenerator::gen_store_to(const Expr* target, int value_reg, FunctionContext& func,
	const Stmt* site)
{
	if (auto* var_expr = dynamic_cast<const VariableExpr*>(target)) {
		gen_store_to_variable(var_expr->name, value_reg, func, site);
		return;
	}

	if (auto* index_expr = dynamic_cast<const IndexExpr*>(target)) {
		// Handles need no write-back, but the container may be a copy: resolve as lvalue.
		LValue base = resolve_lvalue(index_expr->object.get(), func);
		check_struct_subscript(base.reg, index_expr->index.get(), func);
		const bool constant_key =
			constant_dictionary_key(base.reg, index_expr->index.get(), func) != nullptr;
		int idx_reg = constant_key ? -1 : gen_expr(index_expr->index.get(), func);
		if (auto it = func.array_element_structs.find(base.reg);
			it != func.array_element_structs.end()) {
			value_reg = require_struct_value(value_reg, *it->second,
				"an element of Array[" + it->second->name + "]", func,
				site ? site->line : 0, site ? site->column : 0);
		} else if (auto it = func.dictionary_value_structs.find(base.reg);
			it != func.dictionary_value_structs.end()) {
			value_reg = require_struct_value(value_reg, *it->second,
				"a value of Dictionary[..., " + it->second->name + "]", func,
				site ? site->line : 0, site ? site->column : 0);
		}
		if (auto it = func.array_element_traits.find(base.reg);
			it != func.array_element_traits.end()) {
			value_reg = require_trait_value(value_reg, *it->second,
				"an element of Array[" + it->second->name + "]", func,
				site ? site->line : 0, site ? site->column : 0);
		} else if (auto it = func.dictionary_value_traits.find(base.reg);
			it != func.dictionary_value_traits.end()) {
			value_reg = require_trait_value(value_reg, *it->second,
				"a value of Dictionary[..., " + it->second->name + "]", func,
				site ? site->line : 0, site ? site->column : 0);
		}
		if (!gen_constant_key_store(base.reg, index_expr->index.get(), value_reg, func)) {
			gen_element_store(base.reg, idx_reg, value_reg, func);
			free_register(func, idx_reg);
		}
		free_register(func, value_reg);
		free_lvalue(base, func);
		return;
	}

	if (auto* member_expr = dynamic_cast<const MemberCallExpr*>(target)) {
		if (member_expr->is_method_call) {
			error_at("Cannot assign to method call", site);
		}

		LValue base = resolve_lvalue(member_expr->object.get(), func);

		// A trait-typed receiver exposes only declared trait variables. Apply the
		// declaration's coercion before the ordinary OBJECT/DICTIONARY store.
		const TraitDecl* member_trait = nullptr;
		const VarDeclStmt* trait_var = nullptr;
		if (auto known = func.register_traits.find(base.reg); known != func.register_traits.end()) {
			for (const TraitDecl* trait : known->second) {
				if ((trait_var = find_trait_var(*trait, member_expr->member_name))) {
					member_trait = trait;
					break;
				}
			}
		}
		if (trait_var == nullptr) {
			if (auto declared = func.declared_traits.find(base.reg); declared != func.declared_traits.end()) {
				for (const TraitDecl* trait : declared->second) {
					if ((trait_var = find_trait_var(*trait, member_expr->member_name))) {
						member_trait = trait;
						break;
					}
				}
			}
		}
		if (trait_var != nullptr) {
			if (const TraitDecl* value_trait = find_trait(trait_var->type_hint.sole_name())) {
				value_reg = require_trait_value(value_reg, *value_trait,
					"variable '" + trait_var->name + "' of trait '" + member_trait->name + "'",
					func, member_expr->line, member_expr->column, trait_var->type_hint.nullable);
			} else if (trait_var->type_hint.is_union()) {
				value_reg = coerce_to_declared_type(value_reg, type_set_from(trait_var->type_hint),
					func, "variable '" + trait_var->name + "' of trait '" + member_trait->name + "'",
					site, trait_var->type_hint.to_string());
			} else if (!trait_var->type_hint.empty()) {
				value_reg = coerce_to_declared_type(value_reg, single_type_from(trait_var->type_hint),
					func, "variable '" + trait_var->name + "' of trait '" + member_trait->name + "'", site);
			}
		} else if (func.trait_only_registers.count(base.reg)) {
			// A register can be trait-only with no trait left to name it (a match
			// binding, an unnarrowed nullable): the ordinary store handles it.
			if (const TraitDecl* trait = get_register_trait(func, base.reg)) {
				error_at("'" + trait->name + "' has no variable '" +
					member_expr->member_name + "'", site);
			}
		}

		// Struct field: coerce to declared type.
		if (const StructDecl* decl = get_register_struct(func, base.reg);
			decl != nullptr && !(find_struct_field(*decl, member_expr->member_name) == nullptr &&
				native_base(*decl) != nullptr))
		{
			const StructField& field = require_struct_field(*decl, member_expr->member_name,
				member_expr->line, member_expr->column);
			if (const TraitDecl* field_iface = find_trait(field.type_hint.sole_name())) {
				value_reg = require_trait_value(value_reg, *field_iface,
					"field '" + field.name + "' of struct '" + decl->name + "'", func,
					member_expr->line, member_expr->column, field.type_hint.nullable);
			} else if (const StructDecl* field_struct = find_struct(field.type_hint.single_name());
				field_struct != nullptr && !field_struct->is_class) {
				value_reg = require_struct_value(value_reg, *field_struct,
					"field '" + field.name + "' of struct '" + decl->name + "'", func,
					member_expr->line, member_expr->column);
			} else if (!field.type_hint.empty() && field_struct == nullptr) {
				if (field.type_hint.is_union()) {
					value_reg = coerce_to_declared_type(value_reg,
						type_set_from(field.type_hint, field.line, field.column), func,
						"field '" + field.name + "' of struct '" + decl->name + "'", site,
						field.type_hint.to_string());
				} else {
					value_reg = coerce_to_declared_type(value_reg,
						single_type_from(field.type_hint), func,
						"field '" + field.name + "' of struct '" + decl->name + "'", site);
				}
			}
		}

		const bool mutated_copy = gen_member_store(base.reg, member_expr->member_name, value_reg, func);
		free_register(func, value_reg);

		// Value type: write the mutated copy back.
		if (mutated_copy) {
			store_lvalue(base, base.reg, func, site);
		}

		free_lvalue(base, func);
		return;
	}

	error_at("Invalid assignment target type", site);
}

// Track origin of result for write-back. Each chain link evaluated once.
CodeGenerator::LValue CodeGenerator::resolve_lvalue(const Expr* expr, FunctionContext& func) {
	LValue lvalue;

	if (auto* var_expr = dynamic_cast<const VariableExpr*>(expr)) {
		if (Variable* var = find_variable(func, var_expr->name)) {
			// Own register: member store mutates in place.
			lvalue.kind = var->is_const ? LValue::Kind::VALUE : LValue::Kind::LOCAL;
			lvalue.reg = var->register_num;
			lvalue.name = var_expr->name;
			lvalue.borrowed = true;
			return lvalue;
		}
		if (is_global_variable(var_expr->name) && !is_global_const(var_expr->name)) {
			lvalue.kind = LValue::Kind::GLOBAL;
			lvalue.reg = gen_expr(expr, func);
			lvalue.name = var_expr->name;
			return lvalue;
		}
		VariableOrigin origin;
		lvalue.reg = gen_variable(var_expr, func, &origin);
		if (origin.container_reg >= 0) {
			auto container = std::make_shared<LValue>();
			container->reg = origin.container_reg;
			container->borrowed = origin.borrowed;
			lvalue.kind = LValue::Kind::MEMBER;
			lvalue.name = var_expr->name;
			lvalue.container = std::move(container);
		}
		return lvalue;
	}

	if (auto* member_expr = dynamic_cast<const MemberCallExpr*>(expr)) {
		if (!member_expr->is_method_call && member_expr->arguments.empty()) {
			if (auto* object = dynamic_cast<const VariableExpr*>(member_expr->object.get())) {
				if (names_a_chain_class(object->name, func)) {
					VariableExpr member(member_expr->member_name);
					member.line = member_expr->line;
					member.column = member_expr->column;
					return resolve_lvalue(&member, func);
				}
			}
			auto container = std::make_shared<LValue>(resolve_lvalue(member_expr->object.get(), func));
			lvalue.kind = LValue::Kind::MEMBER;
			lvalue.name = member_expr->member_name;
			lvalue.reg = gen_member_read(container->reg, member_expr->member_name, func);
			lvalue.container = std::move(container);
			return lvalue;
		}
	}

	if (auto* index_expr = dynamic_cast<const IndexExpr*>(expr)) {
		auto container = std::make_shared<LValue>(resolve_lvalue(index_expr->object.get(), func));
		check_struct_subscript(container->reg, index_expr->index.get(), func);
		lvalue.kind = LValue::Kind::INDEX;
		lvalue.index_expr = index_expr->index.get();
		if (!gen_constant_key_read(container->reg, lvalue.index_expr, func, lvalue.reg)) {
			lvalue.index_reg = gen_expr(index_expr->index.get(), func);
			lvalue.reg = gen_element_read(container->reg, lvalue.index_reg, func);
		}
		lvalue.container = std::move(container);
		return lvalue;
	}

	// Temporary with no write-back target (Object handles mutate through the handle).
	lvalue.reg = gen_expr(expr, func);
	return lvalue;
}

void CodeGenerator::store_lvalue(const LValue& target, int value_reg, FunctionContext& func,
	const Stmt* site)
{
	switch (target.kind) {
		case LValue::Kind::LOCAL:
			// Registers usually match; MOVE only when they diverge.
			if (target.reg != value_reg) {
				func.ir.instructions.emplace_back(IROpcode::MOVE,
					IRValue::reg(target.reg), IRValue::reg(value_reg));
			}
			return;

		case LValue::Kind::GLOBAL: {
			size_t global_idx = m_global_variables.at(target.name);
			if (m_global_structs[global_idx] != nullptr &&
				!m_global_structs[global_idx]->is_class) {
				value_reg = require_struct_value(value_reg, *m_global_structs[global_idx],
					"global '" + target.name + "'", func, site ? site->line : 0,
					site ? site->column : 0);
			}
			if (m_global_traits[global_idx] != nullptr) {
				value_reg = require_trait_value(value_reg, *m_global_traits[global_idx],
					"global '" + target.name + "'", func, site ? site->line : 0,
					site ? site->column : 0,
					m_global_sets[global_idx].contains(Variant::NIL));
			}
			if (!m_global_sets[global_idx].any()) {
				value_reg = coerce_to_declared_type(value_reg, m_global_sets[global_idx], func,
					"global '" + target.name + "'", site, m_global_type_names[global_idx]);
			} else {
				value_reg = coerce_to_declared_type(value_reg, m_global_types[global_idx], func,
					"global '" + target.name + "'", site);
			}
			if (!global_setter(global_idx).empty()) {
				gen_property_set(global_idx, value_reg, func);
				return;
			}
			if (get_register_type(func, value_reg) == Variant::OBJECT) {
				mark_global_holds_object(static_cast<int64_t>(global_idx));
			}
			func.ir.instructions.emplace_back(IROpcode::STORE_GLOBAL,
				IRValue::imm(global_idx), IRValue::reg(value_reg));
			return;
		}

		case LValue::Kind::MEMBER: {
			const bool mutated_copy = gen_member_store(target.container->reg, target.name, value_reg, func);
			if (mutated_copy) {
				store_lvalue(*target.container, target.container->reg, func, site);
			}
			return;
		}

		case LValue::Kind::INDEX:
			if (!gen_constant_key_store(target.container->reg, target.index_expr, value_reg, func)) {
				gen_element_store(target.container->reg, target.index_reg, value_reg, func);
			}
			return;

		case LValue::Kind::VALUE:
			// No write-back target.
			return;
	}
}

void CodeGenerator::free_lvalue(const LValue& lvalue, FunctionContext& func) {
	if (lvalue.index_reg >= 0) {
		free_register(func, lvalue.index_reg);
	}
	if (lvalue.reg >= 0 && !lvalue.borrowed) {
		free_register(func, lvalue.reg);
	}
	if (lvalue.container) {
		free_lvalue(*lvalue.container, func);
	}
}

void CodeGenerator::gen_return(const ReturnStmt* stmt, FunctionContext& func) {
	if (stmt->value) {
		int reg = gen_expr(stmt->value.get(), func);
		if (const StructDecl* returned = find_struct(func.return_type.single_name());
			returned != nullptr && !returned->is_class) {
			reg = require_struct_value(reg, *returned,
				"the return value of '" + m_current_function + "'", func,
				stmt->line, stmt->column);
		}
		// Coerce to declared return type before moving to r0.
		if (const TraitDecl* iface = find_trait(func.return_type.sole_name())) {
			reg = require_trait_value(reg, *iface,
				"the return value of '" + m_current_function + "'", func,
				stmt->line, stmt->column, func.return_type.nullable);
		} else if (func.return_type.is_union()) {
			reg = coerce_to_declared_type(reg, type_set_from(func.return_type), func,
				"the return value of '" + m_current_function + "'", stmt,
				func.return_type.to_string());
		} else {
			reg = coerce_to_declared_type(reg, single_type_from(func.return_type), func,
				"the return value of '" + m_current_function + "'", stmt);
		}
		if (reg != 0) {
			func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(0), IRValue::reg(reg));
		}
		free_register(func, reg);
	} else {
		if (func.return_type.is_union() && !type_set_from(func.return_type).contains(Variant::NIL)) {
			error_at("A bare return cannot satisfy return type " + func.return_type.to_string(), stmt);
		}
		// r0 aliases the first parameter; explicit null needed.
		func.ir.instructions.emplace_back(IROpcode::LOAD_NIL, IRValue::reg(0));
	}

	func.ir.instructions.emplace_back(IROpcode::RETURN);
}

void CodeGenerator::emit_conditional_branch(IROpcode opcode, int cond_reg,
	const std::string& label, FunctionContext& func)
{
	// Type travels with the branch for inline truthiness in the backend.
	IRInstruction branch(opcode, IRValue::reg(cond_reg), ir_label(label));
	branch.type_hint = get_register_type(func, cond_reg);
	func.ir.instructions.push_back(branch);
}

CodeGenerator::NarrowingInfo CodeGenerator::condition_narrowing(
	const Expr* condition, FunctionContext& func)
{
	if (auto* unary = dynamic_cast<const UnaryExpr*>(condition)) {
		if (unary->op == UnaryExpr::Op::NOT) {
			NarrowingInfo result = condition_narrowing(unary->operand.get(), func);
			std::swap(result.then_set, result.else_set);
			result.trait_then = !result.trait_then;
			return result;
		}
	}
	if (auto* test = dynamic_cast<const TypeTestExpr*>(condition);
		test != nullptr && find_trait(test->type.single_name()) != nullptr) {
		const VariableExpr* subject = dynamic_cast<const VariableExpr*>(test->value.get());
		const TraitDecl* iface = find_trait(test->type.single_name());
		if (subject == nullptr || iface == nullptr) return {};
		Variable* variable = find_variable(func, subject->name);
		if (variable == nullptr) return {};
		NarrowingInfo result;
		result.reg = variable->register_num;
		result.saved_type = get_register_type(func, result.reg);
		result.saved_struct = get_register_struct(func, result.reg);
		result.narrowed_trait = iface;
		if (auto it = func.register_traits.find(result.reg);
			it != func.register_traits.end()) result.saved_traits = it->second;
		result.saved_trait_only = func.trait_only_registers.count(result.reg) != 0;
		return result;
	}
	if (auto* logical = dynamic_cast<const BinaryExpr*>(condition)) {
		if (logical->op == BinaryExpr::Op::AND) {
			NarrowingInfo result = condition_narrowing(logical->left.get(), func);
			if (result.valid()) {
				// False may come from either operand, so only the true path narrows.
				result.else_set = result.original;
			}
			return result;
		}
	}

	const VariableExpr* subject = nullptr;
	TypeSet tested;
	bool inverse = false;
	bool truthiness = false;
	if (auto* type_test = dynamic_cast<const TypeTestExpr*>(condition)) {
		subject = dynamic_cast<const VariableExpr*>(type_test->value.get());
		if (subject != nullptr) {
			tested = type_set_from(type_test->type, type_test->line, type_test->column);
		}
	} else if (auto* comparison = dynamic_cast<const BinaryExpr*>(condition)) {
		if (comparison->op == BinaryExpr::Op::EQ || comparison->op == BinaryExpr::Op::NEQ) {
			auto is_null = [](const Expr* expr) {
				auto* literal = dynamic_cast<const LiteralExpr*>(expr);
				return literal != nullptr && literal->lit_type == LiteralExpr::Type::NULL_VAL;
			};
			if (is_null(comparison->left.get())) {
				subject = dynamic_cast<const VariableExpr*>(comparison->right.get());
			} else if (is_null(comparison->right.get())) {
				subject = dynamic_cast<const VariableExpr*>(comparison->left.get());
			}
			if (subject != nullptr) {
				tested.mask = uint64_t(1) << Variant::NIL;
				inverse = comparison->op == BinaryExpr::Op::NEQ;
			}
		}
	} else {
		subject = dynamic_cast<const VariableExpr*>(condition);
		if (subject != nullptr) {
			tested.mask = uint64_t(1) << Variant::NIL;
			inverse = true; // truthy means non-null
			truthiness = true;
		}
	}
	if (subject == nullptr || tested.any()) {
		return {};
	}

	Variable* variable = find_variable(func, subject->name);
	if (variable == nullptr) {
		if (!is_global_variable(subject->name)) {
			return {};
		}
		const size_t global_idx = m_global_variables.at(subject->name);
		if (global_idx >= m_global_sets.size() || m_global_sets[global_idx].any() ||
			global_idx >= m_global_is_member.size() || !m_global_is_member[global_idx] ||
			!global_getter(global_idx).empty()) {
			return {};
		}
		NarrowingInfo result;
		result.global_idx = global_idx;
		result.original = m_global_sets[global_idx];
		result.then_set = { result.original.mask & tested.mask };
		result.else_set = { result.original.mask & ~tested.mask };
		if (inverse) {
			std::swap(result.then_set, result.else_set);
		}
		if (truthiness) {
			result.else_set = result.original;
		}
		if (auto saved = func.narrowed_global_types.find(global_idx);
			saved != func.narrowed_global_types.end()) {
			result.had_saved_global = true;
			result.saved_global = saved->second;
		}
		return result;
	}
	const auto declared = func.declared_sets.find(variable->register_num);
	if (declared == func.declared_sets.end()) {
		return {};
	}
	NarrowingInfo result;
	result.reg = variable->register_num;
	result.original = declared->second;
	result.saved_type = get_register_type(func, result.reg);
	result.saved_struct = get_register_struct(func, result.reg);
	if (auto saved = func.register_traits.find(result.reg);
		saved != func.register_traits.end()) result.saved_traits = saved->second;
	result.saved_trait_only = func.trait_only_registers.count(result.reg) != 0;
	result.then_set = { result.original.mask & tested.mask };
	result.else_set = { result.original.mask & ~tested.mask };
	if (inverse) {
		std::swap(result.then_set, result.else_set);
	}
	if (truthiness) {
		// A falsy value need not be NIL (0, empty String, Vector2.ZERO, ...).
		result.else_set = result.original;
	}
	return result;
}

void CodeGenerator::apply_narrowing(const NarrowingInfo& narrowing, bool then_branch,
	FunctionContext& func)
{
	if (!narrowing.valid()) {
		return;
	}
	if (narrowing.narrowed_trait != nullptr) {
		if (then_branch == narrowing.trait_then) {
			add_register_trait(func, narrowing.reg, narrowing.narrowed_trait);
		}
		return;
	}
	const TypeSet set = then_branch ? narrowing.then_set : narrowing.else_set;
	if (narrowing.is_member()) {
		if (set.single()) {
			func.narrowed_global_types[narrowing.global_idx] =
				static_cast<IRInstruction::TypeHint>(set.only());
		} else {
			func.narrowed_global_types.erase(narrowing.global_idx);
		}
		return;
	}
	set_register_type(func, narrowing.reg, set.single()
		? static_cast<IRInstruction::TypeHint>(set.only())
		: IRInstruction::TypeHint_NONE);
	if (set.single() && set.only() == Variant::DICTIONARY) {
		const auto declared = func.declared_structs.find(narrowing.reg);
		if (declared != func.declared_structs.end()) {
			func.register_structs[narrowing.reg] = declared->second;
		}
	} else {
		func.register_structs.erase(narrowing.reg);
	}
	if (!set.contains(Variant::NIL)) {
		if (auto declared = func.declared_traits.find(narrowing.reg);
			declared != func.declared_traits.end()) {
			for (const TraitDecl* iface : declared->second) {
				add_register_trait(func, narrowing.reg, iface);
			}
			func.trait_only_registers.insert(narrowing.reg);
		}
	} else if (func.declared_traits.count(narrowing.reg) != 0) {
		func.register_traits.erase(narrowing.reg);
	}
}

void CodeGenerator::restore_narrowing(const NarrowingInfo& narrowing,
	FunctionContext& func)
{
	if (!narrowing.valid()) {
		return;
	}
	if (narrowing.narrowed_trait != nullptr) {
		if (narrowing.saved_traits.empty()) {
			func.register_traits.erase(narrowing.reg);
		} else {
			func.register_traits[narrowing.reg] = narrowing.saved_traits;
		}
		return;
	}
	if (narrowing.is_member()) {
		if (narrowing.had_saved_global) {
			func.narrowed_global_types[narrowing.global_idx] = narrowing.saved_global;
		} else {
			func.narrowed_global_types.erase(narrowing.global_idx);
		}
		return;
	}
	set_register_type(func, narrowing.reg, narrowing.saved_type);
	if (narrowing.saved_struct != nullptr) {
		func.register_structs[narrowing.reg] = narrowing.saved_struct;
	} else {
		func.register_structs.erase(narrowing.reg);
	}
	if (narrowing.saved_traits.empty()) {
		func.register_traits.erase(narrowing.reg);
	} else {
		func.register_traits[narrowing.reg] = narrowing.saved_traits;
	}
	// Restore what the register had: 'as T', a match binding and a typed
	// container element make a register trait-only with no declaration.
	if (narrowing.saved_trait_only) {
		func.trait_only_registers.insert(narrowing.reg);
	} else {
		func.trait_only_registers.erase(narrowing.reg);
	}
}

bool CodeGenerator::branch_returns(const std::vector<StmtPtr>& body) {
	if (body.empty()) {
		return false;
	}
	const Stmt* last = body.back().get();
	return dynamic_cast<const ReturnStmt*>(last) != nullptr ||
		dynamic_cast<const BreakStmt*>(last) != nullptr ||
		dynamic_cast<const ContinueStmt*>(last) != nullptr;
}

namespace {

bool narrowing_expr_calls_out(const Expr* expr) {
	if (expr == nullptr) return false;
	if (dynamic_cast<const CallExpr*>(expr) != nullptr ||
		dynamic_cast<const AwaitExpr*>(expr) != nullptr ||
		dynamic_cast<const LambdaExpr*>(expr) != nullptr) {
		return true;
	}
	if (auto* member = dynamic_cast<const MemberCallExpr*>(expr)) {
		if (member->is_method_call || narrowing_expr_calls_out(member->object.get())) return true;
		for (const auto& argument : member->arguments) {
			if (narrowing_expr_calls_out(argument.get())) return true;
		}
		return false;
	}
	if (auto* binary = dynamic_cast<const BinaryExpr*>(expr)) {
		return narrowing_expr_calls_out(binary->left.get()) ||
			narrowing_expr_calls_out(binary->right.get());
	}
	if (auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
		return narrowing_expr_calls_out(unary->operand.get());
	}
	if (auto* test = dynamic_cast<const TypeTestExpr*>(expr)) {
		return narrowing_expr_calls_out(test->value.get());
	}
	if (auto* cast = dynamic_cast<const CastExpr*>(expr)) {
		return narrowing_expr_calls_out(cast->value.get());
	}
	if (auto* ternary = dynamic_cast<const TernaryExpr*>(expr)) {
		return narrowing_expr_calls_out(ternary->condition.get()) ||
			narrowing_expr_calls_out(ternary->true_value.get()) ||
			narrowing_expr_calls_out(ternary->false_value.get());
	}
	if (auto* index = dynamic_cast<const IndexExpr*>(expr)) {
		return narrowing_expr_calls_out(index->object.get()) ||
			narrowing_expr_calls_out(index->index.get());
	}
	if (auto* array = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
		for (const auto& element : array->elements) {
			if (narrowing_expr_calls_out(element.get())) return true;
		}
	}
	if (auto* dictionary = dynamic_cast<const DictionaryLiteralExpr*>(expr)) {
		for (const auto& [key, value] : dictionary->elements) {
			if (narrowing_expr_calls_out(key.get()) || narrowing_expr_calls_out(value.get())) return true;
		}
	}
	return false;
}

bool narrowing_expr_names(const Expr* expr, const std::string& name) {
	if (expr == nullptr) return false;
	if (auto* variable = dynamic_cast<const VariableExpr*>(expr)) return variable->name == name;
	if (auto* call = dynamic_cast<const CallExpr*>(expr)) {
		for (const auto& argument : call->arguments) {
			if (narrowing_expr_names(argument.get(), name)) return true;
		}
		return false;
	}
	if (auto* member = dynamic_cast<const MemberCallExpr*>(expr)) {
		if (narrowing_expr_names(member->object.get(), name)) return true;
		for (const auto& argument : member->arguments) {
			if (narrowing_expr_names(argument.get(), name)) return true;
		}
		return false;
	}
	if (auto* binary = dynamic_cast<const BinaryExpr*>(expr)) {
		return narrowing_expr_names(binary->left.get(), name) ||
			narrowing_expr_names(binary->right.get(), name);
	}
	if (auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
		return narrowing_expr_names(unary->operand.get(), name);
	}
	if (auto* test = dynamic_cast<const TypeTestExpr*>(expr)) {
		return narrowing_expr_names(test->value.get(), name);
	}
	if (auto* cast = dynamic_cast<const CastExpr*>(expr)) {
		return narrowing_expr_names(cast->value.get(), name);
	}
	if (auto* await = dynamic_cast<const AwaitExpr*>(expr)) {
		return narrowing_expr_names(await->operand.get(), name);
	}
	if (auto* ternary = dynamic_cast<const TernaryExpr*>(expr)) {
		return narrowing_expr_names(ternary->condition.get(), name) ||
			narrowing_expr_names(ternary->true_value.get(), name) ||
			narrowing_expr_names(ternary->false_value.get(), name);
	}
	if (auto* index = dynamic_cast<const IndexExpr*>(expr)) {
		return narrowing_expr_names(index->object.get(), name) ||
			narrowing_expr_names(index->index.get(), name);
	}
	if (auto* array = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
		for (const auto& element : array->elements) {
			if (narrowing_expr_names(element.get(), name)) return true;
		}
	}
	if (auto* dictionary = dynamic_cast<const DictionaryLiteralExpr*>(expr)) {
		for (const auto& [key, value] : dictionary->elements) {
			if (narrowing_expr_names(key.get(), name) || narrowing_expr_names(value.get(), name)) return true;
		}
	}
	return false;
}

bool narrowing_body_is_safe(const std::vector<StmtPtr>& body, const std::string& member_name);

bool narrowing_pattern_calls_out(const MatchPattern& pattern) {
	if (narrowing_expr_calls_out(pattern.value.get())) return true;
	for (const auto& element : pattern.elements) {
		if (narrowing_pattern_calls_out(*element)) return true;
	}
	for (const auto& entry : pattern.entries) {
		if (narrowing_expr_calls_out(entry.key.get()) ||
			(entry.value && narrowing_pattern_calls_out(*entry.value))) return true;
	}
	return false;
}

bool narrowing_stmt_is_safe(const Stmt* stmt, const std::string& member_name) {
	if (auto* expression = dynamic_cast<const ExprStmt*>(stmt)) {
		return !narrowing_expr_calls_out(expression->expression.get());
	}
	if (auto* declaration = dynamic_cast<const VarDeclStmt*>(stmt)) {
		return !narrowing_expr_calls_out(declaration->initializer.get());
	}
	if (auto* assignment = dynamic_cast<const AssignStmt*>(stmt)) {
		const bool writes_member = assignment->name == member_name ||
			narrowing_expr_names(assignment->target.get(), member_name);
		return !writes_member && !narrowing_expr_calls_out(assignment->value.get()) &&
			!narrowing_expr_calls_out(assignment->target.get());
	}
	if (auto* returned = dynamic_cast<const ReturnStmt*>(stmt)) {
		return !narrowing_expr_calls_out(returned->value.get());
	}
	if (auto* branch = dynamic_cast<const IfStmt*>(stmt)) {
		const Expr* condition = branch->binding
			? branch->binding->initializer.get() : branch->condition.get();
		return !narrowing_expr_calls_out(condition) &&
			narrowing_body_is_safe(branch->then_branch, member_name) &&
			narrowing_body_is_safe(branch->else_branch, member_name);
	}
	if (auto* loop = dynamic_cast<const WhileStmt*>(stmt)) {
		return !narrowing_expr_calls_out(loop->condition.get()) &&
			narrowing_body_is_safe(loop->body, member_name);
	}
	if (auto* loop = dynamic_cast<const ForStmt*>(stmt)) {
		return !narrowing_expr_calls_out(loop->iterable.get()) &&
			narrowing_body_is_safe(loop->body, member_name);
	}
	if (auto* match = dynamic_cast<const MatchStmt*>(stmt)) {
		if (narrowing_expr_calls_out(match->subject.get())) return false;
		for (const auto& branch : match->branches) {
			if (narrowing_expr_calls_out(branch.guard.get()) ||
				!narrowing_body_is_safe(branch.body, member_name)) return false;
			for (const auto& pattern : branch.patterns) {
				if (narrowing_pattern_calls_out(*pattern)) return false;
			}
		}
	}
	return true;
}

bool narrowing_body_is_safe(const std::vector<StmtPtr>& body, const std::string& member_name) {
	for (const auto& statement : body) {
		if (!narrowing_stmt_is_safe(statement.get(), member_name)) return false;
	}
	return true;
}

} // namespace

void CodeGenerator::gen_if(const IfStmt* stmt, FunctionContext& func) {
	if (stmt->binding) {
		gen_if_binding(stmt, func);
		return;
	}

	NarrowingInfo narrowing = condition_narrowing(stmt->condition.get(), func);
	std::string narrowed_member;
	if (narrowing.is_member()) {
		for (const auto& [name, index] : m_global_variables) {
			if (index == narrowing.global_idx) {
				narrowed_member = name;
				break;
			}
		}
	}
	const bool condition_safe = !narrowing.is_member() ||
		!narrowing_expr_calls_out(stmt->condition.get());
	const bool then_safe = !narrowing.is_member() || (condition_safe &&
		narrowing_body_is_safe(stmt->then_branch, narrowed_member));
	const bool else_safe = !narrowing.is_member() || (condition_safe &&
		narrowing_body_is_safe(stmt->else_branch, narrowed_member));
	std::string else_label = make_label("else");
	std::string end_label = make_label("endif");
	int cond_reg = gen_expr(stmt->condition.get(), func);
	if (!stmt->else_branch.empty()) {
		emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, else_label, func);
	} else {
		emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, end_label, func);
	}

	free_register(func, cond_reg);

	const int then_scope = push_block_scope(func);
	if (then_safe) apply_narrowing(narrowing, true, func);
	for (const auto& s : stmt->then_branch) {
		gen_stmt(s.get(), func);
	}
	pop_block_scope(then_scope, func);
	if (then_safe) restore_narrowing(narrowing, func);

	if (!stmt->else_branch.empty()) {
		func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(end_label));

		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(else_label));
		const int else_scope = push_block_scope(func);
		if (else_safe) apply_narrowing(narrowing, false, func);
		for (const auto& s : stmt->else_branch) {
			gen_stmt(s.get(), func);
		}
		pop_block_scope(else_scope, func);
		if (else_safe) restore_narrowing(narrowing, func);
	}

	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end_label));
	if (narrowing.valid() && !narrowing.is_member() && stmt->else_branch.empty() &&
		branch_returns(stmt->then_branch)) {
		apply_narrowing(narrowing, false, func);
	}
}

void CodeGenerator::gen_if_binding(const IfStmt* stmt, FunctionContext& func) {
	const std::string null_label = make_label("if_var_null");
	const std::string end_label = make_label("endif");

	// The binding owns the initializer and is the then branch's lexical scope.
	// Opening its run-time scope before evaluation also releases temporary
	// Variants on both the success and NIL paths.
	const int binding_scope = open_scope(func);
	push_scope(func);
	gen_var_decl(stmt->binding.get(), func, true);

	Variable* binding = find_variable(func, stmt->binding->name);
	if (binding == nullptr) {
		error_at("Failed to declare 'if var' binding '" + stmt->binding->name + "'", stmt);
	}
	const int value_reg = binding->register_num;

	// This is deliberately a NIL tag test rather than a truthiness branch.  Zero,
	// false, empty strings and zero-valued vectors all enter the body.
	const int is_nil_reg = alloc_register(func);
	IRInstruction is_nil(IROpcode::TYPE_TEST, IRValue::reg(is_nil_reg),
		IRValue::reg(value_reg), IRValue::imm(static_cast<int64_t>(Variant::NIL)));
	is_nil.type_hint = Variant::BOOL;
	func.ir.instructions.push_back(is_nil);
	set_register_type(func, is_nil_reg, Variant::BOOL);
	emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, is_nil_reg, null_label, func);
	free_register(func, is_nil_reg);

	// A typed nullable binding is plain T in the successful branch.  Reuse the
	// same narrowing machinery as `value != null` so structs and traits retain
	// their compiler-only shape information too.
	NarrowingInfo narrowing;
	if (auto declared = func.declared_sets.find(value_reg);
		declared != func.declared_sets.end()) {
		narrowing.reg = value_reg;
		narrowing.original = declared->second;
		narrowing.then_set = declared->second.non_null();
		narrowing.else_set = declared->second.intersect(
			TypeSet{uint64_t(1) << Variant::NIL});
		narrowing.saved_type = get_register_type(func, value_reg);
		narrowing.saved_struct = get_register_struct(func, value_reg);
		if (auto traits = func.register_traits.find(value_reg);
			traits != func.register_traits.end()) {
			narrowing.saved_traits = traits->second;
		}
		narrowing.saved_trait_only = func.trait_only_registers.count(value_reg) != 0;
		apply_narrowing(narrowing, true, func);
	}

	for (const auto& body_stmt : stmt->then_branch) {
		gen_stmt(body_stmt.get(), func);
	}
	pop_scope(func);
	emit_scope_release(binding_scope, func);
	func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(end_label));

	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(null_label));
	emit_scope_release(binding_scope, func);
	if (!stmt->else_branch.empty()) {
		const int else_scope = push_block_scope(func);
		for (const auto& body_stmt : stmt->else_branch) {
			gen_stmt(body_stmt.get(), func);
		}
		pop_block_scope(else_scope, func);
	}

	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end_label));
	free_register(func, value_reg);
}

static constexpr size_t MIN_SWITCH_CASES = 4;
static constexpr size_t MAX_SWITCH_SPREAD = 3;
static constexpr int64_t MAX_SWITCH_ENTRIES = 4096;

// Pattern kind noun for diagnostics.
static const char* pattern_kind_noun(MatchPattern::Kind kind) {
	switch (kind) {
		case MatchPattern::Kind::WILDCARD:    return "a wildcard";
		case MatchPattern::Kind::BIND:        return "a binding";
		case MatchPattern::Kind::ARRAY:       return "an array pattern";
		case MatchPattern::Kind::DICTIONARY:  return "a dictionary pattern";
		case MatchPattern::Kind::STRUCT:      return "a struct pattern";
		case MatchPattern::Kind::VALUE:       return "a value pattern";
	}
	return "a pattern";
}

bool CodeGenerator::gen_match_jump_table(const MatchStmt* stmt, int subject_reg,
                                         const std::vector<std::string>& body_labels,
                                         const std::string& default_label,
                                         FunctionContext& func,
                                         JumpTableReject* reject) {
	auto decline = [&](const std::string& reason, const std::string& hint,
	                   int line, int column) {
		if (reject != nullptr) {
			*reject = JumpTableReject{reason, hint, line, column};
		}
		return false;
	};

	// Every pattern must be an integer constant; non-integer patterns disqualify the table.
	struct Case {
		int64_t value;
		size_t branch;
		int line;
		int column;
	};
	std::vector<Case> cases;
	for (size_t i = 0; i < stmt->branches.size(); i++) {
		const auto& branch = stmt->branches[i];
		if (branch.is_catch_all()) {
			continue;
		}
		// Guards disqualify: an entry jumping to the body would skip the guard.
		if (branch.guard) {
			return decline("A 'switch' arm cannot have a 'when' guard",
				"A guard has to run before the arm is taken, which a jump into the "
				"body skips. Use 'match' when arms need guards.",
				branch.guard->line, branch.guard->column);
		}
		for (const auto& pattern : branch.patterns) {
			if (pattern->kind != MatchPattern::Kind::VALUE) {
				const bool lone_wildcard = pattern->kind == MatchPattern::Kind::WILDCARD;
				return decline(
					std::string("A 'switch' pattern has to be an integer constant, but this is ")
						+ pattern_kind_noun(pattern->kind),
					lone_wildcard
						? "'_' is the default arm of a 'switch' and cannot share an arm "
						  "with other patterns."
						: "Use 'match' for patterns that have to be tested at run time.",
					pattern->line, pattern->column);
			}
			IRGlobalVar folded;
			if (!fold_global_initializer(pattern->value.get(), folded, &func)) {
				return decline("A 'switch' pattern has to be an integer constant the compiler can fold",
					"Only literals, 'const' values and enum members can index a jump table. "
					"Use 'match' to compare against a run-time value.",
					pattern->line, pattern->column);
			}
			if (folded.init_type != IRGlobalVar::InitType::INT) {
				return decline("A 'switch' pattern has to be an integer constant",
					"A jump table is indexed by an integer. Use 'match' to compare "
					"against a value of another type.",
					pattern->line, pattern->column);
			}
			cases.push_back(Case{std::get<int64_t>(folded.init_value), i,
				pattern->line, pattern->column});
		}
	}
	// match: MIN_SWITCH_CASES floor applies. switch: any count above zero.
	if (cases.empty()) {
		return decline("A 'switch' needs at least one integer pattern",
			"An arm list of nothing but '_' is an 'if' with extra steps.",
			stmt->line, stmt->column);
	}
	if (!stmt->is_switch && cases.size() < MIN_SWITCH_CASES) {
		return false;
	}

	std::map<int64_t, size_t> first_branch;
	for (const auto& entry : cases) {
		// match keeps the first arm silently; switch rejects duplicates.
		if (!first_branch.emplace(entry.value, entry.branch).second && stmt->is_switch) {
			return decline("Duplicate 'switch' pattern " + std::to_string(entry.value),
				"An earlier arm already covers this value, so this one can never run.",
				entry.line, entry.column);
		}
	}

	const int64_t low = first_branch.begin()->first;
	const int64_t high = first_branch.rbegin()->first;
	const uint64_t span = static_cast<uint64_t>(high) - static_cast<uint64_t>(low) + 1;
	if (span > static_cast<uint64_t>(MAX_SWITCH_ENTRIES) ||
	    span > first_branch.size() * MAX_SWITCH_SPREAD) {
		return decline("The patterns of this 'switch' are too sparse for a jump table: "
			+ std::to_string(first_branch.size()) + " arms spanning "
			+ std::to_string(span) + " entries, from " + std::to_string(low)
			+ " to " + std::to_string(high),
			"A jump table holds one entry per value in the range, so it is bounded "
			"at " + std::to_string(MAX_SWITCH_ENTRIES) + " entries and "
			+ std::to_string(MAX_SWITCH_SPREAD) + "x the arm count. Use 'match' "
			"for a sparse set of values.",
			stmt->line, stmt->column);
	}

	IRInstruction table(IROpcode::SWITCH, IRValue::reg(subject_reg), IRValue::imm(low),
	                    IRValue::imm(static_cast<int64_t>(span)));
	if (get_register_type(func, subject_reg) == Variant::INT) {
		table.type_hint = Variant::INT;
	}
	for (uint64_t i = 0; i < span; i++) {
		auto it = first_branch.find(low + static_cast<int64_t>(i));
		table.operands.push_back(ir_label(
			it == first_branch.end() ? default_label : body_labels[it->second]));
	}
	func.ir.instructions.push_back(table);
	return true;
}

void CodeGenerator::gen_match(const MatchStmt* stmt, FunctionContext& func) {
	// Jump table for dense integer patterns, then a compare chain for the rest.
	// The table falls through for non-integer or out-of-range subjects.
	const std::string end_label = make_label("endmatch");

	const int subject_reg = gen_expr(stmt->subject.get(), func);
	int narrowed_reg = -1;
	size_t narrowed_global_idx = SIZE_MAX;
	std::string narrowed_global_name;
	TypeSet narrowed_declared;
	IRInstruction::TypeHint narrowed_saved = IRInstruction::TypeHint_NONE;
	const StructDecl* narrowed_saved_struct = nullptr;
	bool narrowed_global_had_saved = false;
	IRInstruction::TypeHint narrowed_global_saved = IRInstruction::TypeHint_NONE;
	bool subject_is_typeof = false;
	const VariableExpr* narrowed_subject = dynamic_cast<const VariableExpr*>(stmt->subject.get());
	if (auto* call = dynamic_cast<const CallExpr*>(stmt->subject.get());
		call != nullptr && call->function_name == "typeof" && call->arguments.size() == 1) {
		narrowed_subject = dynamic_cast<const VariableExpr*>(call->arguments[0].get());
		subject_is_typeof = narrowed_subject != nullptr;
	}
	if (narrowed_subject != nullptr) {
		if (Variable* variable = find_variable(func, narrowed_subject->name)) {
			const auto declared = func.declared_sets.find(variable->register_num);
			if (declared != func.declared_sets.end()) {
				narrowed_reg = variable->register_num;
				narrowed_declared = declared->second;
				narrowed_saved = get_register_type(func, narrowed_reg);
				narrowed_saved_struct = get_register_struct(func, narrowed_reg);
			}
		} else if (is_global_variable(narrowed_subject->name)) {
			const size_t global_idx = m_global_variables.at(narrowed_subject->name);
			if (global_idx < m_global_sets.size() && !m_global_sets[global_idx].any() &&
				global_idx < m_global_is_member.size() && m_global_is_member[global_idx] &&
				global_getter(global_idx).empty()) {
				narrowed_global_idx = global_idx;
				narrowed_global_name = narrowed_subject->name;
				narrowed_declared = m_global_sets[global_idx];
				if (auto saved = func.narrowed_global_types.find(global_idx);
					saved != func.narrowed_global_types.end()) {
					narrowed_global_had_saved = true;
					narrowed_global_saved = saved->second;
				}
			}
		}
	}
	auto arm_narrowing = [&](const MatchStmt::Branch& branch) {
		TypeSet accepted;
		for (const auto& pattern : branch.patterns) {
			if (pattern->kind != MatchPattern::Kind::VALUE) {
				return TypeSet{};
			}
			Variant::Type type = Variant::VARIANT_MAX;
			if (subject_is_typeof) {
				IRGlobalVar folded;
				if (fold_global_initializer(pattern->value.get(), folded, &func) &&
					folded.init_type == IRGlobalVar::InitType::INT) {
					const int64_t tag = std::get<int64_t>(folded.init_value);
					if (tag >= 0 && tag < Variant::VARIANT_MAX) {
						type = static_cast<Variant::Type>(tag);
					}
				}
				if (type == Variant::VARIANT_MAX) {
					if (auto* name = dynamic_cast<const VariableExpr*>(pattern->value.get())) {
						if (const GlobalConstant* constant = find_global_constant(name->name);
							constant != nullptr && !constant->is_float &&
							constant->int_value >= 0 && constant->int_value < Variant::VARIANT_MAX) {
							type = static_cast<Variant::Type>(constant->int_value);
						}
					}
				}
			} else if (auto* literal = dynamic_cast<const LiteralExpr*>(pattern->value.get())) {
				switch (literal->lit_type) {
					case LiteralExpr::Type::INTEGER: type = Variant::INT; break;
					case LiteralExpr::Type::FLOAT: type = Variant::FLOAT; break;
					case LiteralExpr::Type::STRING:
						type = literal->string_type == LiteralExpr::StringType::STRING_NAME
							? Variant::STRING_NAME : literal->string_type == LiteralExpr::StringType::NODE_PATH
							? Variant::NODE_PATH : Variant::STRING;
						break;
					case LiteralExpr::Type::BOOL: type = Variant::BOOL; break;
					case LiteralExpr::Type::NULL_VAL: type = Variant::NIL; break;
				}
			}
			if (type == Variant::VARIANT_MAX) {
				return TypeSet{};
			}
			accepted.mask |= uint64_t(1) << type;
		}
		return TypeSet{accepted.mask & narrowed_declared.mask};
	};

	std::vector<std::string> test_labels;
	std::vector<std::string> body_labels;
	test_labels.reserve(stmt->branches.size());
	body_labels.reserve(stmt->branches.size());
	for (size_t i = 0; i < stmt->branches.size(); i++) {
		test_labels.push_back(make_label("match_test"));
		body_labels.push_back(make_label("match_body"));
	}

	size_t catch_all = stmt->branches.size();
	for (size_t i = 0; i < stmt->branches.size(); i++) {
		if (stmt->branches[i].is_catch_all()) {
			catch_all = i;
			break;
		}
	}
	const std::string& default_label =
		catch_all < stmt->branches.size() ? body_labels[catch_all] : end_label;

	// switch requires a compile-time int subject; a type test defeats O(1) dispatch.
	if (stmt->is_switch && get_register_type(func, subject_reg) != Variant::INT) {
		const IRInstruction::TypeHint actual = get_register_type(func, subject_reg);
		error_at(std::string("The subject of a 'switch' has to be a known integer, but this is ")
				+ (actual == IRInstruction::TypeHint_NONE
					? "of no type the compiler can determine"
					: std::string("a ") + variant_type_name(actual)),
			stmt->subject.get(),
			"A jump table is entered on the integer itself. Add a type hint that "
			"makes the subject an 'int', or use 'match', which tests the type at "
			"run time.");
	}

	JumpTableReject reject;
	const bool has_table = gen_match_jump_table(stmt, subject_reg, body_labels,
		default_label, func, stmt->is_switch ? &reject : nullptr);
	if (stmt->is_switch && !has_table) {
		error_at(reject.reason, reject.line, reject.column, reject.hint);
	}
	// Known INT subject: the table decides the whole match.
	const bool table_is_complete = has_table && get_register_type(func, subject_reg) == Variant::INT;
	if (table_is_complete) {
		func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(default_label));
	}

	TypeSet remaining_narrowed = narrowed_declared;
	for (size_t i = 0; i < stmt->branches.size(); i++) {
		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(test_labels[i]));

		const int arm_scope = push_block_scope(func);
		if (!table_is_complete) {
			const std::string& next_label =
				i + 1 < stmt->branches.size() ? test_labels[i + 1] : end_label;
			gen_branch_test(stmt->branches[i], subject_reg, next_label, func);
		}

		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(body_labels[i]));
		if (narrowed_reg >= 0 || narrowed_global_idx != SIZE_MAX) {
			TypeSet accepted = arm_narrowing(stmt->branches[i]);
			if (stmt->branches[i].is_catch_all()) {
				accepted = remaining_narrowed;
			} else if (!accepted.any()) {
				remaining_narrowed.mask &= ~accepted.mask;
			}
			if (narrowed_reg >= 0) {
				set_register_type(func, narrowed_reg, accepted.single()
					? static_cast<IRInstruction::TypeHint>(accepted.only())
					: IRInstruction::TypeHint_NONE);
				if (accepted.single() && accepted.only() == Variant::DICTIONARY) {
					const auto structure = func.declared_structs.find(narrowed_reg);
					if (structure != func.declared_structs.end()) {
						func.register_structs[narrowed_reg] = structure->second;
					}
				} else {
					func.register_structs.erase(narrowed_reg);
				}
			} else {
				bool safe = !narrowing_expr_calls_out(stmt->branches[i].guard.get()) &&
					narrowing_body_is_safe(stmt->branches[i].body, narrowed_global_name);
				for (const auto& pattern : stmt->branches[i].patterns) {
					safe = safe && !narrowing_pattern_calls_out(*pattern);
				}
				if (safe && accepted.single()) {
					func.narrowed_global_types[narrowed_global_idx] =
						static_cast<IRInstruction::TypeHint>(accepted.only());
				} else {
					func.narrowed_global_types.erase(narrowed_global_idx);
				}
			}
		}
		for (const auto& body_stmt : stmt->branches[i].body) {
			gen_stmt(body_stmt.get(), func);
		}
		if (narrowed_reg >= 0) {
			set_register_type(func, narrowed_reg, narrowed_saved);
			if (narrowed_saved_struct != nullptr) {
				func.register_structs[narrowed_reg] = narrowed_saved_struct;
			} else {
				func.register_structs.erase(narrowed_reg);
			}
		} else if (narrowed_global_idx != SIZE_MAX) {
			if (narrowed_global_had_saved) {
				func.narrowed_global_types[narrowed_global_idx] = narrowed_global_saved;
			} else {
				func.narrowed_global_types.erase(narrowed_global_idx);
			}
		}
		pop_block_scope(arm_scope, func);
		func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(end_label));
	}

	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end_label));
	free_register(func, subject_reg);
}

void CodeGenerator::gen_branch_test(const MatchStmt::Branch& branch, int subject_reg,
                                    const std::string& fail_label, FunctionContext& func) {
	if (branch.patterns.size() == 1) {
		gen_pattern_test(*branch.patterns[0], subject_reg, fail_label, func);
	} else {
		const std::string matched_label = make_label("match_any");
		for (size_t i = 0; i < branch.patterns.size(); i++) {
			const bool last = i + 1 == branch.patterns.size();
			if (last) {
				gen_pattern_test(*branch.patterns[i], subject_reg, fail_label, func);
				break;
			}
			const std::string next_pattern = make_label("match_or");
			gen_pattern_test(*branch.patterns[i], subject_reg, next_pattern, func);
			func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(matched_label));
			func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(next_pattern));
		}
		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(matched_label));
	}

	if (branch.guard) {
		int guard_reg = gen_expr(branch.guard.get(), func);
		emit_conditional_branch(IROpcode::BRANCH_ZERO, guard_reg, fail_label, func);
		free_register(func, guard_reg);
	}
}

void CodeGenerator::gen_pattern_test(const MatchPattern& pattern, int subject_reg,
                                     const std::string& fail_label, FunctionContext& func) {
	switch (pattern.kind) {
		case MatchPattern::Kind::WILDCARD:
			return;

		case MatchPattern::Kind::BIND: {
			// Own register: assignment in the body must not write back into the subject.
			int bound_reg = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(bound_reg),
				IRValue::reg(subject_reg));
			set_register_type(func, bound_reg, get_register_type(func, subject_reg));
			if (const StructDecl* structure = get_register_struct(func, subject_reg)) {
				set_register_struct(func, bound_reg, structure);
			}
			if (auto it = func.array_element_structs.find(subject_reg);
				it != func.array_element_structs.end()) {
				func.array_element_structs[bound_reg] = it->second;
			}
			if (auto it = func.dictionary_value_structs.find(subject_reg);
				it != func.dictionary_value_structs.end()) {
				func.dictionary_value_structs[bound_reg] = it->second;
			}
			if (auto it = func.register_traits.find(subject_reg);
				it != func.register_traits.end()) func.register_traits[bound_reg] = it->second;
			if (auto it = func.declared_traits.find(subject_reg);
				it != func.declared_traits.end()) func.declared_traits[bound_reg] = it->second;
			if (func.trait_only_registers.count(subject_reg)) {
				func.trait_only_registers.insert(bound_reg);
			}
			if (auto it = func.array_element_traits.find(subject_reg);
				it != func.array_element_traits.end()) {
				func.array_element_traits[bound_reg] = it->second;
			}
			if (auto it = func.dictionary_value_traits.find(subject_reg);
				it != func.dictionary_value_traits.end()) {
				func.dictionary_value_traits[bound_reg] = it->second;
			}
			declare_variable(func, pattern.name, bound_reg);
			return;
		}

		case MatchPattern::Kind::VALUE: {
			int pattern_reg = gen_expr(pattern.value.get(), func);
			IRInstruction cmp(IROpcode::CMP_EQ, IRValue::reg(pattern_reg),
			                  IRValue::reg(subject_reg), IRValue::reg(pattern_reg));
			cmp.type_hint = fused_compare_type(get_register_type(func, subject_reg),
			                                   get_register_type(func, pattern_reg));
			func.ir.instructions.push_back(cmp);
			set_register_type(func, pattern_reg, Variant::BOOL);
			emit_conditional_branch(IROpcode::BRANCH_ZERO, pattern_reg, fail_label, func);
			free_register(func, pattern_reg);
			return;
		}

		case MatchPattern::Kind::ARRAY:
			gen_array_pattern_test(pattern, subject_reg, fail_label, func);
			return;

		case MatchPattern::Kind::DICTIONARY:
			gen_dictionary_pattern_test(pattern, subject_reg, fail_label, func);
			return;

		case MatchPattern::Kind::STRUCT:
			gen_struct_pattern_test(pattern, subject_reg, fail_label, func);
			return;
	}
}

void CodeGenerator::gen_struct_pattern_test(const MatchPattern& pattern, int subject_reg,
		const std::string& fail_label, FunctionContext& func) {
	const StructDecl* decl = find_struct(pattern.struct_name);
	if (decl == nullptr || decl->is_class) {
		error_at("Unknown struct '" + pattern.struct_name + "' in match pattern",
			pattern.line, pattern.column);
	}

	std::vector<const MatchPattern*> field_patterns(decl->fields.size(), nullptr);
	std::unordered_set<std::string> named_fields;
	size_t positional = 0;
	for (const auto& entry : pattern.struct_entries) {
		size_t field_index = 0;
		if (entry.name.empty()) {
			if (positional >= decl->fields.size()) {
				error_at("Too many positional fields in '" + decl->name + "' pattern",
					pattern.line, pattern.column);
			}
			field_index = positional++;
		} else {
			if (!named_fields.insert(entry.name).second) {
				error_at("Field '" + entry.name + "' is repeated in '" + decl->name + "' pattern",
					pattern.line, pattern.column);
			}
			auto it = std::find_if(decl->fields.begin(), decl->fields.end(),
				[&](const StructField& field) { return field.name == entry.name; });
			if (it == decl->fields.end()) {
				error_at("Struct '" + decl->name + "' has no field named '" + entry.name + "'",
					pattern.line, pattern.column);
			}
			field_index = static_cast<size_t>(std::distance(decl->fields.begin(), it));
		}
		if (field_patterns[field_index] != nullptr) {
			error_at("Field '" + decl->fields[field_index].name + "' is repeated in '" +
				decl->name + "' pattern", pattern.line, pattern.column);
		}
		field_patterns[field_index] = entry.value.get();
	}
	for (size_t i = 0; i < field_patterns.size(); i++) {
		if (field_patterns[i] == nullptr) {
			error_at("Struct pattern '" + decl->name + "' must include field '" +
				decl->fields[i].name + "' (use '_' to ignore it)", pattern.line, pattern.column);
		}
	}

	if (const StructDecl* actual = get_register_struct(func, subject_reg)) {
		if (actual != decl) {
			func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(fail_label));
			return;
		}
	} else {
		if (!emit_type_guard(subject_reg, Variant::DICTIONARY, fail_label, func)) {
			return;
		}
		int exact = gen_struct_shape_test(subject_reg, *decl, func);
		emit_conditional_branch(IROpcode::BRANCH_ZERO, exact, fail_label, func);
		free_register(func, exact);
	}

	for (size_t i = 0; i < decl->fields.size(); i++) {
		int field_reg = gen_dict_get(subject_reg, decl->fields[i].name, func);
		apply_declared_type(field_reg, decl->fields[i].type_hint, func);
		gen_pattern_test(*field_patterns[i], field_reg, fail_label, func);
		free_register(func, field_reg);
	}
}

void CodeGenerator::gen_array_pattern_test(const MatchPattern& pattern, int subject_reg,
                                           const std::string& fail_label, FunctionContext& func) {
	// Type, length, then elements. Length tested before any element fetch.
	if (!emit_type_guard(subject_reg, Variant::ARRAY, fail_label, func)) {
		return;
	}

	const int size_reg = gen_array_size(subject_reg, func);
	const int wanted_reg = gen_int_immediate(static_cast<int64_t>(pattern.elements.size()), func);
	const int fits_reg = gen_compare(pattern.open ? IROpcode::CMP_GTE : IROpcode::CMP_EQ,
		size_reg, wanted_reg, func);
	emit_conditional_branch(IROpcode::BRANCH_ZERO, fits_reg, fail_label, func);
	free_register(func, fits_reg);
	free_register(func, wanted_reg);
	free_register(func, size_reg);

	for (size_t i = 0; i < pattern.elements.size(); i++) {
		int index_reg = gen_int_immediate(static_cast<int64_t>(i), func);
		int element_reg = gen_array_element(subject_reg, index_reg, func);
		gen_pattern_test(*pattern.elements[i], element_reg, fail_label, func);
		free_register(func, element_reg);
		free_register(func, index_reg);
	}
}

void CodeGenerator::gen_dictionary_pattern_test(const MatchPattern& pattern, int subject_reg,
                                                const std::string& fail_label, FunctionContext& func) {
	constexpr int64_t DICT_OP_GET = 0;
	constexpr int64_t DICT_OP_HAS = 3;
	constexpr int64_t DICT_OP_GET_SIZE = 6;

	if (!emit_type_guard(subject_reg, Variant::DICTIONARY, fail_label, func)) {
		return;
	}

	if (!pattern.open) {
		const int size_reg = gen_dictionary_op(DICT_OP_GET_SIZE, subject_reg, -1, Variant::INT, func);
		const int wanted_reg = gen_int_immediate(static_cast<int64_t>(pattern.entries.size()), func);
		const int fits_reg = gen_compare(IROpcode::CMP_EQ, size_reg, wanted_reg, func);
		emit_conditional_branch(IROpcode::BRANCH_ZERO, fits_reg, fail_label, func);
		free_register(func, fits_reg);
		free_register(func, wanted_reg);
		free_register(func, size_reg);
	}

	for (const auto& entry : pattern.entries) {
		int key_reg = gen_expr(entry.key.get(), func);

		int has_reg = gen_dictionary_op(DICT_OP_HAS, subject_reg, key_reg, Variant::BOOL, func);
		emit_conditional_branch(IROpcode::BRANCH_ZERO, has_reg, fail_label, func);
		free_register(func, has_reg);

		if (entry.value) {
			int value_reg = gen_dictionary_op(DICT_OP_GET, subject_reg, key_reg,
				IRInstruction::TypeHint_NONE, func);
			gen_pattern_test(*entry.value, value_reg, fail_label, func);
			free_register(func, value_reg);
		}

		free_register(func, key_reg);
	}
}

bool CodeGenerator::emit_type_guard(int value_reg, IRInstruction::TypeHint type,
                                    const std::string& fail_label, FunctionContext& func) {
	const IRInstruction::TypeHint known = get_register_type(func, value_reg);
	if (known == type) {
		return true;
	}
	if (known != IRInstruction::TypeHint_NONE) {
		// Known different type: unconditional jump, no destructuring emitted.
		func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(fail_label));
		return false;
	}

	int test_reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(test_reg),
		IRValue::reg(value_reg), IRValue::imm(static_cast<int64_t>(type)));
	set_register_type(func, test_reg, Variant::BOOL);
	emit_conditional_branch(IROpcode::BRANCH_ZERO, test_reg, fail_label, func);
	free_register(func, test_reg);
	return true;
}

int CodeGenerator::gen_array_size(int array_reg, FunctionContext& func) {
	int size_reg = alloc_register(func);
	IRInstruction call(IROpcode::CALL_SYSCALL);
	call.operands.push_back(IRValue::reg(size_reg));
	call.operands.push_back(IRValue::imm(ECALL_ARRAY_SIZE));
	call.operands.push_back(IRValue::reg(array_reg));
	call.type_hint = Variant::INT;
	func.ir.instructions.push_back(call);
	set_register_type(func, size_reg, Variant::INT);
	return size_reg;
}

int CodeGenerator::gen_array_element(int array_reg, int index_reg, FunctionContext& func) {
	int element_reg = alloc_register(func);
	IRInstruction call(IROpcode::CALL_SYSCALL);
	call.operands.push_back(IRValue::reg(element_reg));
	call.operands.push_back(IRValue::imm(ECALL_ARRAY_AT));
	call.operands.push_back(IRValue::reg(array_reg));
	call.operands.push_back(IRValue::reg(index_reg));
	func.ir.instructions.push_back(call);
	return element_reg;
}

int CodeGenerator::gen_dictionary_op(int64_t op, int dict_reg, int key_reg,
                                     IRInstruction::TypeHint result_type, FunctionContext& func) {
	int result_reg = alloc_register(func);
	IRInstruction call(IROpcode::CALL_SYSCALL);
	call.operands.push_back(IRValue::reg(result_reg));
	call.operands.push_back(IRValue::imm(ECALL_DICTIONARY_OPS));
	call.operands.push_back(IRValue::imm(op));
	call.operands.push_back(IRValue::reg(dict_reg));
	if (key_reg >= 0) {
		call.operands.push_back(IRValue::reg(key_reg));
	}
	call.type_hint = result_type;
	func.ir.instructions.push_back(call);
	if (result_type != IRInstruction::TypeHint_NONE) {
		set_register_type(func, result_reg, result_type);
	}
	return result_reg;
}

int CodeGenerator::gen_struct_shape_test(int value_reg, const StructDecl& decl,
	FunctionContext& func)
{
	int result_reg = alloc_register(func);
	IRInstruction check(IROpcode::STRUCT_CHECK);
	check.operands.push_back(IRValue::reg(result_reg));
	check.operands.push_back(IRValue::reg(value_reg));
	const std::vector<const StructField*> fields = struct_fields(decl);
	check.operands.push_back(IRValue::imm(int64_t(fields.size())));
	for (const StructField* field : fields) {
		check.operands.push_back(IRValue::imm(add_string_constant(field->name)));
	}
	check.type_hint = Variant::BOOL;
	func.ir.instructions.push_back(std::move(check));
	set_register_type(func, result_reg, Variant::BOOL);
	return result_reg;
}

int CodeGenerator::require_struct_value(int value_reg, const StructDecl& decl,
	const std::string& what, FunctionContext& func, int line, int column)
{
	if (const StructDecl* actual = get_register_struct(func, value_reg)) {
		if (actual != &decl) {
			error_at("Cannot assign a '" + actual->name + "' to " + what +
				" of type '" + decl.name + "'", line, column);
		}
		return value_reg;
	}

	const IRInstruction::TypeHint known = get_register_type(func, value_reg);
	if (known != IRInstruction::TypeHint_NONE && known != Variant::DICTIONARY) {
		error_at("Cannot assign a value of type " + std::string(variant_type_name(known)) +
			" to " + what + " of type '" + decl.name + "'", line, column);
	}
	if (!m_struct_checks) {
		set_register_struct(func, value_reg, &decl);
		return value_reg;
	}

	const std::string failed = make_label("struct_shape_failed");
	const std::string passed = make_label("struct_shape_ok");
	if (known != Variant::DICTIONARY) {
		int is_dictionary = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(is_dictionary),
			IRValue::reg(value_reg), IRValue::imm(int64_t(Variant::DICTIONARY)));
		set_register_type(func, is_dictionary, Variant::BOOL);
		emit_conditional_branch(IROpcode::BRANCH_ZERO, is_dictionary, failed, func);
		free_register(func, is_dictionary);
	}
	int exact = gen_struct_shape_test(value_reg, decl, func);
	if (m_struct_deep_checks) {
		emit_conditional_branch(IROpcode::BRANCH_ZERO, exact, failed, func);
		for (const StructField* field : struct_fields(decl)) {
			if (find_struct(field->type_hint.single_name()) != nullptr) continue;
			const IRInstruction::TypeHint field_type = single_type_from(field->type_hint);
			if (field_type == IRInstruction::TypeHint_NONE) continue;
			int field_reg = gen_dict_get(value_reg, field->name, func);
			field_reg = coerce_to_declared_type(field_reg, field_type, func,
				"field '" + field->name + "' of " + what, line, column);
			gen_dict_set(value_reg, field->name, field_reg, func);
			free_register(func, field_reg);
		}
		func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(passed));
	} else {
		emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, exact, passed, func);
	}
	free_register(func, exact);
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(failed));
	IRInstruction fail(IROpcode::THROW, ir_str("TypeError"),
		ir_str(what + " is not a " + decl.name));
	fail.operands.push_back(IRValue::imm(0));
	func.ir.instructions.push_back(std::move(fail));
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(passed));
	set_register_struct(func, value_reg, &decl);
	return value_reg;
}

int CodeGenerator::gen_compare(IROpcode opcode, int left_reg, int right_reg, FunctionContext& func) {
	int result_reg = alloc_register(func);
	IRInstruction cmp(opcode, IRValue::reg(result_reg), IRValue::reg(left_reg),
	                  IRValue::reg(right_reg));
	cmp.type_hint = fused_compare_type(get_register_type(func, left_reg),
	                                   get_register_type(func, right_reg));
	func.ir.instructions.push_back(cmp);
	set_register_type(func, result_reg, Variant::BOOL);
	return result_reg;
}

// Skipped in coroutines: suspension restores slots but not the mark.
int CodeGenerator::open_scope(FunctionContext& func) {
	if (func.ir.is_coroutine) {
		return -1;
	}
	const int scope_id = func.next_scope_id++;
	func.ir.instructions.emplace_back(IROpcode::SCOPE_MARK, IRValue::imm(scope_id));
	return scope_id;
}

void CodeGenerator::emit_scope_release(int scope_id, FunctionContext& func) {
	if (scope_id < 0) {
		return;
	}
	func.ir.instructions.emplace_back(IROpcode::SCOPE_RELEASE, IRValue::imm(scope_id));
}

int CodeGenerator::push_block_scope(FunctionContext& func) {
	const int scope_id = open_scope(func);
	push_scope(func);
	return scope_id;
}

void CodeGenerator::pop_block_scope(int scope_id, FunctionContext& func) {
	pop_scope(func);
	emit_scope_release(scope_id, func);
}

namespace {
// Every name a statement list assigns to, nested statements included.
void collect_assigned_names(const Stmt* stmt, std::unordered_set<std::string>& names);

void collect_assigned_names(const std::vector<StmtPtr>& body,
	std::unordered_set<std::string>& names)
{
	for (const auto& stmt : body) collect_assigned_names(stmt.get(), names);
}

void collect_assigned_names(const Stmt* stmt, std::unordered_set<std::string>& names) {
	if (stmt == nullptr) return;
	if (auto* assign = dynamic_cast<const AssignStmt*>(stmt)) {
		if (!assign->name.empty()) names.insert(assign->name);
	} else if (auto* if_stmt = dynamic_cast<const IfStmt*>(stmt)) {
		collect_assigned_names(if_stmt->then_branch, names);
		collect_assigned_names(if_stmt->else_branch, names);
	} else if (auto* while_stmt = dynamic_cast<const WhileStmt*>(stmt)) {
		collect_assigned_names(while_stmt->body, names);
	} else if (auto* for_stmt = dynamic_cast<const ForStmt*>(stmt)) {
		collect_assigned_names(for_stmt->body, names);
	} else if (auto* match_stmt = dynamic_cast<const MatchStmt*>(stmt)) {
		for (const auto& branch : match_stmt->branches)
			collect_assigned_names(branch.body, names);
	}
}
} // namespace

// A body runs many times, so an assignment below a use still precedes that use
// on the next pass: linear order only settles the character property for
// straight-line code. Drop it for everything the loop assigns, before the loop
// emits anything that could fold a character's length() to 1.
void CodeGenerator::invalidate_loop_character_registers(const std::vector<StmtPtr>& body,
	FunctionContext& func)
{
	if (func.string_character_registers.empty() && func.codepoint_value_registers.empty()) return;
	std::unordered_set<std::string> assigned;
	collect_assigned_names(body, assigned);
	for (const std::string& name : assigned) {
		if (Variable* local = find_variable(func, name)) {
			func.string_character_registers.erase(local->register_num);
			func.codepoint_value_registers.erase(local->register_num);
		}
	}
}

void CodeGenerator::gen_while(const WhileStmt* stmt, FunctionContext& func) {
	invalidate_loop_character_registers(stmt->body, func);
	std::string body_label = make_label("loop");
	std::string continue_label = make_label("loop_continue");
	std::string end_label = make_label("endloop");

	func.loops.push_back({end_label, continue_label});
	const int scope_id = open_scope(func);
	// Rotate the loop.  The first test enters the body, while later passes use
	// the bottom test as the back edge so an interpreter does not pay a second
	// taken jump on every iteration.  The condition is deliberately generated
	// twice: it observes body-side changes before each later pass.
	int cond_reg = gen_expr(stmt->condition.get(), func);
	emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, end_label, func);
	free_register(func, cond_reg);

	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(body_label));
	push_scope(func);
	for (const auto& s : stmt->body) {
		gen_stmt(s.get(), func);
	}
	pop_scope(func);
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(continue_label));
	emit_scope_release(scope_id, func);
	cond_reg = gen_expr(stmt->condition.get(), func);
	emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, cond_reg, body_label, func);
	free_register(func, cond_reg);
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end_label));
	emit_scope_release(scope_id, func);

	func.loops.pop_back();
}

void CodeGenerator::gen_for(const ForStmt* stmt, FunctionContext& func) {
	invalidate_loop_character_registers(stmt->body, func);
	auto* call_expr = dynamic_cast<const CallExpr*>(stmt->iterable.get());
	bool is_range = call_expr && call_expr->function_name == "range";

	auto* literal = dynamic_cast<const LiteralExpr*>(stmt->iterable.get());
	if (literal) {
		if (literal->lit_type == LiteralExpr::Type::BOOL ||
		    literal->lit_type == LiteralExpr::Type::NULL_VAL) {
			error_at("Cannot iterate over a non-iterable value in a 'for' loop", stmt,
				"Did you mean 'for " + stmt->variable + " in range(N):'?");
		}
	}

	if (!is_range) {
		// Evaluate iterable first: an integer bound takes the numeric loop.
		int array_reg = gen_expr(stmt->iterable.get(), func);
		const StructDecl* iterable_element = nullptr;
		const TraitDecl* iterable_trait = nullptr;
		if (auto it = func.array_element_structs.find(array_reg);
			it != func.array_element_structs.end()) {
			iterable_element = it->second;
		}
		if (auto it = func.array_element_traits.find(array_reg);
			it != func.array_element_traits.end()) {
			iterable_trait = it->second;
		}

		if (get_register_type(func, array_reg) == Variant::INT) {
			int start_reg = gen_int_immediate(0, func);
			int step_reg = gen_int_immediate(1, func);
			gen_numeric_for(stmt, start_reg, array_reg, step_reg, func);
			return;
		}
		if (get_register_type(func, array_reg) == Variant::FLOAT) {
			int start_reg = gen_float_immediate(0.0, func);
			int step_reg = gen_float_immediate(1.0, func);
			gen_numeric_for(stmt, start_reg, array_reg, step_reg, func);
			return;
		}

		std::string loop_label = make_label("for_loop");
		std::string continue_label = make_label("for_continue");
		std::string end_label = make_label("for_end");

		func.loops.push_back({end_label, continue_label});
		push_scope(func);

		// Packed arrays use VCALL size()/get(); ECALL_ARRAY_SIZE/AT are Array-only.
		const bool packed_walk = is_packed_array_type(get_register_type(func, array_reg));

		// String has its own size/at syscalls (ECALL_STRING_SIZE/AT).
		const bool string_walk = get_register_type(func, array_reg) == Variant::STRING;

		const bool unknown_iterable = get_register_type(func, array_reg) == IRInstruction::TypeHint_NONE;

		// String is a value type: the walk is over the value the loop began
		// with, so it takes its own copy and reassigning the source variable in
		// the body cannot move the walk.
		if (string_walk) {
			int snapshot_reg = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(snapshot_reg),
				IRValue::reg(array_reg));
			set_register_type(func, snapshot_reg, Variant::STRING);
			if (!func.ir.is_coroutine) {
				gen_string_walk(stmt, snapshot_reg, func);
				return;
			}
			// Neither batch survives a suspension. Fall back to one character
			// at a time; the index and String are frame slots, so they restore.
			array_reg = snapshot_reg;
		}
		if (get_register_type(func, array_reg) == Variant::ARRAY) {
			gen_array_walk(stmt, array_reg, func, iterable_element, iterable_trait);
			return;
		}

		// Float joins the int arm: ceil(f) replaces the bound before the loop.
		int is_float_reg = -1;
		if (unknown_iterable) {
			is_float_reg = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(is_float_reg),
				IRValue::reg(array_reg), IRValue::imm(static_cast<int64_t>(Variant::FLOAT)));
			set_register_type(func, is_float_reg, Variant::BOOL);

			const std::string not_float_label = make_label("for_not_a_float");
			emit_conditional_branch(IROpcode::BRANCH_ZERO, is_float_reg, not_float_label, func);

			int as_float_reg = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(as_float_reg),
				IRValue::reg(array_reg));
			set_register_type(func, as_float_reg, Variant::FLOAT);
			int ceiled_reg = gen_global_call(*find_global_function("ceilf"), { as_float_reg },
				func, nullptr);
			int bound_reg = alloc_register(func);
			auto& to_int = func.ir.instructions.emplace_back(IROpcode::CONVERT, IRValue::reg(bound_reg),
				IRValue::reg(ceiled_reg), IRValue::imm(static_cast<int64_t>(Variant::FLOAT)));
			to_int.type_hint = Variant::INT;
			func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(array_reg),
				IRValue::reg(bound_reg));
			free_register(func, bound_reg);
			free_register(func, ceiled_reg);
			free_register(func, as_float_reg);

			func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(not_float_label));
		}

		int is_int_reg = -1;
		if (unknown_iterable) {
			is_int_reg = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(is_int_reg),
				IRValue::reg(array_reg), IRValue::imm(static_cast<int64_t>(Variant::INT)));
			set_register_type(func, is_int_reg, Variant::BOOL);
		}

		// Dictionary → keys conversion (no-op on int/Array).
		if (!packed_walk && !string_walk) {
			gen_dictionary_keys_for_iteration(array_reg, func);
		}

		int is_array_reg = -1;
		int is_string_reg = -1;
		if (unknown_iterable) {
			is_array_reg = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(is_array_reg),
				IRValue::reg(array_reg), IRValue::imm(static_cast<int64_t>(Variant::ARRAY)));
			set_register_type(func, is_array_reg, Variant::BOOL);

			is_string_reg = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(is_string_reg),
				IRValue::reg(array_reg), IRValue::imm(static_cast<int64_t>(Variant::STRING)));
			set_register_type(func, is_string_reg, Variant::BOOL);
		}

		int index_reg = alloc_register(func);
		auto& index_load = func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(index_reg), IRValue::imm(0));
		index_load.type_hint = Variant::INT;
		set_register_type(func, index_reg, Variant::INT);

		int one_reg = alloc_register(func);
		auto& one_load = func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(one_reg), IRValue::imm(1));
		one_load.type_hint = Variant::INT;
		set_register_type(func, one_reg, Variant::INT);

		auto emit_array_size = [&](int dest) {
			IRInstruction size_syscall(IROpcode::CALL_SYSCALL);
			size_syscall.operands.push_back(IRValue::reg(dest));
			size_syscall.operands.push_back(IRValue::imm(ECALL_ARRAY_SIZE));
			size_syscall.operands.push_back(IRValue::reg(array_reg));
			size_syscall.type_hint = Variant::INT;
			func.ir.instructions.push_back(size_syscall);
		};
		auto emit_vcall_size = [&](int dest) {
			IRInstruction size_call(IROpcode::VCALL);
			size_call.operands.push_back(IRValue::reg(dest));
			size_call.operands.push_back(IRValue::reg(array_reg));
			size_call.operands.push_back(ir_str("size"));
			size_call.operands.push_back(IRValue::imm(0));
			func.ir.instructions.push_back(size_call);
		};
		auto emit_array_at = [&](int dest) {
			IRInstruction at_syscall(IROpcode::CALL_SYSCALL);
			at_syscall.operands.push_back(IRValue::reg(dest));
			at_syscall.operands.push_back(IRValue::imm(ECALL_ARRAY_AT));
			at_syscall.operands.push_back(IRValue::reg(array_reg));
			at_syscall.operands.push_back(IRValue::reg(index_reg));
			func.ir.instructions.push_back(at_syscall);
		};
		auto emit_vcall_get = [&](int dest) {
			IRInstruction at_call(IROpcode::VCALL);
			at_call.operands.push_back(IRValue::reg(dest));
			at_call.operands.push_back(IRValue::reg(array_reg));
			at_call.operands.push_back(ir_str("get"));
			at_call.operands.push_back(IRValue::imm(1));
			at_call.operands.push_back(IRValue::reg(index_reg));
			func.ir.instructions.push_back(at_call);
		};
		auto emit_string_size = [&](int dest) {
			IRInstruction size_syscall(IROpcode::CALL_SYSCALL);
			size_syscall.operands.push_back(IRValue::reg(dest));
			size_syscall.operands.push_back(IRValue::imm(ECALL_STRING_SIZE));
			size_syscall.operands.push_back(IRValue::reg(array_reg));
			size_syscall.type_hint = Variant::INT;
			func.ir.instructions.push_back(size_syscall);
		};
		auto emit_string_at = [&](int dest) {
			IRInstruction at_syscall(IROpcode::CALL_SYSCALL);
			at_syscall.operands.push_back(IRValue::reg(dest));
			at_syscall.operands.push_back(IRValue::imm(ECALL_STRING_AT));
			at_syscall.operands.push_back(IRValue::reg(array_reg));
			at_syscall.operands.push_back(IRValue::reg(index_reg));
			func.ir.instructions.push_back(at_syscall);
		};

		// Type tests hoisted; only branches are per-iteration, body emitted once.
		auto emit_four_way = [&](const char* what, int dest,
			const std::function<void(int)>& int_arm,
			const std::function<void(int)>& array_arm, const std::function<void(int)>& string_arm,
			const std::function<void(int)>& generic_arm)
		{
			const std::string join_label = make_label(std::string("for_") + what + "_done");
			const std::string not_int_label = make_label(std::string("for_") + what + "_not_int");
			const std::string not_array_label = make_label(std::string("for_") + what + "_not_array");
			const std::string generic_label = make_label(std::string("for_") + what + "_generic");

			emit_conditional_branch(IROpcode::BRANCH_ZERO, is_int_reg, not_int_label, func);
			int_arm(dest);
			func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(join_label));

			func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(not_int_label));
			emit_conditional_branch(IROpcode::BRANCH_ZERO, is_array_reg, not_array_label, func);
			array_arm(dest);
			func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(join_label));

			func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(not_array_label));
			emit_conditional_branch(IROpcode::BRANCH_ZERO, is_string_reg, generic_label, func);
			string_arm(dest);
			func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(join_label));

			func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(generic_label));
			generic_arm(dest);

			func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(join_label));
		};

		const int scope_id = open_scope(func);
		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(loop_label));
		emit_scope_release(scope_id, func);
		int size_reg = alloc_register(func);
		if (unknown_iterable) {
			emit_four_way("size", size_reg,
				[&](int dest) {
					func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(dest),
						IRValue::reg(array_reg));
				},
				emit_array_size, emit_string_size, emit_vcall_size);
		} else if (packed_walk) {
			emit_vcall_size(size_reg);
		} else if (string_walk) {
			emit_string_size(size_reg);
		} else {
			emit_array_size(size_reg);
		}
		set_register_type(func, size_reg, Variant::INT);

		int cond_reg = alloc_register(func);
		auto& cmp_instr = func.ir.instructions.emplace_back(IROpcode::CMP_LT, IRValue::reg(cond_reg),
		                               IRValue::reg(index_reg), IRValue::reg(size_reg));
		cmp_instr.type_hint = Variant::INT;
		set_register_type(func, cond_reg, Variant::BOOL);

		emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, end_label, func);
		free_register(func, cond_reg);

		int elem_reg = alloc_register(func);
		if (unknown_iterable) {
			// Float path widens the integer counter to float.
			emit_four_way("elem", elem_reg,
				[&](int dest) {
					const std::string float_counter_label = make_label("for_float_counter");
					const std::string counter_done_label = make_label("for_counter_done");
					emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, is_float_reg,
						float_counter_label, func);
					func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(dest),
						IRValue::reg(index_reg));
					func.ir.instructions.emplace_back(IROpcode::JUMP,
						ir_label(counter_done_label));
					func.ir.instructions.emplace_back(IROpcode::LABEL,
						ir_label(float_counter_label));
					auto& widen = func.ir.instructions.emplace_back(IROpcode::CONVERT,
						IRValue::reg(dest), IRValue::reg(index_reg),
						IRValue::imm(static_cast<int64_t>(Variant::INT)));
					widen.type_hint = Variant::FLOAT;
					func.ir.instructions.emplace_back(IROpcode::LABEL,
						ir_label(counter_done_label));
				},
				emit_array_at, emit_string_at, emit_vcall_get);
			set_register_type(func, elem_reg, IRInstruction::TypeHint_NONE);
		} else if (packed_walk) {
			emit_vcall_get(elem_reg);
		} else if (string_walk) {
			emit_string_at(elem_reg);
			set_register_type(func, elem_reg, Variant::STRING);
			std::unordered_set<std::string> assigned_in_body;
			collect_assigned_names(stmt->body, assigned_in_body);
			if (assigned_in_body.count(stmt->variable) == 0) {
				func.string_character_registers.insert(elem_reg);
			}
		} else {
			emit_array_at(elem_reg);
		}

		declare_variable(func, stmt->variable, elem_reg, false, stmt);
		if (iterable_element != nullptr) {
			set_register_struct(func, elem_reg, iterable_element);
			func.declared_structs[elem_reg] = iterable_element;
		}
		if (iterable_trait != nullptr) {
			require_trait_value(elem_reg, *iterable_trait,
				"an element of Array[" + iterable_trait->name + "]", func,
				stmt->line, stmt->column);
			func.declared_traits[elem_reg].insert(iterable_trait);
			add_register_trait(func, elem_reg, iterable_trait);
			func.trait_only_registers.insert(elem_reg);
		}
		push_scope(func);
		for (const auto& s : stmt->body) {
			gen_stmt(s.get(), func);
		}
		pop_scope(func);

		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(continue_label));
		int new_idx_reg = alloc_register(func);
		auto& add_instr = func.ir.instructions.emplace_back(IROpcode::ADD, IRValue::reg(new_idx_reg),
		                               IRValue::reg(index_reg), IRValue::reg(one_reg));
		add_instr.type_hint = Variant::INT;
		set_register_type(func, new_idx_reg, Variant::INT);
		func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(index_reg), IRValue::reg(new_idx_reg));
		free_register(func, new_idx_reg);

		func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(loop_label));
		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end_label));
		emit_scope_release(scope_id, func);
		pop_scope(func);
		func.loops.pop_back();
		if (unknown_iterable) {
			free_register(func, is_int_reg);
			free_register(func, is_array_reg);
		}
		free_register(func, one_reg);
		free_register(func, index_reg);
		free_register(func, elem_reg);
		return;
	}

	int start_reg = -1, end_reg = -1, step_reg = -1;

	if (call_expr->arguments.size() == 1) {
		start_reg = gen_int_immediate(0, func);
		end_reg = gen_expr(call_expr->arguments[0].get(), func);
		step_reg = gen_int_immediate(1, func);
	} else if (call_expr->arguments.size() == 2) {
		start_reg = gen_expr(call_expr->arguments[0].get(), func);
		end_reg = gen_expr(call_expr->arguments[1].get(), func);
		step_reg = gen_int_immediate(1, func);
	} else if (call_expr->arguments.size() == 3) {
		start_reg = gen_expr(call_expr->arguments[0].get(), func);
		end_reg = gen_expr(call_expr->arguments[1].get(), func);
		step_reg = gen_expr(call_expr->arguments[2].get(), func);
	} else {
		error_at("range() takes 1, 2, or 3 arguments, got " +
			std::to_string(call_expr->arguments.size()), call_expr);
	}

	gen_numeric_for(stmt, start_reg, end_reg, step_reg, func);
}

void CodeGenerator::gen_array_walk(const ForStmt* stmt, int array_reg, FunctionContext& func,
	const StructDecl* element_struct, const TraitDecl* element_trait)
{
	constexpr int64_t BATCH_SIZE = 16;
	const std::string refill_label = make_label("array_refill");
	const std::string have_label = make_label("array_have");
	const std::string continue_label = make_label("array_continue");
	const std::string end_label = make_label("array_end");
	func.loops.push_back({ end_label, continue_label });
	push_scope(func);

	auto int_const = [&](int64_t value) {
		const int reg = alloc_register(func);
		auto& load = func.ir.instructions.emplace_back(IROpcode::LOAD_IMM,
			IRValue::reg(reg), IRValue::imm(value));
		load.type_hint = Variant::INT;
		set_register_type(func, reg, Variant::INT);
		return reg;
	};
	auto int_binop = [&](IROpcode op, int dest, int lhs, int rhs) {
		auto& instr = func.ir.instructions.emplace_back(op, IRValue::reg(dest),
			IRValue::reg(lhs), IRValue::reg(rhs));
		instr.type_hint = Variant::INT;
		set_register_type(func, dest, Variant::INT);
	};

	const int index_reg = int_const(0);
	const int batch_index_reg = int_const(0);
	const int one_reg = int_const(1);
	const int buffer_base = func.next_array_batch_id++;
	const int left_reg = alloc_register(func);
	set_register_type(func, left_reg, Variant::INT);

	const int batch_scope = open_scope(func);
	func.ir.array_batch_scopes.emplace_back(batch_scope, int64_t(buffer_base));
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(refill_label));
	emit_scope_release(batch_scope, func);
	IRInstruction refill(IROpcode::CALL_SYSCALL);
	refill.operands.push_back(IRValue::reg(left_reg));
	refill.operands.push_back(IRValue::imm(ECALL_ARRAY_BATCH));
	refill.operands.push_back(IRValue::reg(array_reg));
	refill.operands.push_back(IRValue::reg(index_reg));
	refill.operands.push_back(IRValue::imm(BATCH_SIZE));
	refill.operands.push_back(IRValue::imm(buffer_base));
	refill.type_hint = Variant::INT;
	func.ir.instructions.push_back(refill);
	emit_conditional_branch(IROpcode::BRANCH_ZERO, left_reg, end_label, func);

	const int body_scope = open_scope(func);
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(have_label));
	emit_scope_release(body_scope, func);
	const int elem_reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::BATCH_GET, IRValue::reg(elem_reg),
		IRValue::imm(buffer_base), IRValue::reg(batch_index_reg));
	declare_variable(func, stmt->variable, elem_reg, false, stmt);
	if (element_struct != nullptr) {
		set_register_struct(func, elem_reg, element_struct);
		func.declared_structs[elem_reg] = element_struct;
	}
	if (element_trait != nullptr) {
		require_trait_value(elem_reg, *element_trait,
			"an element of Array[" + element_trait->name + "]", func,
			stmt->line, stmt->column);
		func.declared_traits[elem_reg].insert(element_trait);
		add_register_trait(func, elem_reg, element_trait);
		func.trait_only_registers.insert(elem_reg);
	}
	push_scope(func);
	for (const auto& body_stmt : stmt->body) gen_stmt(body_stmt.get(), func);
	pop_scope(func);

	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(continue_label));
	int_binop(IROpcode::ADD, index_reg, index_reg, one_reg);
	int_binop(IROpcode::ADD, batch_index_reg, batch_index_reg, one_reg);
	int_binop(IROpcode::SUB, left_reg, left_reg, one_reg);
	emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, left_reg, have_label, func);
	// The next refill starts at the absolute index and resets the buffer index.
	func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(batch_index_reg),
		IRValue::imm(0)).type_hint = Variant::INT;
	func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(refill_label));
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end_label));
	emit_scope_release(body_scope, func);
	emit_scope_release(batch_scope, func);

	pop_scope(func);
	func.loops.pop_back();
	free_register(func, elem_reg);
	free_register(func, left_reg);
	free_register(func, one_reg);
	free_register(func, batch_index_reg);
	free_register(func, index_reg);
}

// `for c in <String>`: the characters come in batches, so the walk costs one
// syscall per batch instead of one per character -- and a syscall is what these
// loops are made of. ECALL_STRING_BATCH answers with the first scoped index and
// how many it made, the run being consecutive, so handing out the next character
// is arithmetic on an index and a store of a type tag.
//
// Two scopes. The outer one holds the batch and is released only when it runs
// out. The inner one is marked after each refill and released every pass, so
// what the body makes cannot pile up between refills -- and since its body spans
// exactly the loop, the backend elides it outright when the body makes nothing,
// which leaves a walk like `for c in text: n += c.length()` with one syscall per
// character rather than three.
//
// When the body only uses code points, the host writes UTF-32 to a guest
// buffer instead. Kept narrow: any escape (call, store, comparison, match)
// would need a boxing fallback. The scoped String batch handles those.
bool CodeGenerator::string_walk_uses_only_codepoints(const ForStmt* stmt) const {
	const std::string &name = stmt->variable;
	std::function<bool(const Expr*)> expression = [&](const Expr* expr) -> bool {
		if (expr == nullptr) return true;
		if (const auto* variable = dynamic_cast<const VariableExpr*>(expr)) {
			return variable->name != name;
		}
		if (const auto* binary = dynamic_cast<const BinaryExpr*>(expr)) {
			return expression(binary->left.get()) && expression(binary->right.get());
		}
		if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
			return expression(unary->operand.get());
		}
		if (const auto* await_expr = dynamic_cast<const AwaitExpr*>(expr)) {
			return expression(await_expr->operand.get());
		}
		if (const auto* type_test = dynamic_cast<const TypeTestExpr*>(expr)) {
			return expression(type_test->value.get());
		}
		if (const auto* cast = dynamic_cast<const CastExpr*>(expr)) {
			return expression(cast->value.get());
		}
		if (const auto* ternary = dynamic_cast<const TernaryExpr*>(expr)) {
			return expression(ternary->condition.get()) && expression(ternary->true_value.get()) &&
				expression(ternary->false_value.get());
		}
		if (const auto* call = dynamic_cast<const CallExpr*>(expr)) {
			if (call->function_name == "ord" && call->arguments.size() == 1) {
				if (const auto* argument = dynamic_cast<const VariableExpr*>(call->arguments[0].get());
					argument != nullptr && argument->name == name) return true;
			}
			for (const auto& arg : call->arguments) if (!expression(arg.get())) return false;
			return true;
		}
		if (const auto* member = dynamic_cast<const MemberCallExpr*>(expr)) {
			if (const auto* object = dynamic_cast<const VariableExpr*>(member->object.get());
				object != nullptr && object->name == name)
			{
				return member->is_method_call && member->arguments.empty() &&
					(member->member_name == "length" || member->member_name == "size");
			}
			if (!expression(member->object.get())) return false;
			for (const auto& arg : member->arguments) if (!expression(arg.get())) return false;
			return true;
		}
		if (const auto* index = dynamic_cast<const IndexExpr*>(expr)) {
			return expression(index->object.get()) && expression(index->index.get());
		}
		if (const auto* array = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
			for (const auto& element : array->elements) if (!expression(element.get())) return false;
			return true;
		}
		if (const auto* dictionary = dynamic_cast<const DictionaryLiteralExpr*>(expr)) {
			for (const auto& [key, value] : dictionary->elements) {
				if (!expression(key.get()) || !expression(value.get())) return false;
			}
			return true;
		}
		// Lambda capture is an escape; the buffer may be refilled before it runs.
		if (dynamic_cast<const LambdaExpr*>(expr) != nullptr) return false;
		return true; // literal
	};

	std::function<bool(const std::vector<StmtPtr>&)> statements;
	std::function<bool(const Stmt*)> statement = [&](const Stmt* body_stmt) -> bool {
		if (const auto* expr_stmt = dynamic_cast<const ExprStmt*>(body_stmt))
			return expression(expr_stmt->expression.get());
		if (const auto* declaration = dynamic_cast<const VarDeclStmt*>(body_stmt))
			return declaration->name != name && expression(declaration->initializer.get());
		if (const auto* assignment = dynamic_cast<const AssignStmt*>(body_stmt))
			return assignment->name != name && expression(assignment->target.get()) &&
				expression(assignment->value.get());
		if (const auto* returned = dynamic_cast<const ReturnStmt*>(body_stmt))
			return expression(returned->value.get());
		if (const auto* branch = dynamic_cast<const IfStmt*>(body_stmt))
			return expression(branch->condition.get()) &&
				(!branch->binding || (branch->binding->name != name &&
					expression(branch->binding->initializer.get()))) &&
				statements(branch->then_branch) && statements(branch->else_branch);
		if (const auto* loop = dynamic_cast<const WhileStmt*>(body_stmt))
			return expression(loop->condition.get()) && statements(loop->body);
		if (const auto* loop = dynamic_cast<const ForStmt*>(body_stmt))
			return loop->variable != name && expression(loop->iterable.get()) && statements(loop->body);
		// Match patterns may bind or compare the value, requiring a String.
		if (dynamic_cast<const MatchStmt*>(body_stmt) != nullptr) return false;
		return dynamic_cast<const BreakStmt*>(body_stmt) != nullptr ||
			dynamic_cast<const ContinueStmt*>(body_stmt) != nullptr ||
			dynamic_cast<const PassStmt*>(body_stmt) != nullptr ||
			dynamic_cast<const BreakpointStmt*>(body_stmt) != nullptr;
	};
	statements = [&](const std::vector<StmtPtr>& body) {
		for (const auto& body_stmt : body) if (!statement(body_stmt.get())) return false;
		return true;
	};
	return statements(stmt->body);
}

void CodeGenerator::gen_string_walk(const ForStmt* stmt, int string_reg, FunctionContext& func) {
	if (string_walk_uses_only_codepoints(stmt)) {
		constexpr int64_t BATCH_SIZE = 256;
		const int64_t buffer_token = func.next_codepoint_batch_id++;
		func.ir.codepoint_batch_buffers.push_back(buffer_token);
		const std::string refill_label = make_label("for_codepoint_refill");
		const std::string have_label = make_label("for_codepoint_have");
		const std::string continue_label = make_label("for_codepoint_continue");
		const std::string end_label = make_label("for_codepoint_end");

		func.loops.push_back({ end_label, continue_label });
		push_scope(func);
		auto int_const = [&](int64_t value) {
			int reg = alloc_register(func);
			auto& load = func.ir.instructions.emplace_back(IROpcode::LOAD_IMM,
				IRValue::reg(reg), IRValue::imm(value));
			load.type_hint = Variant::INT;
			set_register_type(func, reg, Variant::INT);
			return reg;
		};
		auto int_binop = [&](IROpcode op, int dest, int lhs, int rhs) {
			auto& instr = func.ir.instructions.emplace_back(op, IRValue::reg(dest),
				IRValue::reg(lhs), IRValue::reg(rhs));
			instr.type_hint = Variant::INT;
			set_register_type(func, dest, Variant::INT);
		};

		const int index_reg = int_const(0);
		const int one_reg = int_const(1);
		const int left_reg = alloc_register(func);
		set_register_type(func, left_reg, Variant::INT);
		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(refill_label));
		IRInstruction refill(IROpcode::CALL_SYSCALL);
		refill.operands.push_back(IRValue::reg(left_reg));
		refill.operands.push_back(IRValue::imm(ECALL_STRING_CODEPOINT_BATCH));
		refill.operands.push_back(IRValue::reg(string_reg));
		refill.operands.push_back(IRValue::reg(index_reg));
		refill.operands.push_back(IRValue::imm(BATCH_SIZE));
		refill.operands.push_back(IRValue::imm(buffer_token));
		refill.type_hint = Variant::INT;
		func.ir.instructions.push_back(refill);
		emit_conditional_branch(IROpcode::BRANCH_ZERO, left_reg, end_label, func);

		int batch_index_reg = int_const(0);
		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(have_label));
		const int elem_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::CODEPOINT_GET, IRValue::reg(elem_reg),
			IRValue::imm(buffer_token), IRValue::reg(batch_index_reg));
		// Backend stores an INT code point; this marker lets length/size fold to 1.
		set_register_type(func, elem_reg, Variant::STRING);
		func.string_character_registers.insert(elem_reg);
		func.codepoint_value_registers.insert(elem_reg);
		declare_variable(func, stmt->variable, elem_reg, false, stmt);
		push_scope(func);
		for (const auto& s : stmt->body) gen_stmt(s.get(), func);
		pop_scope(func);

		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(continue_label));
		int_binop(IROpcode::ADD, index_reg, index_reg, one_reg);
		int_binop(IROpcode::ADD, batch_index_reg, batch_index_reg, one_reg);
		int_binop(IROpcode::SUB, left_reg, left_reg, one_reg);
		emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, left_reg, have_label, func);
		func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(refill_label));
		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end_label));

		pop_scope(func);
		func.loops.pop_back();
		free_register(func, elem_reg);
		free_register(func, batch_index_reg);
		free_register(func, left_reg);
		free_register(func, one_reg);
		free_register(func, index_reg);
		return;
	}

	// Big enough that the refill disappears into the loop, small enough to leave
	// a restricted sandbox's reference budget room for the body. The host clamps
	// it further against what is actually left.
	constexpr int64_t BATCH_SIZE = 16;

	const std::string refill_label = make_label("for_refill");
	const std::string have_label = make_label("for_have");
	const std::string continue_label = make_label("for_continue");
	const std::string end_label = make_label("for_end");

	func.loops.push_back({ end_label, continue_label });
	push_scope(func);

	auto int_const = [&](int64_t value) {
		int reg = alloc_register(func);
		auto& load = func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(reg),
			IRValue::imm(value));
		load.type_hint = Variant::INT;
		set_register_type(func, reg, Variant::INT);
		return reg;
	};
	auto int_binop = [&](IROpcode op, int dest, int lhs, int rhs) {
		auto& instr = func.ir.instructions.emplace_back(op, IRValue::reg(dest),
			IRValue::reg(lhs), IRValue::reg(rhs));
		instr.type_hint = Variant::INT;
		set_register_type(func, dest, Variant::INT);
	};

	int index_reg = int_const(0);
	int one_reg = int_const(1);
	int shift_reg = int_const(32);
	int mask_reg = int_const(0xffffffff);

	// Written by the refill before anything reads them.
	int handle_reg = alloc_register(func);
	int left_reg = alloc_register(func);
	set_register_type(func, handle_reg, Variant::INT);
	set_register_type(func, left_reg, Variant::INT);

	const int batch_scope = open_scope(func);
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(refill_label));
	emit_scope_release(batch_scope, func);

	int packed_reg = alloc_register(func);
	IRInstruction refill(IROpcode::CALL_SYSCALL);
	refill.operands.push_back(IRValue::reg(packed_reg));
	refill.operands.push_back(IRValue::imm(ECALL_STRING_BATCH));
	refill.operands.push_back(IRValue::reg(string_reg));
	refill.operands.push_back(IRValue::reg(index_reg));
	refill.operands.push_back(IRValue::imm(BATCH_SIZE));
	refill.type_hint = Variant::INT;
	func.ir.instructions.push_back(refill);
	set_register_type(func, packed_reg, Variant::INT);

	// (first scoped index << 32) | count. The index is signed: a permanent slot
	// is negative, so the shift has to keep its sign.
	int_binop(IROpcode::SHR, handle_reg, packed_reg, shift_reg);
	int_binop(IROpcode::BIT_AND, left_reg, packed_reg, mask_reg);
	free_register(func, packed_reg);
	emit_conditional_branch(IROpcode::BRANCH_ZERO, left_reg, end_label, func);

	// Marked after the batch exists, so releasing it never takes the batch.
	const int body_scope = open_scope(func);
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(have_label));
	emit_scope_release(body_scope, func);

	int elem_reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::MAKE_SCOPED, IRValue::reg(elem_reg),
		IRValue::reg(handle_reg), IRValue::imm(static_cast<int64_t>(Variant::STRING)));
	set_register_type(func, elem_reg, Variant::STRING);
	std::unordered_set<std::string> assigned;
	collect_assigned_names(stmt->body, assigned);
	if (assigned.count(stmt->variable) == 0) func.string_character_registers.insert(elem_reg);

	declare_variable(func, stmt->variable, elem_reg, false, stmt);
	push_scope(func);
	for (const auto& s : stmt->body) {
		gen_stmt(s.get(), func);
	}
	pop_scope(func);

	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(continue_label));
	int_binop(IROpcode::ADD, index_reg, index_reg, one_reg);
	int_binop(IROpcode::ADD, handle_reg, handle_reg, one_reg);
	int_binop(IROpcode::SUB, left_reg, left_reg, one_reg);
	emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, left_reg, have_label, func);
	func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(refill_label));
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end_label));
	emit_scope_release(body_scope, func);
	emit_scope_release(batch_scope, func);

	pop_scope(func);
	func.loops.pop_back();
	free_register(func, elem_reg);
	free_register(func, left_reg);
	free_register(func, handle_reg);
	free_register(func, mask_reg);
	free_register(func, shift_reg);
	free_register(func, one_reg);
	free_register(func, index_reg);
}

// Counted loop: `for i in range(...)` and `for i in <int>`.
void CodeGenerator::gen_numeric_for(const ForStmt* stmt, int start_reg, int end_reg, int step_reg,
	FunctionContext& func)
{
	std::string loop_label = make_label("for_loop");
	std::string continue_label = make_label("for_continue");
	std::string end_label = make_label("for_end");

	func.loops.push_back({end_label, continue_label});
	push_scope(func);
	int loop_var_reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(loop_var_reg), IRValue::reg(start_reg));
	declare_variable(func, stmt->variable, loop_var_reg, false, stmt);

	// Counter type from start+step; end only bounds.
	const bool float_loop = get_register_type(func, start_reg) == Variant::FLOAT &&
		get_register_type(func, step_reg) == Variant::FLOAT;
	if (get_register_type(func, start_reg) == Variant::INT &&
		get_register_type(func, step_reg) == Variant::INT) {
		set_register_type(func, loop_var_reg, Variant::INT);
	} else if (float_loop) {
		set_register_type(func, loop_var_reg, Variant::FLOAT);
	}
	const IRInstruction::TypeHint numeric = float_loop ? Variant::FLOAT : Variant::INT;

	const int scope_id = open_scope(func);
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(loop_label));
	emit_scope_release(scope_id, func);

	int cond_reg = alloc_register(func);

	// Constant step: elide the run-time sign test by walking back to the
	// last write of step_reg.
	bool step_is_constant = false;
	bool step_is_positive = true;
	for (size_t i = func.ir.instructions.size(); i-- > 0; ) {
		const IRInstruction& previous = func.ir.instructions[i];
		if (ir_destination_register(previous) != step_reg) {
			continue;
		}
		if (previous.opcode == IROpcode::LOAD_IMM) {
			step_is_constant = true;
			step_is_positive = previous.operands[1].immediate() >= 0;
		} else if (previous.opcode == IROpcode::LOAD_FLOAT_IMM) {
			step_is_constant = true;
			step_is_positive = previous.operands[1].float_number() >= 0.0;
		}
		break;
	}

	if (step_is_constant) {
		auto& cmp_instr = func.ir.instructions.emplace_back(
			step_is_positive ? IROpcode::CMP_LT : IROpcode::CMP_GT, IRValue::reg(cond_reg),
			IRValue::reg(loop_var_reg), IRValue::reg(end_reg));
		cmp_instr.type_hint = numeric;
	} else {
		std::string pos_step_label = make_label("for_pos_step");
		std::string check_cond_label = make_label("for_check_cond");

		int zero_reg = float_loop ? gen_float_immediate(0.0, func) : alloc_register(func);
		if (!float_loop) {
			func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(zero_reg), IRValue::imm(0));
		}

		int step_sign_reg = alloc_register(func);
		auto& step_cmp = func.ir.instructions.emplace_back(IROpcode::CMP_GTE, IRValue::reg(step_sign_reg),
		                               IRValue::reg(step_reg), IRValue::reg(zero_reg));
		step_cmp.type_hint = numeric;
		free_register(func, zero_reg);
		emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, step_sign_reg, pos_step_label, func);

		auto& neg_cmp = func.ir.instructions.emplace_back(IROpcode::CMP_GT, IRValue::reg(cond_reg),
		                               IRValue::reg(loop_var_reg), IRValue::reg(end_reg));
		neg_cmp.type_hint = numeric;
		func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(check_cond_label));

		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(pos_step_label));
		auto& pos_cmp = func.ir.instructions.emplace_back(IROpcode::CMP_LT, IRValue::reg(cond_reg),
		                               IRValue::reg(loop_var_reg), IRValue::reg(end_reg));
		pos_cmp.type_hint = numeric;

		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(check_cond_label));
		free_register(func, step_sign_reg);
	}

	emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, end_label, func);
	free_register(func, cond_reg);

	push_scope(func);
	for (const auto& s : stmt->body) {
		gen_stmt(s.get(), func);
	}
	pop_scope(func);

	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(continue_label));

	int new_val_reg = alloc_register(func);
	auto& add_instr = func.ir.instructions.emplace_back(IROpcode::ADD, IRValue::reg(new_val_reg),
	                               IRValue::reg(loop_var_reg), IRValue::reg(step_reg));
	add_instr.type_hint = numeric;
	func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(loop_var_reg), IRValue::reg(new_val_reg));
	free_register(func, new_val_reg);

	func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(loop_label));
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end_label));
	emit_scope_release(scope_id, func);

	pop_scope(func);
	func.loops.pop_back();
	free_register(func, start_reg);
	free_register(func, end_reg);
	free_register(func, step_reg);
}

void CodeGenerator::gen_break(const BreakStmt* stmt, FunctionContext& func) {
	if (func.loops.empty()) {
		error_at("'break' outside of loop", stmt);
	}

	func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(func.loops.back().break_label));
}

void CodeGenerator::gen_continue(const ContinueStmt* stmt, FunctionContext& func) {
	if (func.loops.empty()) {
		error_at("'continue' outside of loop", stmt);
	}

	func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(func.loops.back().continue_label));
}

void CodeGenerator::gen_expr_stmt(const ExprStmt* stmt, FunctionContext& func) {
	int reg = gen_expr(stmt->expression.get(), func);
	free_register(func, reg);
}

int CodeGenerator::gen_expr(const Expr* expr, FunctionContext& func) {
	if (auto* lit = dynamic_cast<const LiteralExpr*>(expr)) {
		return gen_literal(lit, func);
	} else if (auto* var = dynamic_cast<const VariableExpr*>(expr)) {
		return gen_variable(var, func);
	} else if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
		return gen_binary(bin, func);
	} else if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
		return gen_unary(un, func);
	} else if (auto* await_expr = dynamic_cast<const AwaitExpr*>(expr)) {
		return gen_await(await_expr, func);
	} else if (auto* ternary = dynamic_cast<const TernaryExpr*>(expr)) {
		return gen_ternary(ternary, func);
	} else if (auto* type_test = dynamic_cast<const TypeTestExpr*>(expr)) {
		return gen_type_test(type_test, func);
	} else if (auto* cast = dynamic_cast<const CastExpr*>(expr)) {
		return gen_cast(cast, func);
	} else if (auto* call = dynamic_cast<const CallExpr*>(expr)) {
		return gen_call(call, func);
	} else if (auto* member = dynamic_cast<const MemberCallExpr*>(expr)) {
		return gen_member_call(member, func);
	} else if (auto* index = dynamic_cast<const IndexExpr*>(expr)) {
		return gen_index(index, func);
	} else if (auto* array_lit = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
		return gen_array_literal(array_lit, func);
	} else if (auto* dict_lit = dynamic_cast<const DictionaryLiteralExpr*>(expr)) {
		return gen_dictionary_literal(dict_lit, func);
	} else if (auto* lambda = dynamic_cast<const LambdaExpr*>(expr)) {
		return gen_lambda(lambda, func);
	} else {
		// Every expression kind must be lowered above; a default register would silently mis-compile.
		error_at("This kind of expression is not supported by the compiler yet", expr);
	}
}

void CodeGenerator::gen_dictionary_keys_for_iteration(int iterable_reg, FunctionContext& func) {
	// GET_KEYS: result in a2, not a3 (no key argument).
	constexpr int64_t DICT_OP_GET_KEYS = 4;

	auto emit_get_keys = [&]() {
		int keys_reg = alloc_register(func);
		IRInstruction keys(IROpcode::CALL_SYSCALL);
		keys.operands.push_back(IRValue::reg(keys_reg));
		keys.operands.push_back(IRValue::imm(ECALL_DICTIONARY_OPS));
		keys.operands.push_back(IRValue::imm(DICT_OP_GET_KEYS));
		keys.operands.push_back(IRValue::reg(iterable_reg));
		keys.type_hint = Variant::ARRAY;
		func.ir.instructions.push_back(keys);
		func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(iterable_reg),
			IRValue::reg(keys_reg));
		set_register_type(func, iterable_reg, Variant::ARRAY);
		free_register(func, keys_reg);
	};

	const IRInstruction::TypeHint known = get_register_type(func, iterable_reg);
	if (known == Variant::DICTIONARY) {
		// Known Dictionary: no type test.
		emit_get_keys();
		return;
	}
	if (known != IRInstruction::TypeHint_NONE) {
		// Known to be another type, so not a Dictionary: nothing to do.
		return;
	}

	// Unknown type: one type-tag test per loop, at run time.
	const std::string skip_label = make_label("for_not_a_dict");
	int is_dict_reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(is_dict_reg),
		IRValue::reg(iterable_reg), IRValue::imm(static_cast<int64_t>(Variant::DICTIONARY)));
	set_register_type(func, is_dict_reg, Variant::BOOL);
	emit_conditional_branch(IROpcode::BRANCH_ZERO, is_dict_reg, skip_label, func);
	free_register(func, is_dict_reg);

	emit_get_keys();

	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(skip_label));
	// Join: only one path made it an Array, so type is unknown.
	set_register_type(func, iterable_reg, IRInstruction::TypeHint_NONE);
}

// Color8(r, g, b, a = 255): integer components divided by 255.
int CodeGenerator::gen_color8(const CallExpr* expr, FunctionContext& func) {
	if (expr->arguments.size() < 3 || expr->arguments.size() > 4) {
		error_at("Color8() takes 3 or 4 arguments, got " +
			std::to_string(expr->arguments.size()), expr);
	}

	int scale_reg = gen_float_immediate(255.0, func);
	std::vector<int> components;
	for (const auto& argument : expr->arguments) {
		int value_reg = gen_expr(argument.get(), func);
		int scaled_reg = alloc_register(func);
		// No type hint: int or float argument, division by float yields float.
		func.ir.instructions.emplace_back(IROpcode::DIV, IRValue::reg(scaled_reg),
			IRValue::reg(value_reg), IRValue::reg(scale_reg));
		set_register_type(func, scaled_reg, Variant::FLOAT);
		free_register(func, value_reg);
		components.push_back(scaled_reg);
	}
	if (components.size() == 3) {
		components.push_back(gen_float_immediate(1.0, func));
	}

	int result_reg = gen_inline_constructor("Color", components, func, expr);
	for (int reg : components) {
		free_register(func, reg);
	}
	free_register(func, scale_reg);
	return result_reg;
}

// range() as a value: builds the array element-by-element via ARRAY_APPEND.
int CodeGenerator::gen_range(const CallExpr* expr, FunctionContext& func) {
	if (expr->arguments.empty() || expr->arguments.size() > 3) {
		error_at("range() takes 1, 2, or 3 arguments, got " +
			std::to_string(expr->arguments.size()), expr);
	}

	int start_reg = -1;
	int end_reg = -1;
	int step_reg = -1;
	if (expr->arguments.size() == 1) {
		start_reg = gen_int_immediate(0, func);
		end_reg = gen_expr(expr->arguments[0].get(), func);
		step_reg = gen_int_immediate(1, func);
	} else {
		start_reg = gen_expr(expr->arguments[0].get(), func);
		end_reg = gen_expr(expr->arguments[1].get(), func);
		step_reg = expr->arguments.size() == 3
			? gen_expr(expr->arguments[2].get(), func)
			: gen_int_immediate(1, func);
	}

	int result_reg = alloc_register(func);
	IRInstruction make(IROpcode::MAKE_ARRAY, IRValue::reg(result_reg), IRValue::imm(0));
	make.type_hint = Variant::ARRAY;
	func.ir.instructions.push_back(make);
	set_register_type(func, result_reg, Variant::ARRAY);

	// Zero step: empty array.
	const std::string loop_label = make_label("range_loop");
	const std::string body_label = make_label("range_body");
	const std::string down_label = make_label("range_down");
	const std::string end_label = make_label("range_end");

	// INT hint only when all three bounds are known integers.
	const bool integral = get_register_type(func, start_reg) == Variant::INT &&
		get_register_type(func, end_reg) == Variant::INT &&
		get_register_type(func, step_reg) == Variant::INT;
	const IRInstruction::TypeHint bound_hint =
		integral ? IRInstruction::TypeHint(Variant::INT) : IRInstruction::TypeHint_NONE;

	int value_reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(value_reg),
		IRValue::reg(start_reg));
	if (integral) {
		set_register_type(func, value_reg, Variant::INT);
	}

	int zero_reg = gen_int_immediate(0, func);
	int cond_reg = alloc_register(func);

	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(loop_label));

	// Run-time step sign test: determines loop direction per iteration.
	auto& sign_cmp = func.ir.instructions.emplace_back(IROpcode::CMP_GT, IRValue::reg(cond_reg),
		IRValue::reg(step_reg), IRValue::reg(zero_reg));
	sign_cmp.type_hint = bound_hint;
	set_register_type(func, cond_reg, Variant::BOOL);
	emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, down_label, func);

	auto& up_cmp = func.ir.instructions.emplace_back(IROpcode::CMP_LT, IRValue::reg(cond_reg),
		IRValue::reg(value_reg), IRValue::reg(end_reg));
	up_cmp.type_hint = bound_hint;
	emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, cond_reg, body_label, func);
	func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(end_label));

	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(down_label));
	// Step zero fails both comparisons: empty array.
	auto& down_cmp = func.ir.instructions.emplace_back(IROpcode::CMP_LT, IRValue::reg(cond_reg),
		IRValue::reg(step_reg), IRValue::reg(zero_reg));
	down_cmp.type_hint = bound_hint;
	emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, end_label, func);
	auto& down_bound = func.ir.instructions.emplace_back(IROpcode::CMP_GT, IRValue::reg(cond_reg),
		IRValue::reg(value_reg), IRValue::reg(end_reg));
	down_bound.type_hint = bound_hint;
	emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, end_label, func);

	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(body_label));
	int appended_reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::ARRAY_APPEND, IRValue::reg(appended_reg),
		IRValue::reg(result_reg), IRValue::reg(value_reg));
	free_register(func, appended_reg);

	int next_reg = alloc_register(func);
	auto& advance = func.ir.instructions.emplace_back(IROpcode::ADD, IRValue::reg(next_reg),
		IRValue::reg(value_reg), IRValue::reg(step_reg));
	advance.type_hint = bound_hint;
	if (integral) {
		set_register_type(func, next_reg, Variant::INT);
	}
	func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(value_reg),
		IRValue::reg(next_reg));
	free_register(func, next_reg);
	func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(loop_label));

	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end_label));

	free_register(func, cond_reg);
	free_register(func, zero_reg);
	free_register(func, value_reg);
	free_register(func, start_reg);
	free_register(func, end_reg);
	free_register(func, step_reg);
	return result_reg;
}

// get_node() with a compile-time path. "." resolves to the attached node.
int CodeGenerator::gen_get_node(const std::string& path, FunctionContext& func) {
	int result_reg = alloc_register(func);
	IRInstruction instr(IROpcode::GET_NODE, IRValue::reg(result_reg), ir_str(path));
	instr.type_hint = Variant::OBJECT;
	func.ir.instructions.push_back(instr);
	set_register_type(func, result_reg, Variant::OBJECT);
	return result_reg;
}

// Compile-time string path, or nullptr. A shadowing local returns nullptr.
const std::string* CodeGenerator::constant_string(const Expr* expr, FunctionContext& func) {
	if (auto* literal = dynamic_cast<const LiteralExpr*>(expr)) {
		if (literal->lit_type == LiteralExpr::Type::STRING) {
			return &std::get<std::string>(literal->value);
		}
		return nullptr;
	}
	auto* var = dynamic_cast<const VariableExpr*>(expr);
	if (var == nullptr || find_variable(func, var->name) != nullptr) {
		return nullptr;
	}
	auto it = m_global_const_values.find(var->name);
	if (it == m_global_const_values.end() || !it->second.is_const ||
		it->second.init_type != IRGlobalVar::InitType::STRING) {
		return nullptr;
	}
	return &std::get<std::string>(it->second.init_value);
}

// Compile-time path: characters embedded in the instruction, no String Variant.
int CodeGenerator::gen_load_resource(const std::string& path, FunctionContext& func) {
	int result_reg = alloc_register(func);
	IRInstruction instr(IROpcode::LOAD_RESOURCE, IRValue::reg(result_reg),
		ir_str(resolve_resource_path(path)));
	instr.type_hint = Variant::OBJECT;
	func.ir.instructions.push_back(instr);
	set_register_type(func, result_reg, Variant::OBJECT);
	return result_reg;
}

std::string CodeGenerator::resolve_resource_path(const std::string& path) const {
	if (path.empty() || path.find("://") != std::string::npos || path.front() == '/') {
		return path;
	}

	std::string source = m_source_path;
	if (size_t(m_current_chain_link) < m_chain.paths.size() &&
		!m_chain.paths[size_t(m_current_chain_link)].empty()) {
		source = m_chain.paths[size_t(m_current_chain_link)];
	}
	const size_t subresource = source.find("::");
	if (subresource != std::string::npos) {
		source.erase(subresource);
	}
	const size_t slash = source.rfind('/');
	if (slash == std::string::npos) {
		return path;
	}

	const std::string joined = source.substr(0, slash + 1) + path;
	const size_t scheme = joined.find("://");
	const std::string prefix = scheme == std::string::npos
		? std::string()
		: joined.substr(0, scheme + 3);
	const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
	std::vector<std::string> parts;
	for (size_t at = start; at <= joined.size();) {
		const size_t next = joined.find('/', at);
		const std::string part = joined.substr(at,
			next == std::string::npos ? std::string::npos : next - at);
		if (part == "..") {
			if (!parts.empty()) {
				parts.pop_back();
			}
		} else if (!part.empty() && part != ".") {
			parts.push_back(part);
		}
		if (next == std::string::npos) {
			break;
		}
		at = next + 1;
	}

	std::string resolved = prefix;
	for (size_t i = 0; i < parts.size(); i++) {
		if (i > 0) {
			resolved += '/';
		}
		resolved += parts[i];
	}
	return resolved;
}

int CodeGenerator::gen_int_immediate(int64_t value, FunctionContext& func) {
	int reg = alloc_register(func);
	IRInstruction instr(IROpcode::LOAD_IMM, IRValue::reg(reg), IRValue::imm(value));
	instr.type_hint = Variant::INT;
	func.ir.instructions.push_back(instr);
	set_register_type(func, reg, Variant::INT);
	return reg;
}

int CodeGenerator::gen_enum_member(const EnumDecl::Member& member, FunctionContext& func) {
	if (member.value_expr == nullptr) {
		return gen_int_immediate(member.value, func);
	}
	int reg = gen_expr(member.value_expr, func);
	if (member.value == 0) {
		return reg;
	}
	int offset_reg = gen_int_immediate(member.value, func);
	int result_reg = alloc_register(func);
	auto& add = func.ir.instructions.emplace_back(IROpcode::ADD, IRValue::reg(result_reg),
		IRValue::reg(reg), IRValue::reg(offset_reg));
	add.type_hint = Variant::INT;
	set_register_type(func, result_reg, Variant::INT);
	free_register(func, offset_reg);
	free_register(func, reg);
	return result_reg;
}

int CodeGenerator::gen_enum_dictionary(const EnumDecl& decl, FunctionContext& func) {
	std::vector<int> key_regs;
	std::vector<int> value_regs;
	key_regs.reserve(decl.members.size());
	value_regs.reserve(decl.members.size());
	for (const auto& member : decl.members) {
		key_regs.push_back(gen_string_value(member.name, func));
		value_regs.push_back(gen_enum_member(member, func));
	}

	const int result_reg = alloc_register(func);
	IRInstruction make(IROpcode::MAKE_DICTIONARY);
	make.operands.push_back(IRValue::reg(result_reg));
	make.operands.push_back(IRValue::imm(static_cast<int>(key_regs.size())));
	for (size_t i = 0; i < key_regs.size(); i++) {
		make.operands.push_back(IRValue::reg(key_regs[i]));
		make.operands.push_back(IRValue::reg(value_regs[i]));
	}
	make.type_hint = Variant::DICTIONARY;
	func.ir.instructions.push_back(make);
	set_register_type(func, result_reg, Variant::DICTIONARY);

	for (int reg : key_regs) {
		free_register(func, reg);
	}
	for (int reg : value_regs) {
		free_register(func, reg);
	}
	return result_reg;
}

int CodeGenerator::gen_float_immediate(double value, FunctionContext& func) {
	int reg = alloc_register(func);
	IRInstruction instr(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(reg), IRValue::fimm(value));
	instr.type_hint = Variant::FLOAT;
	func.ir.instructions.push_back(instr);
	set_register_type(func, reg, Variant::FLOAT);
	return reg;
}

int CodeGenerator::gen_literal(const LiteralExpr* expr, FunctionContext& func) {
	int reg = alloc_register(func);

	switch (expr->lit_type) {
		case LiteralExpr::Type::INTEGER: {
			IRInstruction instr(IROpcode::LOAD_IMM, IRValue::reg(reg),
			                    IRValue::imm(std::get<int64_t>(expr->value)));
			instr.type_hint = Variant::INT;
			func.ir.instructions.push_back(instr);
			set_register_type(func, reg, Variant::INT);
			break;
		}

		case LiteralExpr::Type::FLOAT: {
			// Float literals are always 64-bit doubles in GDScript
			double d = std::get<double>(expr->value);
			IRInstruction instr(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(reg), IRValue::fimm(d));
			instr.type_hint = Variant::FLOAT;
			func.ir.instructions.push_back(instr);
			set_register_type(func, reg, Variant::FLOAT);
			break;
		}

		case LiteralExpr::Type::BOOL: {
			IRInstruction instr(IROpcode::LOAD_BOOL, IRValue::reg(reg),
			                    IRValue::imm(std::get<bool>(expr->value) ? 1 : 0));
			instr.type_hint = Variant::BOOL;
			func.ir.instructions.push_back(instr);
			set_register_type(func, reg, Variant::BOOL);
			break;
		}

		case LiteralExpr::Type::STRING: {
			const int str_idx = add_string_constant(std::get<std::string>(expr->value));
			// Same string data; Variant type distinguishes &"" from ^"".
			const IRInstruction::TypeHint type =
				expr->string_type == LiteralExpr::StringType::STRING_NAME ? Variant::STRING_NAME :
				expr->string_type == LiteralExpr::StringType::NODE_PATH ? Variant::NODE_PATH :
				Variant::STRING;

			IRInstruction instr = type == Variant::STRING
				? IRInstruction(IROpcode::LOAD_STRING, IRValue::reg(reg), IRValue::imm(str_idx))
				: IRInstruction(IROpcode::LOAD_STRING_AS, IRValue::reg(reg), IRValue::imm(str_idx),
					IRValue::imm(static_cast<int64_t>(type)));
			instr.type_hint = type;
			func.ir.instructions.push_back(instr);
			set_register_type(func, reg, type);
			break;
		}

		case LiteralExpr::Type::NULL_VAL: {
			IRInstruction instr(IROpcode::LOAD_NIL, IRValue::reg(reg));
			instr.type_hint = Variant::NIL;
			func.ir.instructions.push_back(instr);
			set_register_type(func, reg, Variant::NIL);
			break;
		}
	}

	return reg;
}

int CodeGenerator::gen_variable(const VariableExpr* expr, FunctionContext& func,
	VariableOrigin* origin)
{
	// Locals shadow globals; 'self' is not declarable, handled below.
	if (Variable* local = find_variable(func, expr->name)) {
		int new_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(new_reg), IRValue::reg(local->register_num));

		IRInstruction::TypeHint type = get_register_type(func, local->register_num);
		if (type != IRInstruction::TypeHint_NONE) {
			set_register_type(func, new_reg, type);
		}
		if (func.reclassifiable_registers.count(local->register_num) != 0) {
			func.reclassifiable_registers.insert(new_reg);
		}
		if (func.string_character_registers.count(local->register_num) != 0) {
			func.string_character_registers.insert(new_reg);
		}
		if (func.codepoint_value_registers.count(local->register_num) != 0) {
			func.codepoint_value_registers.insert(new_reg);
		}
		// Dictionary handle: copy refers to the same fields, so propagate struct.
		set_register_struct(func, new_reg, get_register_struct(func, local->register_num));
		if (auto it = func.register_traits.find(local->register_num);
			it != func.register_traits.end()) {
			func.register_traits[new_reg] = it->second;
		}
		if (auto it = func.declared_traits.find(local->register_num);
			it != func.declared_traits.end()) {
			func.declared_traits[new_reg] = it->second;
		}
		if (func.trait_only_registers.count(local->register_num)) {
			func.trait_only_registers.insert(new_reg);
		}
		if (auto it = func.array_element_structs.find(local->register_num);
			it != func.array_element_structs.end()) {
			func.array_element_structs[new_reg] = it->second;
		}
		if (auto it = func.dictionary_value_structs.find(local->register_num);
			it != func.dictionary_value_structs.end()) {
			func.dictionary_value_structs[new_reg] = it->second;
		}
		if (auto it = func.array_element_traits.find(local->register_num);
			it != func.array_element_traits.end()) func.array_element_traits[new_reg] = it->second;
		if (auto it = func.dictionary_value_traits.find(local->register_num);
			it != func.dictionary_value_traits.end()) func.dictionary_value_traits[new_reg] = it->second;
		// The copy holds a value the variable's declared union already vouched for.
		if (auto it = func.declared_sets.find(local->register_num);
			it != func.declared_sets.end()) {
			func.declared_sets[new_reg] = it->second;
		}
		return new_reg;
	}

	if (int self_reg = class_field_self(expr->name, func); self_reg >= 0) {
		if (origin != nullptr) {
			origin->container_reg = self_reg;
			origin->borrowed = true;
		}
		return gen_member_read(self_reg, expr->name, func, expr);
	}

	// A field with no instance to read it from: a static method saw it.
	if (m_current_class != nullptr && find_variable(func, "self") == nullptr
		&& find_struct_field(*m_current_class, expr->name) != nullptr) {
		error_at("'" + expr->name + "' is one per instance, and a 'static func' has none", expr,
			"Pass the instance as an argument, or make '" + expr->name + "' a 'const'");
	}

	// A class constant shadows a file-level one of the same name, as it does in
	// GDScript. Reachable from a static method too: it needs no instance. A field
	// default is generated at the new() site, so the class it belongs to is the
	// one being constructed, not the one whose body encloses the call.
	{
		const StructDecl* enclosing = !m_struct_default_stack.empty()
			? m_struct_default_stack.back()
			: m_current_class;
		if (enclosing != nullptr && enclosing->is_class) {
			if (int reg = gen_class_constant(*enclosing, expr->name, func); reg >= 0) {
				return reg;
			}
		}
	}

	// Declared signal; locals shadow it (checked above).
	if (find_signal(expr->name) != nullptr) {
		return gen_signal_value(expr->name, func, expr);
	}

	// Unnamed enum member: compile-time integer. Local of same name shadows above.
	if (auto member = m_enum_members.find(expr->name); member != m_enum_members.end()) {
		return gen_enum_member(*member->second, func);
	}

	if (auto found = m_enums.find(expr->name); found != m_enums.end()) {
		return gen_enum_dictionary(*found->second, func);
	}

	if (is_global_class(expr->name)) {
		return gen_global_class_get(expr->name, func);
	}

	if (is_autoload(expr->name)) {
		return gen_global_class_get(expr->name, func);
	}

	if (expr->name == "self") {
		// In a class method `self` is a parameter, declared above. Reaching here
		// from inside one means the method is static, and there is no instance;
		// answering the owner node instead would be a different object entirely.
		if (m_current_class != nullptr || m_in_static_function) {
			error_at("'self' is the instance, and a 'static func' runs without one", expr);
		}
		return gen_get_node(".", func);
	}

	// Folded const: immediate instead of LOAD_GLOBAL (carries type for match/arithmetic).
	if (int const_reg = gen_const_global_value(expr->name, func); const_reg >= 0) {
		return const_reg;
	}

	if (is_global_variable(expr->name)) {
		size_t global_idx = m_global_variables.at(expr->name);
		// Forward reference: global not yet initialized, still NIL.
		if (global_idx >= m_globals_lowered) {
			error_at("Global variable '" + expr->name + "' is used in the initializer of a global "
				"declared before it", expr,
				"Move the declaration of '" + expr->name + "' above that global");
		}
		if (!m_members_in_scope && global_idx < m_global_is_member.size()
			&& m_global_is_member[global_idx]) {
			error_at("Cannot read the member variable '" + expr->name + "' from the initializer "
				"of a 'const' or 'static var'", expr,
				"'" + expr->name + "' is one per instance and does not exist yet when a shared "
				"initializer runs. Make '" + expr->name + "' a 'static var', or move the "
				"expression into a function");
		}
		reject_static_member_access(expr->name, expr->line, expr->column);
		if (!global_getter(global_idx).empty()) {
			return gen_property_get(global_idx, func);
		}
		int result_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::LOAD_GLOBAL, IRValue::reg(result_reg), IRValue::imm(global_idx));
		IRInstruction::TypeHint global_type = m_global_types[global_idx];
		if (global_type == IRInstruction::TypeHint_NONE) {
			if (auto narrowed = func.narrowed_global_types.find(global_idx);
				narrowed != func.narrowed_global_types.end()) {
				global_type = narrowed->second;
			}
		}
		if (const StructDecl* structure = m_global_structs[global_idx]; structure != nullptr) {
			set_register_struct(func, result_reg, structure);
			if (structure->is_class && global_type != Variant::DICTIONARY) {
				set_register_type(func, result_reg, IRInstruction::TypeHint_NONE);
			}
		}
		if (m_global_traits[global_idx] != nullptr &&
			!m_global_sets[global_idx].contains(Variant::NIL)) {
			add_register_trait(func, result_reg, m_global_traits[global_idx]);
			func.trait_only_registers.insert(result_reg);
		}
		if (m_global_array_element_structs[global_idx] != nullptr) {
			func.array_element_structs[result_reg] = m_global_array_element_structs[global_idx];
		}
		if (m_global_dictionary_value_structs[global_idx] != nullptr) {
			func.dictionary_value_structs[result_reg] =
				m_global_dictionary_value_structs[global_idx];
		}
		if (m_global_array_element_traits[global_idx] != nullptr) {
			func.array_element_traits[result_reg] =
				m_global_array_element_traits[global_idx];
		}
		if (m_global_dictionary_value_traits[global_idx] != nullptr) {
			func.dictionary_value_traits[result_reg] =
				m_global_dictionary_value_traits[global_idx];
		}
		// Propagate declared type so member access skips the run-time tag test.
		if (global_type != IRInstruction::TypeHint_NONE) {
			set_register_type(func, result_reg, global_type);
		}
		return result_reg;
	}

	if (const StructDecl* decl = find_struct(expr->name)) {
		error_at("Struct '" + decl->name + "' is a type, not a value", expr,
			"Create an instance with '" + decl->name + ".new()'");
	}
	if (const TraitDecl* iface = find_trait(expr->name)) {
		error_at("Trait '" + iface->name + "' is a type, not a value", expr,
			"Traits cannot be constructed");
	}

	// @GlobalScope constant: compile-time immediate. Checked last (local shadows).
	if (const GlobalConstant* constant = find_global_constant(expr->name)) {
		return constant->is_float
			? gen_float_immediate(constant->float_value, func)
			: gen_int_immediate(constant->int_value, func);
	}

	// Script function used as a Callable value (e.g. `pressed.connect(f)`).
	if (is_local_function(expr->name) || m_test_functions.count(expr->name)) {
		reject_test_reference(expr->name, expr);
		return gen_make_callable(expr->name, -1, func);
	}

	// Without this, an unknown script class falls through to VGET and silently answers null.
	if (global_script_class_path(expr->name) != nullptr) {
		error_at("'" + expr->name + "' is a script in another file, and none of its body is "
			"compiled into this program", expr,
			m_chain.merged()
				? "Only the scripts this one extends are merged in. Reach '" + expr->name +
					"' through an instance: '" + expr->name + ".new()'"
				: "A sandboxed program is one binary built from one file. Reach '" + expr->name +
					"' through an instance ('" + expr->name + ".new()'), or extend it");
	}

	// GDScript refuses this too: a native enum is not a Dictionary, has no
	// keys()/values(), and "cannot be used on its own". Without the check the
	// name would fall through to VGET and answer null.
	if (is_global_enum(expr->name)) {
		error_at("Global enum '" + expr->name + "' cannot be used on its own", expr,
			"Name one of its members: '" + expr->name + ".<MEMBER>'");
	}

	if (int base_reg = gen_implicit_base_load(func); base_reg >= 0) {
		int result_reg = gen_vget(base_reg, expr->name, func);
		if (origin != nullptr) {
			origin->container_reg = base_reg;
		} else {
			free_register(func, base_reg);
		}
		return result_reg;
	}

	error_at("Undefined variable: " + expr->name, expr,
		"Make sure '" + expr->name + "' is declared before use");
}

namespace {

// Collects names free in a lambda body (reads without a matching declaration).
// Nested lambdas open a new scope so a name free in the inner one propagates.
class FreeNameCollector {
public:
	std::vector<std::string> names;
	std::vector<std::string> callees;

	void collect(const FunctionDecl& decl) {
		enter(decl);
		for (const auto& stmt : decl.body) {
			visit(stmt.get());
		}
		m_scopes.pop_back();
	}

private:
	std::vector<std::unordered_set<std::string>> m_scopes;

	void enter(const FunctionDecl& decl) {
		std::unordered_set<std::string> scope;
		for (const auto& param : decl.parameters) {
			scope.insert(param.name);
		}
		for (const auto& stmt : decl.body) {
			declared(stmt.get(), scope);
		}
		m_scopes.push_back(std::move(scope));
	}

	bool bound(const std::string& name) const {
		for (const auto& scope : m_scopes) {
			if (scope.count(name)) {
				return true;
			}
		}
		return false;
	}

	void use(const std::string& name, std::vector<std::string>& into) {
		if (bound(name)) {
			return;
		}
		for (const auto& seen : into) {
			if (seen == name) {
				return;
			}
		}
		into.push_back(name);
	}

	static void declared(const Stmt* stmt, std::unordered_set<std::string>& scope) {
		if (auto* var = dynamic_cast<const VarDeclStmt*>(stmt)) {
			scope.insert(var->name);
		} else if (auto* if_stmt = dynamic_cast<const IfStmt*>(stmt)) {
			for (const auto& s : if_stmt->then_branch) declared(s.get(), scope);
			for (const auto& s : if_stmt->else_branch) declared(s.get(), scope);
		} else if (auto* while_stmt = dynamic_cast<const WhileStmt*>(stmt)) {
			for (const auto& s : while_stmt->body) declared(s.get(), scope);
		} else if (auto* for_stmt = dynamic_cast<const ForStmt*>(stmt)) {
			scope.insert(for_stmt->variable);
			for (const auto& s : for_stmt->body) declared(s.get(), scope);
		} else if (auto* match_stmt = dynamic_cast<const MatchStmt*>(stmt)) {
			for (const auto& branch : match_stmt->branches) {
				for (const auto& pattern : branch.patterns) {
					declared_in_pattern(*pattern, scope);
				}
				for (const auto& s : branch.body) declared(s.get(), scope);
			}
		}
	}

	static void declared_in_pattern(const MatchPattern& pattern, std::unordered_set<std::string>& scope) {
		if (pattern.kind == MatchPattern::Kind::BIND) {
			scope.insert(pattern.name);
		}
		for (const auto& element : pattern.elements) {
			declared_in_pattern(*element, scope);
		}
		for (const auto& entry : pattern.entries) {
			if (entry.value) {
				declared_in_pattern(*entry.value, scope);
			}
		}
	}

	void visit(const Stmt* stmt) {
		if (stmt == nullptr) {
			return;
		}
		if (auto* expr_stmt = dynamic_cast<const ExprStmt*>(stmt)) {
			visit(expr_stmt->expression.get());
		} else if (auto* var = dynamic_cast<const VarDeclStmt*>(stmt)) {
			visit(var->initializer.get());
		} else if (auto* assign = dynamic_cast<const AssignStmt*>(stmt)) {
			// Assignment target is a use: the lifted function needs a local for it.
			if (!assign->name.empty()) {
				use(assign->name, names);
			}
			visit(assign->target.get());
			visit(assign->value.get());
		} else if (auto* ret = dynamic_cast<const ReturnStmt*>(stmt)) {
			visit(ret->value.get());
		} else if (auto* if_stmt = dynamic_cast<const IfStmt*>(stmt)) {
			if (if_stmt->binding) {
				// The initializer is outside the new binding's scope, which matters
				// for the useful shadowing form `if var value = value:` in a lambda.
				visit(if_stmt->binding->initializer.get());
				const bool inserted = m_scopes.back().insert(if_stmt->binding->name).second;
				for (const auto& s : if_stmt->then_branch) visit(s.get());
				if (inserted) m_scopes.back().erase(if_stmt->binding->name);
			} else {
				visit(if_stmt->condition.get());
				for (const auto& s : if_stmt->then_branch) visit(s.get());
			}
			for (const auto& s : if_stmt->else_branch) visit(s.get());
		} else if (auto* while_stmt = dynamic_cast<const WhileStmt*>(stmt)) {
			visit(while_stmt->condition.get());
			for (const auto& s : while_stmt->body) visit(s.get());
		} else if (auto* for_stmt = dynamic_cast<const ForStmt*>(stmt)) {
			visit(for_stmt->iterable.get());
			for (const auto& s : for_stmt->body) visit(s.get());
		} else if (auto* match_stmt = dynamic_cast<const MatchStmt*>(stmt)) {
			visit(match_stmt->subject.get());
			for (const auto& branch : match_stmt->branches) {
				for (const auto& pattern : branch.patterns) {
					visit_pattern(*pattern);
				}
				visit(branch.guard.get());
				for (const auto& s : branch.body) visit(s.get());
			}
		}
	}

	void visit_pattern(const MatchPattern& pattern) {
		visit(pattern.value.get());
		for (const auto& element : pattern.elements) {
			visit_pattern(*element);
		}
		for (const auto& entry : pattern.entries) {
			visit(entry.key.get());
			if (entry.value) {
				visit_pattern(*entry.value);
			}
		}
	}

	void visit(const Expr* expr) {
		if (expr == nullptr) {
			return;
		}
		if (auto* var = dynamic_cast<const VariableExpr*>(expr)) {
			use(var->name, names);
		} else if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
			visit(bin->left.get());
			visit(bin->right.get());
		} else if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
			visit(un->operand.get());
		} else if (auto* await_expr = dynamic_cast<const AwaitExpr*>(expr)) {
			visit(await_expr->operand.get());
		} else if (auto* ternary = dynamic_cast<const TernaryExpr*>(expr)) {
			visit(ternary->condition.get());
			visit(ternary->true_value.get());
			visit(ternary->false_value.get());
		} else if (auto* type_test = dynamic_cast<const TypeTestExpr*>(expr)) {
			visit(type_test->value.get());
		} else if (auto* cast = dynamic_cast<const CastExpr*>(expr)) {
			visit(cast->value.get());
		} else if (auto* call = dynamic_cast<const CallExpr*>(expr)) {
			use(call->function_name, callees);
			for (const auto& arg : call->arguments) visit(arg.get());
		} else if (auto* member = dynamic_cast<const MemberCallExpr*>(expr)) {
			visit(member->object.get());
			for (const auto& arg : member->arguments) visit(arg.get());
		} else if (auto* index = dynamic_cast<const IndexExpr*>(expr)) {
			visit(index->object.get());
			visit(index->index.get());
		} else if (auto* array_lit = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
			for (const auto& element : array_lit->elements) visit(element.get());
		} else if (auto* dict_lit = dynamic_cast<const DictionaryLiteralExpr*>(expr)) {
			for (const auto& entry : dict_lit->elements) {
				visit(entry.first.get());
				visit(entry.second.get());
			}
		} else if (auto* lambda = dynamic_cast<const LambdaExpr*>(expr)) {
			enter(*lambda->decl);
			for (const auto& stmt : lambda->decl->body) {
				visit(stmt.get());
			}
			m_scopes.pop_back();
		}
	}
};

} // namespace

std::vector<std::string> CodeGenerator::collect_captures(const FunctionDecl& decl,
		FunctionContext& enclosing) const {
	FreeNameCollector collector;
	collector.collect(decl);

	std::vector<std::string> captures;
	for (const auto& name : collector.names) {
		// Only locals are captured; globals are read live via LOAD_GLOBAL.
		if (const_cast<CodeGenerator*>(this)->find_variable(enclosing, name) != nullptr) {
			captures.push_back(name);
		}
	}
	for (const auto& name : collector.callees) {
		// Script/global functions resolve by name; only capture unresolved callees.
		if (is_local_function(name) || is_global_function(name)) {
			continue;
		}
		if (const_cast<CodeGenerator*>(this)->find_variable(enclosing, name) == nullptr) {
			continue;
		}
		if (std::find(captures.begin(), captures.end(), name) == captures.end()) {
			captures.push_back(name);
		}
	}

	// Capture `self` when the lambda body references a class field or method.
	if (m_current_class != nullptr) {
		bool needs_self = false;
		for (const auto& name : collector.names) {
			if (find_struct_field(*m_current_class, name) != nullptr) {
				needs_self = true;
				break;
			}
		}
		for (const auto& name : collector.callees) {
			if (needs_self) {
				break;
			}
			needs_self = find_class_method(*m_current_class, name) != nullptr;
		}
		if (needs_self &&
			const_cast<CodeGenerator*>(this)->find_variable(enclosing, "self") != nullptr &&
			std::find(captures.begin(), captures.end(), "self") == captures.end()) {
			captures.push_back("self");
		}
	}
	return captures;
}

int CodeGenerator::gen_make_callable(const std::string& function_name, int bound_reg,
		FunctionContext& func) {
	int argument_reg = bound_reg;
	if (argument_reg < 0) {
		argument_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::LOAD_NIL, IRValue::reg(argument_reg));
	}

	int result_reg = alloc_register(func);
	IRInstruction instr(IROpcode::MAKE_CALLABLE, IRValue::reg(result_reg),
		ir_str(function_name), IRValue::reg(argument_reg));
	instr.type_hint = Variant::CALLABLE;
	func.ir.instructions.push_back(instr);
	set_register_type(func, result_reg, Variant::CALLABLE);

	if (bound_reg < 0) {
		free_register(func, argument_reg);
	}
	return result_reg;
}

int CodeGenerator::gen_callable_constructor(const CallExpr* expr, FunctionContext& func) {
	reject_named_arguments(*expr, "'Callable'", expr);
	if (expr->arguments.empty()) {
		return gen_make_callable("", -1, func);
	}
	if (expr->arguments.size() == 1) {
		int value_reg = gen_expr(expr->arguments[0].get(), func);
		const IRInstruction::TypeHint value_type = get_register_type(func, value_reg);
		if (value_type == Variant::CALLABLE) {
			return value_reg;
		}
		if (value_type != IRInstruction::TypeHint_NONE) {
			free_register(func, value_reg);
			error_at("Callable(value) needs a Callable argument", expr);
		}
		int result = gen_host_constructor_typed("Callable", Variant::CALLABLE,
			{ value_reg }, func, expr);
		free_register(func, value_reg);
		return result;
	}
	if (expr->arguments.size() != 2) {
		error_at("Callable() takes 0, 1 or 2 arguments, got " +
			std::to_string(expr->arguments.size()), expr);
	}

	auto* object = dynamic_cast<const VariableExpr*>(expr->arguments[0].get());
	const bool names_script_self = object != nullptr && object->name == "self" &&
		find_variable(func, object->name) == nullptr;
	if (names_script_self) {
		const std::string* name = constant_string(expr->arguments[1].get(), func);
		if (name == nullptr) {
			error_at("The method of Callable(self, method) must be a compile-time string", expr,
				"The guest resolves its own function address while it compiles");
		}
		if (!is_local_function(*name)) {
			error_at("This program declares no function named '" + *name + "'", expr,
				"Callable(self, \"" + *name + "\") would never be valid");
		}
		return gen_make_callable(*name, -1, func);
	}

	if (m_restricted) {
		error_at("Callable(object, method) is not supported in a restricted Sandbox", expr,
			"Only an unrestricted script may keep and invoke a method on another Object");
	}

	std::vector<int> arg_regs;
	arg_regs.reserve(2);
	for (const auto& argument : expr->arguments) {
		arg_regs.push_back(gen_expr(argument.get(), func));
	}

	const IRInstruction::TypeHint object_type = get_register_type(func, arg_regs[0]);
	if (object_type != IRInstruction::TypeHint_NONE && object_type != Variant::OBJECT) {
		for (int reg : arg_regs) {
			free_register(func, reg);
		}
		error_at("Callable() needs an Object as its first argument", expr);
	}

	int result = gen_host_constructor_typed("Callable", Variant::CALLABLE,
		arg_regs, func, expr);
	for (int reg : arg_regs) {
		free_register(func, reg);
	}
	return result;
}

int CodeGenerator::gen_lambda(const LambdaExpr* expr, FunctionContext& func) {
	const FunctionDecl& decl = *expr->decl;

	const std::vector<std::string> captures = collect_captures(decl, func);

	const size_t slots = decl.parameters.size() + (captures.empty() ? 0 : 1);
	if (slots > IRFunction::MAX_PARAMETERS) {
		error_at("This lambda needs " + std::to_string(slots) + " parameter slots, but at most " +
			std::to_string(IRFunction::MAX_PARAMETERS) + " can be passed", expr,
			captures.empty()
				? "Pass the extra values in an Array or Dictionary instead"
				: "It captures " + std::to_string(captures.size()) +
					" name(s), which travel in one further slot");
	}

	std::string lifted_name = "@lambda_" + std::to_string(m_next_lambda++);
	if (!decl.name.empty()) {
		lifted_name += "_" + decl.name;
	}

	int bound_reg = -1;
	if (!captures.empty()) {
		std::vector<int> capture_regs;
		for (const auto& name : captures) {
			Variable* variable = find_variable(func, name);
			capture_regs.push_back(variable->register_num);
		}

		bound_reg = alloc_register(func);
		IRInstruction make_array(IROpcode::MAKE_ARRAY, IRValue::reg(bound_reg),
			IRValue::imm(static_cast<int64_t>(capture_regs.size())));
		for (int reg : capture_regs) {
			make_array.operands.push_back(IRValue::reg(reg));
		}
		make_array.type_hint = Variant::ARRAY;
		func.ir.instructions.push_back(make_array);
		set_register_type(func, bound_reg, Variant::ARRAY);
	}

	m_pending_lambdas.push_back({ &decl, lifted_name, captures, m_current_class,
		m_current_chain_link, m_current_chain_function, m_in_static_function });

	int result_reg = gen_make_callable(lifted_name, bound_reg, func);
	if (bound_reg >= 0) {
		free_register(func, bound_reg);
	}
	return result_reg;
}

IRFunction CodeGenerator::generate_lambda_function(const FunctionDecl& decl,
		const std::vector<std::string>& captures) {
	// Lambda is not an accessor scope; properties go through accessors here.
	m_direct_globals.clear();
	FunctionContext func;
	func.ir.name = decl.name.empty() ? std::string("lambda") : decl.name;
	func.ir.is_coroutine = decl.is_coroutine;
	const TypeSet return_set = type_set_from(decl.return_type, decl.line, decl.column);
	func.ir.return_type_hint = decl.return_type.is_union()
		? IRInstruction::TypeHint_NONE : single_type_from(decl.return_type);
	func.ir.return_set = decl.return_type.is_union() ? return_set.mask : 0;
	func.return_type = decl.return_type;

	push_scope(func);

	// Captures Array prepended by the host, followed by declared parameters.
	int captures_reg = -1;
	if (!captures.empty()) {
		captures_reg = alloc_register(func);
		func.ir.parameters.push_back("@captures");
		set_register_type(func, captures_reg, Variant::ARRAY);
	}

	for (const auto& param : decl.parameters) {
		func.ir.parameters.push_back(param.name);
		int reg = alloc_register(func);
		declare_variable(func, param.name, reg, false, nullptr, false, true);
		apply_declared_type(reg, param.type_hint, func);
		const TypeSet param_set = type_set_from(param.type_hint, param.line, param.column);
		func.ir.param_sets.push_back(param.type_hint.is_union() ? param_set.mask : 0);
	}
	coerce_parameters(decl.parameters, func);

	for (size_t i = 0; i < captures.size(); i++) {
		int index_reg = gen_int_immediate(static_cast<int64_t>(i), func);
		int value_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::ARRAY_GET, IRValue::reg(value_reg),
			IRValue::reg(captures_reg), IRValue::reg(index_reg));
		free_register(func, index_reg);
		declare_variable(func, captures[i], value_reg);
	}

	for (const auto& stmt : decl.body) {
		gen_stmt(stmt.get(), func);
	}

	if (func.ir.instructions.empty() ||
	    func.ir.instructions.back().opcode != IROpcode::RETURN) {
		func.ir.instructions.emplace_back(IROpcode::LOAD_NIL, IRValue::reg(0));
		func.ir.instructions.emplace_back(IROpcode::RETURN);
	}

	func.ir.max_registers = std::max(func.next_register, 1);
	pop_scope(func);

	return std::move(func.ir);
}

// `c(args)` on a variable: lowers to `c.call(args)`.
int CodeGenerator::gen_callable_variable_call(const CallExpr* expr, int callable_reg,
		std::vector<int>& arg_regs, FunctionContext& func) {
	const IRInstruction::TypeHint type = get_register_type(func, callable_reg);
	if (type != IRInstruction::TypeHint_NONE && type != Variant::CALLABLE) {
		error_at("'" + expr->function_name + "' holds " +
			std::string(variant_type_name(type)) + ", which cannot be called", expr,
			"Only a Callable can be called through a variable");
	}

	int result_reg = alloc_register(func);
	IRInstruction vcall(IROpcode::VCALL);
	vcall.operands.push_back(IRValue::reg(result_reg));
	vcall.operands.push_back(IRValue::reg(callable_reg));
	vcall.operands.push_back(ir_str("call"));
	vcall.operands.push_back(IRValue::imm(static_cast<int64_t>(arg_regs.size())));
	for (int arg_reg : arg_regs) {
		vcall.operands.push_back(IRValue::reg(arg_reg));
	}
	func.ir.instructions.push_back(vcall);

	free_register(func, callable_reg);
	for (int reg : arg_regs) {
		free_register(func, reg);
	}
	return result_reg;
}

int CodeGenerator::gen_logical(const BinaryExpr* expr, FunctionContext& func) {
	// Short-circuit: only evaluate RHS when LHS doesn't decide the result.
	const bool is_and = expr->op == BinaryExpr::Op::AND;
	const std::string end_label = make_label(is_and ? "and_end" : "or_end");
	const IROpcode short_circuit_branch = is_and ? IROpcode::BRANCH_ZERO : IROpcode::BRANCH_NOT_ZERO;

	int result_reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(result_reg),
		IRValue::imm(is_and ? 0 : 1));
	set_register_type(func, result_reg, Variant::BOOL);

	int left_reg = gen_expr(expr->left.get(), func);
	IRInstruction left_branch(short_circuit_branch, IRValue::reg(left_reg), ir_label(end_label));
	// Type hint lets backend test truthiness inline.
	left_branch.type_hint = get_register_type(func, left_reg);
	func.ir.instructions.push_back(left_branch);
	free_register(func, left_reg);

	NarrowingInfo rhs_narrowing;
	if (is_and) {
		rhs_narrowing = condition_narrowing(expr->left.get(), func);
		if (!rhs_narrowing.is_member() || !narrowing_expr_calls_out(expr->right.get())) {
			apply_narrowing(rhs_narrowing, true, func);
		} else {
			rhs_narrowing = {};
		}
	}
	int right_reg = gen_expr(expr->right.get(), func);
	restore_narrowing(rhs_narrowing, func);
	IRInstruction right_branch(short_circuit_branch, IRValue::reg(right_reg), ir_label(end_label));
	right_branch.type_hint = get_register_type(func, right_reg);
	func.ir.instructions.push_back(right_branch);
	free_register(func, right_reg);

	func.ir.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(result_reg),
		IRValue::imm(is_and ? 1 : 0));
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end_label));

	return result_reg;
}

// Emit GLOBAL_CALL to str() over `args`.
int CodeGenerator::gen_str_call(const std::vector<int>& args, FunctionContext& func) {
	if (args.size() == 1) {
		if (const StructDecl* structure = get_register_struct(func, args[0]);
			structure != nullptr && !structure->is_class) {
			return gen_struct_string(args[0], *structure, func);
		}
	}
	const int result_reg = alloc_register(func);
	IRInstruction instr(IROpcode::GLOBAL_CALL);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::imm(static_cast<int64_t>(GlobalFn::STR)));
	instr.operands.push_back(IRValue::imm(0)); // untyped: str() is a host call
	instr.operands.push_back(IRValue::imm(static_cast<int64_t>(args.size())));
	for (int reg : args) {
		instr.operands.push_back(IRValue::reg(reg));
	}
	instr.type_hint = Variant::STRING;
	func.ir.instructions.push_back(instr);
	set_register_type(func, result_reg, Variant::STRING);
	return result_reg;
}

int CodeGenerator::gen_struct_string(int value_reg, const StructDecl& decl,
	FunctionContext& func, const Expr* site)
{
	if (const StructDecl* owner = nullptr;
		const FunctionDecl* method = find_class_method(decl, "_to_string", &owner)) {
		return gen_class_method_call(decl, *method, *owner, value_reg, {}, NamedArguments{},
			func, site);
	}

	std::vector<int> parts;
	const auto literal = [&](const std::string& text) {
		int reg = alloc_register(func);
		IRInstruction load(IROpcode::LOAD_STRING, IRValue::reg(reg),
			IRValue::imm(add_string_constant(text)));
		load.type_hint = Variant::STRING;
		func.ir.instructions.push_back(std::move(load));
		set_register_type(func, reg, Variant::STRING);
		return reg;
	};
	parts.push_back(literal(decl.name + "("));
	const std::vector<const StructField*> fields = struct_fields(decl);
	for (size_t i = 0; i < fields.size(); i++) {
		parts.push_back(literal((i == 0 ? "" : ", ") + fields[i]->name + ": "));
		parts.push_back(gen_dict_get(value_reg, fields[i]->name, func));
	}
	parts.push_back(literal(")"));

	const int result = alloc_register(func);
	IRInstruction call(IROpcode::GLOBAL_CALL);
	call.operands.push_back(IRValue::reg(result));
	call.operands.push_back(IRValue::imm(static_cast<int64_t>(GlobalFn::STR)));
	call.operands.push_back(IRValue::imm(0));
	call.operands.push_back(IRValue::imm(static_cast<int64_t>(parts.size())));
	for (int reg : parts) call.operands.push_back(IRValue::reg(reg));
	call.type_hint = Variant::STRING;
	func.ir.instructions.push_back(std::move(call));
	set_register_type(func, result, Variant::STRING);
	for (int reg : parts) free_register(func, reg);
	return result;
}

// Erase the str() that produced `reg` and collect its argument registers into `args`.
// Bails if any intervening instruction reads the result, clobbers an argument, or
// crosses control flow.
bool CodeGenerator::absorb_str_call(FunctionContext& func, int reg, size_t since,
	std::vector<int>& args)
{
	std::vector<IRInstruction>& code = func.ir.instructions;
	size_t at = code.size();
	for (;;) {
		if (at <= since) {
			return false;
		}
		at--;
		if (ir_destination_register(code[at]) == reg) {
			break;
		}
	}

	const IRInstruction& call = code[at];
	if (call.opcode != IROpcode::GLOBAL_CALL || call.operands.size() < 4 ||
	    call.operands[1].type != IRValue::Type::IMMEDIATE ||
	    call.operands[1].immediate() != static_cast<int64_t>(GlobalFn::STR)) {
		return false;
	}
	if (call.operands[3].type != IRValue::Type::IMMEDIATE) {
		return false;
	}
	const size_t count = static_cast<size_t>(call.operands[3].immediate());
	if (call.operands.size() != 4 + count) {
		return false;
	}
	std::vector<int> call_args;
	for (size_t i = 0; i < count; i++) {
		if (call.operands[4 + i].type != IRValue::Type::REGISTER) {
			return false;
		}
		call_args.push_back(call.operands[4 + i].reg_index());
	}

	std::vector<int> reads;
	for (size_t i = at + 1; i < code.size(); i++) {
		const IRInstruction& later = code[i];
		if (ir_is_control_flow(later.opcode)) {
			return false;
		}
		reads.clear();
		ir_collect_read_registers(later, reads);
		if (std::find(reads.begin(), reads.end(), reg) != reads.end()) {
			return false;
		}
		const int dst = ir_destination_register(later);
		if (dst >= 0 && std::find(call_args.begin(), call_args.end(), dst) != call_args.end()) {
			return false;
		}
	}

	args.insert(args.end(), call_args.begin(), call_args.end());
	code.erase(code.begin() + at);
	return true;
}

// Fold String + str(...) chains into a single str() call. Two plain Strings
// stay on the VEVAL path. Returns -1 when nothing folds.
int CodeGenerator::gen_string_concat(const BinaryExpr* expr, FunctionContext& func,
	int& left_reg, int& right_reg)
{
	const size_t left_start = func.ir.instructions.size();
	left_reg = gen_expr(expr->left.get(), func);
	if (get_register_type(func, left_reg) != Variant::STRING) {
		return -1;
	}
	size_t right_start = func.ir.instructions.size();
	right_reg = gen_expr(expr->right.get(), func);
	if (get_register_type(func, right_reg) != Variant::STRING) {
		return -1;
	}

	std::vector<int> args;
	const bool left_folded = absorb_str_call(func, left_reg, left_start, args);
	if (!left_folded) {
		args.push_back(left_reg);
	} else {
		// Erased instruction was before right_start.
		right_start--;
	}
	std::vector<int> right_args;
	const bool right_folded = absorb_str_call(func, right_reg, right_start, right_args);
	if (!right_folded) {
		right_args.push_back(right_reg);
	}
	if (!left_folded && !right_folded) {
		return -1; // Two Strings that are already Strings: VEVAL concatenates them.
	}

	// Matches the host str() argument limit.
	constexpr size_t MAX_STR_ARGS = 63;
	if (args.size() + right_args.size() > MAX_STR_ARGS) {
		if (left_folded) {
			left_reg = gen_str_call(args, func);
		}
		if (right_folded) {
			right_reg = gen_str_call(right_args, func);
		}
		return -1;
	}

	args.insert(args.end(), right_args.begin(), right_args.end());
	return gen_str_call(args, func);
}

int CodeGenerator::gen_binary(const BinaryExpr* expr, FunctionContext& func) {
	if (expr->op == BinaryExpr::Op::AND || expr->op == BinaryExpr::Op::OR) {
		return gen_logical(expr, func);
	}

	int left_reg = -1;
	int right_reg = -1;
	if (expr->op == BinaryExpr::Op::ADD) {
		const int concatenated = gen_string_concat(expr, func, left_reg, right_reg);
		if (concatenated >= 0) {
			return concatenated;
		}
	}
	if (left_reg < 0) {
		left_reg = gen_expr(expr->left.get(), func);
	}
	if (right_reg < 0) {
		right_reg = gen_expr(expr->right.get(), func);
	}
	// Godot's `%s` formatting stringifies its right operand. Preserve the
	// declared struct surface instead of exposing the backing Dictionary.
	if (expr->op == BinaryExpr::Op::MOD) {
		if (const StructDecl* structure = get_register_struct(func, right_reg);
			structure != nullptr && !structure->is_class) {
			right_reg = gen_struct_string(right_reg, *structure, func, expr->right.get());
		}
	}
	int result_reg = alloc_register(func);

	IRInstruction::TypeHint left_type = get_register_type(func, left_reg);
	IRInstruction::TypeHint right_type = get_register_type(func, right_reg);
	if (expr->op == BinaryExpr::Op::EQ || expr->op == BinaryExpr::Op::NEQ) {
		const StructDecl* left_struct = get_register_struct(func, left_reg);
		const StructDecl* right_struct = get_register_struct(func, right_reg);
		if (left_struct != nullptr && right_struct != nullptr) {
			if (left_struct != right_struct) {
				error_at("Cannot compare struct '" + left_struct->name + "' with struct '" +
					right_struct->name + "'", expr);
			}
			left_type = right_type = Variant::DICTIONARY;
		}
	}

	// Equality with null is a tag test. Besides avoiding a host Variant
	// evaluation, this folds immediately in a narrowed branch.
	if ((expr->op == BinaryExpr::Op::EQ || expr->op == BinaryExpr::Op::NEQ) &&
		(left_type == Variant::NIL || right_type == Variant::NIL)) {
		const int value_reg = left_type == Variant::NIL ? right_reg : left_reg;
		const IRInstruction::TypeHint value_type = get_register_type(func, value_reg);
		if (value_type != IRInstruction::TypeHint_NONE) {
			const bool equal = value_type == Variant::NIL;
			func.ir.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(result_reg),
				IRValue::imm((expr->op == BinaryExpr::Op::EQ) == equal ? 1 : 0));
		} else if (expr->op == BinaryExpr::Op::EQ) {
			func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(result_reg),
				IRValue::reg(value_reg), IRValue::imm(static_cast<int64_t>(Variant::NIL)));
		} else {
			const int is_nil = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(is_nil),
				IRValue::reg(value_reg), IRValue::imm(static_cast<int64_t>(Variant::NIL)));
			set_register_type(func, is_nil, Variant::BOOL);
			IRInstruction negate(IROpcode::NOT, IRValue::reg(result_reg), IRValue::reg(is_nil));
			negate.type_hint = Variant::BOOL;
			func.ir.instructions.push_back(negate);
			free_register(func, is_nil);
		}
		set_register_type(func, result_reg, Variant::BOOL);
		free_register(func, left_reg);
		free_register(func, right_reg);
		return result_reg;
	}

	bool is_arithmetic = (expr->op == BinaryExpr::Op::ADD ||
	                      expr->op == BinaryExpr::Op::SUB ||
	                      expr->op == BinaryExpr::Op::MUL ||
	                      expr->op == BinaryExpr::Op::DIV ||
	                      expr->op == BinaryExpr::Op::MOD);
	bool is_bitwise = (expr->op == BinaryExpr::Op::BIT_AND ||
	                   expr->op == BinaryExpr::Op::BIT_OR ||
	                   expr->op == BinaryExpr::Op::BIT_XOR ||
	                   expr->op == BinaryExpr::Op::SHL ||
	                   expr->op == BinaryExpr::Op::SHR);
	bool is_comparison = (expr->op == BinaryExpr::Op::EQ ||
	                      expr->op == BinaryExpr::Op::NEQ ||
	                      expr->op == BinaryExpr::Op::LT ||
	                      expr->op == BinaryExpr::Op::LTE ||
	                      expr->op == BinaryExpr::Op::GT ||
	                      expr->op == BinaryExpr::Op::GTE);

	// Godot promotes a mixed numeric pair to FLOAT. Make that conversion
	// explicit so the backend receives a homogeneous, fully typed operation.
	if (((is_arithmetic && expr->op != BinaryExpr::Op::MOD) || is_comparison) &&
		func.reclassifiable_registers.count(left_reg) == 0 &&
		func.reclassifiable_registers.count(right_reg) == 0 &&
		((left_type == Variant::INT && right_type == Variant::FLOAT) ||
		 (left_type == Variant::FLOAT && right_type == Variant::INT)))
	{
		if (left_type == Variant::INT) {
			left_reg = coerce_to_declared_type(left_reg, Variant::FLOAT, func,
				"the left numeric operand", expr->line, expr->column);
			left_type = Variant::FLOAT;
		} else {
			right_reg = coerce_to_declared_type(right_reg, Variant::FLOAT, func,
				"the right numeric operand", expr->line, expr->column);
			right_type = Variant::FLOAT;
		}
	}

	// Float vectors and Color accept either FLOAT or INT scalars. Normalize an
	// integer scalar once, then let the backend broadcast it component-wise.
	const bool mul_or_div = expr->op == BinaryExpr::Op::MUL || expr->op == BinaryExpr::Op::DIV;
	if (mul_or_div && TypeHintUtils::is_float_vector(left_type) && right_type == Variant::INT) {
		right_reg = coerce_to_declared_type(right_reg, Variant::FLOAT, func,
			"the vector scalar", expr->line, expr->column);
		right_type = Variant::FLOAT;
	} else if (expr->op == BinaryExpr::Op::MUL && left_type == Variant::INT &&
		TypeHintUtils::is_float_vector(right_type))
	{
		left_reg = coerce_to_declared_type(left_reg, Variant::FLOAT, func,
			"the vector scalar", expr->line, expr->column);
		left_type = Variant::FLOAT;
	}

	// Same-type operands enable native codegen; mixed types fall back to VEVAL.
	const auto scalar_matches_vector = [](IRInstruction::TypeHint vector_type,
	                                      IRInstruction::TypeHint scalar_type) {
		return (TypeHintUtils::is_float_vector(vector_type) &&
			(scalar_type == Variant::INT || scalar_type == Variant::FLOAT)) ||
			(TypeHintUtils::is_int_vector(vector_type) && scalar_type == Variant::INT);
	};
	IRInstruction::TypeHint result_type = IRInstruction::TypeHint_NONE;
	if (is_bitwise) {
		// Bitwise is integer-only; unknown types fall back to VEVAL for runtime error.
		if (left_type == Variant::INT && right_type == Variant::INT) {
			result_type = Variant::INT;
		}
	} else if (is_arithmetic || is_comparison) {
		if (left_type == Variant::INT && right_type == Variant::INT) {
			result_type = Variant::INT;
		} else if (left_type == Variant::FLOAT && right_type == Variant::FLOAT) {
			result_type = Variant::FLOAT;
		} else if (left_type != IRInstruction::TypeHint_NONE &&
		           right_type != IRInstruction::TypeHint_NONE &&
		           left_type == right_type &&
		           TypeHintUtils::is_vector(left_type)) {
			result_type = left_type;
		} else if (expr->op == BinaryExpr::Op::ADD &&
			left_type == Variant::STRING && right_type == Variant::STRING) {
			result_type = Variant::STRING;
		} else if (mul_or_div && scalar_matches_vector(left_type, right_type)) {
			result_type = left_type;
		} else if (expr->op == BinaryExpr::Op::MUL &&
			scalar_matches_vector(right_type, left_type)) {
			result_type = right_type;
		}
	}

	IROpcode op;
	switch (expr->op) {
		case BinaryExpr::Op::ADD: op = IROpcode::ADD; break;
		case BinaryExpr::Op::SUB: op = IROpcode::SUB; break;
		case BinaryExpr::Op::MUL: op = IROpcode::MUL; break;
		case BinaryExpr::Op::DIV: op = IROpcode::DIV; break;
		case BinaryExpr::Op::MOD: op = IROpcode::MOD; break;
		case BinaryExpr::Op::EQ: op = IROpcode::CMP_EQ; break;
		case BinaryExpr::Op::NEQ: op = IROpcode::CMP_NEQ; break;
		case BinaryExpr::Op::LT: op = IROpcode::CMP_LT; break;
		case BinaryExpr::Op::LTE: op = IROpcode::CMP_LTE; break;
		case BinaryExpr::Op::GT: op = IROpcode::CMP_GT; break;
		case BinaryExpr::Op::GTE: op = IROpcode::CMP_GTE; break;
		case BinaryExpr::Op::AND: op = IROpcode::AND; break;
		case BinaryExpr::Op::OR: op = IROpcode::OR; break;
		case BinaryExpr::Op::BIT_AND: op = IROpcode::BIT_AND; break;
		case BinaryExpr::Op::BIT_OR: op = IROpcode::BIT_OR; break;
		case BinaryExpr::Op::BIT_XOR: op = IROpcode::BIT_XOR; break;
		case BinaryExpr::Op::SHL: op = IROpcode::SHL; break;
		case BinaryExpr::Op::SHR: op = IROpcode::SHR; break;
		case BinaryExpr::Op::POW: op = IROpcode::POW; break;
		case BinaryExpr::Op::IN: op = IROpcode::IN; break;
		default:
			error_at("Unknown binary operator", expr);
	}

	IRInstruction instr(op, IRValue::reg(result_reg), IRValue::reg(left_reg), IRValue::reg(right_reg));
	instr.type_hint = result_type;
	instr.lhs_type_hint = left_type;
	instr.rhs_type_hint = right_type;
	func.ir.instructions.push_back(instr);
	if (func.reclassifiable_registers.count(left_reg) != 0 ||
		func.reclassifiable_registers.count(right_reg) != 0)
	{
		func.reclassifiable_registers.insert(result_reg);
	}

	if (expr->op == BinaryExpr::Op::IN) {
		set_register_type(func, result_reg, Variant::BOOL);
	} else if (is_comparison) {
		// type_hint describes the operands (selects native compare); result is BOOL.
		set_register_type(func, result_reg, Variant::BOOL);
	} else if (result_type != IRInstruction::TypeHint_NONE) {
		set_register_type(func, result_reg, result_type);
	} else if (expr->op == BinaryExpr::Op::ADD &&
	           left_type == Variant::STRING && right_type == Variant::STRING) {
		// VEVAL carries no type hint; propagate String so length() etc. lower.
		set_register_type(func, result_reg, Variant::STRING);
	}

	free_register(func, left_reg);
	free_register(func, right_reg);

	return result_reg;
}

// `x is SomeClass`: TYPE_TEST for OBJECT, then Object.is_class() via VCALL.
// Non-objects return false without the call.
//
// is_class() walks the ClassDB chain and nothing else, so a name declared by a
// script's `class_name` answers false there. The script chain is walked after
// it: get_script(), then get_global_name() against the name and
// get_base_script() until one matches or the chain runs out. Without that,
// `node is Enemy` is quietly false and `node as Enemy` quietly null.
// The name a class instance was made from, so a Dictionary can answer `is`.
static constexpr const char* CLASS_NAME_KEY = "@class";

// `x is Declared` where x is a Dictionary of unknown provenance: compare the
// `@class` the constructor wrote against every class in the file that derives
// from the one asked about. Answers -1 when the name is not one of them, which
// leaves the engine walk in gen_class_test to run.
int CodeGenerator::gen_instance_class_test(int value_reg, const std::string& class_name,
	int result_reg, FunctionContext& func)
{
	const StructDecl* target = find_struct(class_name);
	if (target == nullptr || !target->is_class) {
		return -1;
	}
	const IRInstruction::TypeHint known = get_register_type(func, value_reg);
	if (known != IRInstruction::TypeHint_NONE && known != Variant::DICTIONARY) {
		return -1;
	}

	std::vector<std::string> names;
	for (const auto& [name, decl] : m_structs) {
		if (!decl->is_class) {
			continue;
		}
		for (const StructDecl* at = decl; at != nullptr; at = class_base(*at)) {
			if (at->name == class_name) {
				names.push_back(name);
				break;
			}
		}
	}
	if (names.empty()) {
		return -1;
	}
	// m_structs is a hash map: sort so the same source gives the same machine code.
	std::sort(names.begin(), names.end());

	const std::string end_label = make_label("is_instance_end");
	if (known != Variant::DICTIONARY) {
		int is_dict_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(is_dict_reg),
			IRValue::reg(value_reg), IRValue::imm(static_cast<int64_t>(Variant::DICTIONARY)));
		set_register_type(func, is_dict_reg, Variant::BOOL);
		emit_conditional_branch(IROpcode::BRANCH_ZERO, is_dict_reg, end_label, func);
		free_register(func, is_dict_reg);
	}

	// One get: a missing key answers null, which matches no name.
	int tag_reg = gen_dict_get(value_reg, CLASS_NAME_KEY, func);
	for (const std::string& name : names) {
		int name_reg = alloc_register(func);
		IRInstruction load_name(IROpcode::LOAD_STRING, IRValue::reg(name_reg),
			IRValue::imm(add_string_constant(name)));
		load_name.type_hint = Variant::STRING;
		func.ir.instructions.push_back(load_name);
		set_register_type(func, name_reg, Variant::STRING);

		func.ir.instructions.emplace_back(IROpcode::CMP_EQ, IRValue::reg(result_reg),
			IRValue::reg(tag_reg), IRValue::reg(name_reg));
		set_register_type(func, result_reg, Variant::BOOL);
		free_register(func, name_reg);
		emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, result_reg, end_label, func);
	}
	free_register(func, tag_reg);

	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end_label));
	return result_reg;
}

int CodeGenerator::gen_class_test(int value_reg, const std::string& class_name,
	FunctionContext& func)
{
	if (const StructDecl* target = find_struct(class_name);
		target != nullptr && !target->is_class) {
		if (const StructDecl* actual = get_register_struct(func, value_reg)) {
			int folded = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(folded),
				IRValue::imm(actual == target ? 1 : 0));
			set_register_type(func, folded, Variant::BOOL);
			return folded;
		}
		const IRInstruction::TypeHint known = get_register_type(func, value_reg);
		int result = alloc_register(func);
		if (known != IRInstruction::TypeHint_NONE && known != Variant::DICTIONARY) {
			func.ir.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(result),
				IRValue::imm(0));
			set_register_type(func, result, Variant::BOOL);
			return result;
		}
		const std::string end = make_label("is_struct_end");
		if (known != Variant::DICTIONARY) {
			func.ir.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(result),
				IRValue::imm(0));
			int is_dict = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(is_dict),
				IRValue::reg(value_reg), IRValue::imm(int64_t(Variant::DICTIONARY)));
			set_register_type(func, is_dict, Variant::BOOL);
			emit_conditional_branch(IROpcode::BRANCH_ZERO, is_dict, end, func);
			free_register(func, is_dict);
		}
		int exact = gen_struct_shape_test(value_reg, *target, func);
		func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(result),
			IRValue::reg(exact));
		free_register(func, exact);
		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end));
		set_register_type(func, result, Variant::BOOL);
		return result;
	}

	// A class instance is a Dictionary, so the tag test below would answer false
	// for its own name and for everything it extends. The declaration settles the
	// script side of the chain; the engine side is the same run-time walk, run on
	// the object the instance holds rather than on the Dictionary.
	int object_reg = value_reg;
	bool owns_object_reg = false;
	if (const StructDecl* decl = get_register_struct(func, value_reg)) {
		bool declares_it = false;
		for (const StructDecl* at = decl; at != nullptr && !declares_it; at = class_base(*at)) {
			declares_it = at->name == class_name;
		}
		// A class the file declares but that is not in this chain is settled here:
		// only an engine name is left for the run-time walk.
		const bool script_class = find_struct(class_name) != nullptr;
		// A class without `extends` still derives from RefCounted, and so from
		// Object: the two names GDScript answers true for without a base.
		const bool implicit_base = decl->is_class && native_base(*decl) == nullptr
			&& (class_name == "RefCounted" || class_name == "Object");
		if (declares_it || script_class || native_base(*decl) == nullptr) {
			int folded_reg = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(folded_reg),
				IRValue::imm(declares_it || implicit_base ? 1 : 0));
			set_register_type(func, folded_reg, Variant::BOOL);
			return folded_reg;
		}
		object_reg = gen_native_base_load(value_reg, func);
		owns_object_reg = true;
	}

	int result_reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(result_reg),
		IRValue::imm(0));
	set_register_type(func, result_reg, Variant::BOOL);

	// A value the compiler no longer tracks -- one read back out of a container, an
	// untyped parameter -- is still an instance at run time, and carries the name
	// it was made from. The file declares the whole chain, so which names answer
	// true for this one is known here.
	if (object_reg == value_reg) {
		if (int tagged = gen_instance_class_test(value_reg, class_name, result_reg, func);
			tagged >= 0) {
			return tagged;
		}
	}

	const IRInstruction::TypeHint known = get_register_type(func, object_reg);
	if (known != IRInstruction::TypeHint_NONE && known != Variant::OBJECT) {
		if (owns_object_reg) {
			free_register(func, object_reg);
		}
		return result_reg;
	}

	const auto emit_vcall = [&](int dst_reg, int object_reg, const char* method, int arg_reg) {
		IRInstruction vcall(IROpcode::VCALL);
		vcall.operands.push_back(IRValue::reg(dst_reg));
		vcall.operands.push_back(IRValue::reg(object_reg));
		vcall.operands.push_back(ir_str(method));
		vcall.operands.push_back(IRValue::imm(arg_reg >= 0 ? 1 : 0));
		if (arg_reg >= 0) {
			vcall.operands.push_back(IRValue::reg(arg_reg));
		}
		func.ir.instructions.push_back(vcall);
	};
	const auto emit_is_object = [&](int object_reg, const std::string& label) {
		int is_object_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(is_object_reg),
			IRValue::reg(object_reg), IRValue::imm(static_cast<int64_t>(Variant::OBJECT)));
		set_register_type(func, is_object_reg, Variant::BOOL);
		emit_conditional_branch(IROpcode::BRANCH_ZERO, is_object_reg, label, func);
		free_register(func, is_object_reg);
	};

	const std::string end_label = make_label("is_class_end");
	if (known != Variant::OBJECT) {
		emit_is_object(object_reg, end_label);
	}

	const int name_index = add_string_constant(class_name);

	int name_reg = alloc_register(func);
	IRInstruction load_name(IROpcode::LOAD_STRING, IRValue::reg(name_reg),
		IRValue::imm(name_index));
	load_name.type_hint = Variant::STRING;
	func.ir.instructions.push_back(load_name);
	set_register_type(func, name_reg, Variant::STRING);

	emit_vcall(result_reg, object_reg, "is_class", name_reg);
	free_register(func, name_reg);

	// An engine class is settled; only a script class needs the walk.
	emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, result_reg, end_label, func);

	// get_global_name() answers a StringName, so compare against one: the pair
	// (STRING_NAME, STRING) is a different Variant evaluator.
	int global_name_reg = alloc_register(func);
	IRInstruction load_global_name(IROpcode::LOAD_STRING_AS, IRValue::reg(global_name_reg),
		IRValue::imm(name_index), IRValue::imm(static_cast<int64_t>(Variant::STRING_NAME)));
	load_global_name.type_hint = Variant::STRING_NAME;
	func.ir.instructions.push_back(load_global_name);
	set_register_type(func, global_name_reg, Variant::STRING_NAME);

	int script_reg = alloc_register(func);
	emit_vcall(script_reg, object_reg, "get_script", -1);

	const std::string loop_label = make_label("is_class_script");
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(loop_label));

	// A null script ends the chain.
	emit_is_object(script_reg, end_label);

	int declared_reg = alloc_register(func);
	emit_vcall(declared_reg, script_reg, "get_global_name", -1);
	func.ir.instructions.emplace_back(IROpcode::CMP_EQ, IRValue::reg(result_reg),
		IRValue::reg(declared_reg), IRValue::reg(global_name_reg));
	set_register_type(func, result_reg, Variant::BOOL);
	free_register(func, declared_reg);
	emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, result_reg, end_label, func);

	int base_reg = alloc_register(func);
	emit_vcall(base_reg, script_reg, "get_base_script", -1);
	func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(script_reg),
		IRValue::reg(base_reg));
	free_register(func, base_reg);
	func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(loop_label));

	free_register(func, script_reg);
	free_register(func, global_name_reg);

	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end_label));
	if (owns_object_reg) {
		free_register(func, object_reg);
	}
	return result_reg;
}

int CodeGenerator::gen_cast(const CastExpr* expr, FunctionContext& func) {
	// The engine analyzer owns qualified types such as Viewport.MSAA. They are
	// intentionally dropped by parse_type_name(), just as in declarations.
	if (expr->type_name.empty() || expr->type_name == "Variant") {
		return gen_expr(expr->value.get(), func);
	}
	const IRInstruction::TypeHint target = type_hint_from_string(expr->type_name);
	if (target != IRInstruction::TypeHint_NONE) {
		int result = gen_builtin_cast(expr, target, func);
		if (!expr->type_arguments.empty()) {
			TypeExpr declared;
			declared.names.push_back(expr->type_name);
			declared.arguments = expr->type_arguments;
			apply_declared_type(result, declared, func);
		}
		return result;
	}
	return gen_class_cast(expr, func);
}

int CodeGenerator::gen_builtin_cast(const CastExpr* expr, IRInstruction::TypeHint target,
	FunctionContext& func)
{
	const int value_reg = gen_expr(expr->value.get(), func);

	if (get_register_type(func, value_reg) == target) {
		return value_reg;
	}

	switch (target) {
		case Variant::INT:
		case Variant::FLOAT:
		case Variant::BOOL:
		case Variant::STRING: {
			const GlobalFunction* info = find_global_function(expr->type_name);
			if (info == nullptr) {
				throw CompilerException(ErrorType::CODEGEN_ERROR,
					"'" + expr->type_name + "' is a Variant type but has no globals table row");
			}
			return gen_global_call(*info, { value_reg }, func, expr);
		}
		default:
			return gen_host_constructor_typed(expr->type_name, target, { value_reg }, func, expr);
	}
}

int CodeGenerator::gen_class_cast(const CastExpr* expr, FunctionContext& func) {
	int value_reg = gen_expr(expr->value.get(), func);
	int result_reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::LOAD_NIL, IRValue::reg(result_reg));

	const TraitDecl* iface = find_trait(expr->type_name);
	int test_reg = iface != nullptr
		? gen_trait_test(value_reg, *iface, func)
		: gen_class_test(value_reg, expr->type_name, func);
	const std::string end_label = make_label("as_class_end");
	emit_conditional_branch(IROpcode::BRANCH_ZERO, test_reg, end_label, func);
	free_register(func, test_reg);

	func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(result_reg),
		IRValue::reg(value_reg));
	// The cast answers the same instance, so it stays as usable as the original.
	if (const StructDecl* target = find_struct(expr->type_name);
		target != nullptr && !target->is_class) {
		set_register_struct(func, result_reg, target);
	} else {
		set_register_struct(func, result_reg, get_register_struct(func, value_reg));
	}
	if (iface != nullptr) {
		add_register_trait(func, result_reg, iface);
		func.trait_only_registers.insert(result_reg);
	}
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end_label));

	free_register(func, value_reg);
	// Result is NIL or OBJECT: no type hint.
	return result_reg;
}

int CodeGenerator::gen_type_test(const TypeTestExpr* expr, FunctionContext& func) {
	const std::string& single_name = expr->type.single_name();
	if (const TraitDecl* trait = find_trait(single_name)) {
		int value_reg = gen_expr(expr->value.get(), func);
		int result_reg = gen_trait_test(value_reg, *trait, func);
		free_register(func, value_reg);
		return result_reg;
	}
	const IRInstruction::TypeHint builtin = single_name.empty()
		? IRInstruction::TypeHint_NONE : type_hint_from_string(single_name);
	if (!single_name.empty() && builtin == IRInstruction::TypeHint_NONE &&
		single_name != "Variant" &&
		m_enums.find(single_name) == m_enums.end()) {
		// Class name: requires engine-side inheritance check.
		int value_reg = gen_expr(expr->value.get(), func);
		int result_reg = gen_class_test(value_reg, single_name, func);
		free_register(func, value_reg);
		return result_reg;
	}
	const TypeSet tested_set = type_set_from(expr->type, expr->line, expr->column);

	int value_reg = gen_expr(expr->value.get(), func);
	int result_reg = alloc_register(func);

	// Known type folds to a constant.
	const IRInstruction::TypeHint known = get_register_type(func, value_reg);
	if (known != IRInstruction::TypeHint_NONE) {
		func.ir.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(result_reg),
			IRValue::imm(tested_set.contains(static_cast<Variant::Type>(known)) ? 1 : 0));
	} else {
		func.ir.instructions.emplace_back(expr->type.is_union() ? IROpcode::TYPE_TEST_MASK : IROpcode::TYPE_TEST,
			IRValue::reg(result_reg), IRValue::reg(value_reg),
			IRValue::imm(expr->type.is_union() ? static_cast<int64_t>(tested_set.mask)
			                                : static_cast<int64_t>(tested_set.only())));
	}
	set_register_type(func, result_reg, Variant::BOOL);

	free_register(func, value_reg);
	return result_reg;
}

int CodeGenerator::gen_trait_test(int value_reg, const TraitDecl& iface,
	FunctionContext& func)
{
	if (const StructDecl* actual = get_register_struct(func, value_reg)) {
		int result = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(result),
			IRValue::imm(declaration_uses(*actual, iface) ? 1 : 0));
		set_register_type(func, result, Variant::BOOL);
		return result;
	}
	if (auto known_ifaces = func.register_traits.find(value_reg);
		known_ifaces != func.register_traits.end() && known_ifaces->second.count(&iface)) {
		int result = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(result), IRValue::imm(1));
		set_register_type(func, result, Variant::BOOL);
		return result;
	}

	const IRInstruction::TypeHint known = get_register_type(func, value_reg);
	int result = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(result), IRValue::imm(0));
	set_register_type(func, result, Variant::BOOL);
	if (known != IRInstruction::TypeHint_NONE && known != Variant::OBJECT &&
		known != Variant::DICTIONARY) return result;

	std::vector<std::string> class_names;
	for (const auto& [name, decl] : m_structs) {
		if (decl->is_class && declaration_uses(*decl, iface)) class_names.push_back(name);
	}
	std::sort(class_names.begin(), class_names.end());
	const std::string object_arm = make_label("uses_object");
	const std::string end = make_label("uses_end");

	if (known == IRInstruction::TypeHint_NONE) {
		int is_dict = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(is_dict),
			IRValue::reg(value_reg), IRValue::imm(int64_t(Variant::DICTIONARY)));
		set_register_type(func, is_dict, Variant::BOOL);
		emit_conditional_branch(IROpcode::BRANCH_ZERO, is_dict, object_arm, func);
		free_register(func, is_dict);
	}
	if (known != Variant::OBJECT) {
		if (!class_names.empty()) {
			int tag = gen_dict_get(value_reg, CLASS_NAME_KEY, func);
			for (const std::string& name : class_names) {
				int expected = gen_string_value(name, func);
				func.ir.instructions.emplace_back(IROpcode::CMP_EQ, IRValue::reg(result),
					IRValue::reg(tag), IRValue::reg(expected));
				free_register(func, expected);
				emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, result, end, func);
			}
			free_register(func, tag);
		}
		func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(end));
	}

	if (known == IRInstruction::TypeHint_NONE) {
		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(object_arm));
		int is_object = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(is_object),
			IRValue::reg(value_reg), IRValue::imm(int64_t(Variant::OBJECT)));
		set_register_type(func, is_object, Variant::BOOL);
		emit_conditional_branch(IROpcode::BRANCH_ZERO, is_object, end, func);
		free_register(func, is_object);
	}
	IRInstruction dynamic(IROpcode::TRAIT_TEST, IRValue::reg(result),
		IRValue::reg(value_reg), IRValue::imm(int64_t(m_trait_indices.at(&iface))));
	dynamic.type_hint = Variant::BOOL;
	func.ir.instructions.push_back(std::move(dynamic));
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end));
	return result;
}

int CodeGenerator::require_trait_value(int value_reg, const TraitDecl& iface,
	const std::string& what, FunctionContext& func, int line, int column, bool nullable)
{
	const IRInstruction::TypeHint initial_type = get_register_type(func, value_reg);
	if (nullable && initial_type == Variant::NIL) {
		return value_reg;
	}
	if (const StructDecl* actual = get_register_struct(func, value_reg)) {
		if (!declaration_uses(*actual, iface)) {
			error_at("Cannot assign '" + actual->name + "' to " + what +
				" of trait type '" + iface.name + "'", line, column,
				"Declare 'uses " + iface.name + "' on '" + actual->name + "'");
		}
		add_register_trait(func, value_reg, &iface);
		return value_reg;
	}
	if (auto known_ifaces = func.register_traits.find(value_reg);
		known_ifaces != func.register_traits.end() && known_ifaces->second.count(&iface)) return value_reg;
	const IRInstruction::TypeHint known = get_register_type(func, value_reg);
	if (known != IRInstruction::TypeHint_NONE && known != Variant::OBJECT &&
		known != Variant::DICTIONARY) {
		error_at("Cannot assign a value of type " + std::string(variant_type_name(known)) +
			" to " + what + " of trait type '" + iface.name + "'", line, column);
	}
	if (m_struct_checks || m_restricted) {
		const std::string accepted = nullable ? make_label("nullable_trait_ok") : std::string{};
		if (nullable && known == IRInstruction::TypeHint_NONE) {
			int is_nil = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(is_nil),
				IRValue::reg(value_reg), IRValue::imm(int64_t(Variant::NIL)));
			set_register_type(func, is_nil, Variant::BOOL);
			emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, is_nil, accepted, func);
			free_register(func, is_nil);
		}
		int test = gen_trait_test(value_reg, iface, func);
		const std::string passed = make_label("uses_guard_ok");
		emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, test, passed, func);
		free_register(func, test);
		IRInstruction fail(IROpcode::THROW, ir_str("TypeError"),
			ir_str(what + " does not implement '" + iface.name + "'"));
		fail.operands.push_back(IRValue::imm(0));
		func.ir.instructions.push_back(std::move(fail));
		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(passed));
		if (nullable && known == IRInstruction::TypeHint_NONE) {
			func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(accepted));
		}
	}
	// A nullable value needs a null check before trait methods are legal.
	// The declaration is retained separately so narrowing can restore the proof.
	if (!nullable) add_register_trait(func, value_reg, &iface);
	return value_reg;
}

// assert(condition, "message"): branch over ECALL_THROW.
int CodeGenerator::gen_assert(const CallExpr* expr, FunctionContext& func) {
	if (expr->arguments.empty() || expr->arguments.size() > 2) {
		error_at("assert() takes 1 or 2 arguments, got " +
			std::to_string(expr->arguments.size()), expr);
	}

	std::string message = "assertion failed";
	const Expr* computed_message = nullptr;
	if (expr->arguments.size() == 2) {
		auto* literal = dynamic_cast<const LiteralExpr*>(expr->arguments[1].get());
		if (literal != nullptr && literal->lit_type == LiteralExpr::Type::STRING) {
			message = std::get<std::string>(literal->value);
		} else {
			computed_message = expr->arguments[1].get();
		}
	}

	const std::string passed_label = make_label("assert_passed");
	const NarrowingInfo narrowing = condition_narrowing(expr->arguments[0].get(), func);

	int cond_reg = gen_expr(expr->arguments[0].get(), func);
	emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, cond_reg, passed_label, func);
	free_register(func, cond_reg);

	IRInstruction throw_instr(IROpcode::THROW, ir_str("assert"), ir_str(message));
	if (computed_message != nullptr) {
		const int message_reg = gen_expr(computed_message, func);
		throw_instr.operands.push_back(IRValue::imm(1));
		throw_instr.operands.push_back(IRValue::reg(message_reg));
		func.ir.instructions.push_back(throw_instr);
		free_register(func, message_reg);
	} else {
		throw_instr.operands.push_back(IRValue::imm(0));
		func.ir.instructions.push_back(throw_instr);
	}
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(passed_label));
	// A member can change on any later call, so an assertion cannot safely
	// narrow its re-reads for the rest of the function.
	if (!narrowing.is_member()) {
		apply_narrowing(narrowing, true, func);
	}

	// Parsed as a call: must leave a value. Returns NIL.
	int result_reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::LOAD_NIL, IRValue::reg(result_reg));
	set_register_type(func, result_reg, Variant::NIL);
	return result_reg;
}

int CodeGenerator::gen_unary(const UnaryExpr* expr, FunctionContext& func) {
	int operand_reg = gen_expr(expr->operand.get(), func);
	int result_reg = alloc_register(func);

	IROpcode op;
	switch (expr->op) {
		case UnaryExpr::Op::NEG: op = IROpcode::NEG; break;
		case UnaryExpr::Op::NOT: op = IROpcode::NOT; break;
		case UnaryExpr::Op::BIT_NOT: op = IROpcode::BIT_NOT; break;
		default:
			error_at("Unknown unary operator", expr);
	}

	IRInstruction instr(op, IRValue::reg(result_reg), IRValue::reg(operand_reg));
	if (expr->op == UnaryExpr::Op::BIT_NOT && get_register_type(func, operand_reg) == Variant::INT) {
		instr.type_hint = Variant::INT;
		set_register_type(func, result_reg, Variant::INT);
	} else if (expr->op == UnaryExpr::Op::NOT) {
		set_register_type(func, result_reg, Variant::BOOL);
	} else if (expr->op == UnaryExpr::Op::NEG) {
		const IRInstruction::TypeHint operand_type = get_register_type(func, operand_reg);
		if (operand_type == Variant::INT || operand_type == Variant::FLOAT) {
			instr.type_hint = operand_type;
			set_register_type(func, result_reg, operand_type);
		}
	}
	func.ir.instructions.push_back(instr);

	free_register(func, operand_reg);
	return result_reg;
}

int CodeGenerator::gen_await(const AwaitExpr* expr, FunctionContext& func) {
	if (!func.ir.is_coroutine) {
		// Parser/codegen coroutine flag mismatch.
		error_at("'await' outside a coroutine", expr);
	}
	const int operand_reg = gen_expr(expr->operand.get(), func);
	const int result_reg = alloc_register(func);

	// Awaited value type unknown; no hint propagated.
	func.ir.instructions.emplace_back(IROpcode::AWAIT,
		IRValue::reg(result_reg), IRValue::reg(operand_reg));

	free_register(func, operand_reg);
	return result_reg;
}

int CodeGenerator::gen_ternary(const TernaryExpr* expr, FunctionContext& func) {
	// Only the taken branch is evaluated.
	std::string else_label = make_label("ternary_else");
	std::string end_label = make_label("ternary_end");

	int result_reg = alloc_register(func);

	int cond_reg = gen_expr(expr->condition.get(), func);
	emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, else_label, func);
	free_register(func, cond_reg);

	int true_reg = gen_expr(expr->true_value.get(), func);
	func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(result_reg), IRValue::reg(true_reg));
	IRInstruction::TypeHint true_type = get_register_type(func, true_reg);
	const StructDecl* true_struct = get_register_struct(func, true_reg);
	const StructDecl* true_element = nullptr;
	if (auto it = func.array_element_structs.find(true_reg); it != func.array_element_structs.end()) {
		true_element = it->second;
	}
	free_register(func, true_reg);
	func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(end_label));

	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(else_label));
	int false_reg = gen_expr(expr->false_value.get(), func);
	func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(result_reg), IRValue::reg(false_reg));
	IRInstruction::TypeHint false_type = get_register_type(func, false_reg);
	const StructDecl* false_struct = get_register_struct(func, false_reg);
	const StructDecl* false_element = nullptr;
	if (auto it = func.array_element_structs.find(false_reg); it != func.array_element_structs.end()) {
		false_element = it->second;
	}
	free_register(func, false_reg);

	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end_label));

	if (true_type != IRInstruction::TypeHint_NONE && true_type == false_type) {
		set_register_type(func, result_reg, true_type);
	}
	if (true_struct != nullptr && true_struct == false_struct) {
		set_register_struct(func, result_reg, true_struct);
	}
	if (true_element != nullptr && true_element == false_element) {
		func.array_element_structs[result_reg] = true_element;
	}

	return result_reg;
}

int CodeGenerator::emit_local_call(const std::string& name, std::vector<int> arg_regs,
	FunctionContext& func, const Expr* site)
{
	auto sig = m_local_signatures.find(name);
	if (sig != m_local_signatures.end()) {
		const auto& params = sig->second->parameters;
		if (arg_regs.size() > params.size()) {
			error_at("Too many arguments to '" + name + "': expected at most " +
				std::to_string(params.size()) + ", got " + std::to_string(arg_regs.size()), site);
		}
		for (size_t i = arg_regs.size(); i < params.size(); i++) {
			if (!params[i].default_value) {
				error_at("Missing argument '" + params[i].name + "' in call to '" +
					name + "'", site);
			}
			arg_regs.push_back(gen_expr(params[i].default_value.get(), func));
		}
	}

	int result_reg = alloc_register(func);

	const bool hosted = sig != m_local_signatures.end() && sig->second->is_coroutine;

	if (sig != m_local_signatures.end() && !hosted) {
		apply_declared_type(result_reg, sig->second->return_type, func);
	}

	IRInstruction call_instr(hosted ? IROpcode::CALL_HOSTED : IROpcode::CALL);
	call_instr.operands.push_back(ir_str(name));
	call_instr.operands.push_back(IRValue::reg(result_reg));
	call_instr.operands.push_back(IRValue::imm(arg_regs.size()));
	for (int arg_reg : arg_regs) {
		call_instr.operands.push_back(IRValue::reg(arg_reg));
	}
	if (!hosted && sig != m_local_signatures.end()) {
		bool has_typed_parameter = false;
		bool exact_typed_arguments = true;
		const auto& params = sig->second->parameters;
		for (size_t i = 0; i < params.size(); i++) {
			const IRInstruction::TypeHint declared = single_type_from(params[i].type_hint);
			if (declared != Variant::INT && declared != Variant::FLOAT && declared != Variant::BOOL) {
				continue;
			}
			has_typed_parameter = true;
			if (i >= arg_regs.size() || get_register_type(func, arg_regs[i]) != declared) {
				exact_typed_arguments = false;
				break;
			}
		}
		call_instr.trusted_internal_call = has_typed_parameter && exact_typed_arguments;
	}
	func.ir.instructions.push_back(call_instr);

	for (int reg : arg_regs) {
		free_register(func, reg);
	}

	return result_reg;
}

int CodeGenerator::gen_call(const CallExpr* expr, FunctionContext& func) {
	// Struct constructor: checked before args are lowered (only call that names them).
	if (const StructDecl* decl = find_struct(expr->function_name)) {
		return gen_struct_construct(*decl, expr->arguments, *expr, func, expr);
	}
	// assert(): handled before argument lowering (message is host-side text).
	if (expr->function_name == "assert" && !is_local_function("assert")) {
		return gen_assert(expr, func);
	}
	if (expr->function_name == "range" && !is_local_function("range")) {
		return gen_range(expr, func);
	}
	if (expr->function_name == "Color8" && !is_local_function("Color8")) {
		return gen_color8(expr, func);
	}
	if (expr->function_name == "Callable" && !is_local_function("Callable")) {
		return gen_callable_constructor(expr, func);
	}
	if (expr->function_name == "super" && find_variable(func, "super") == nullptr) {
		return gen_super_init(expr, func);
	}
	if (m_current_class != nullptr && find_variable(func, expr->function_name) == nullptr) {
		const StructDecl* owner = nullptr;
		if (const FunctionDecl* method =
			find_class_method(*m_current_class, expr->function_name, &owner))
		{
			Variable* self = find_variable(func, "self");
			if (method->is_static) {
				return gen_class_method_call(*m_current_class, *method, *owner,
					-1, expr->arguments, *expr, func, expr);
			}
			if (self != nullptr) {
				return gen_class_method_call(*m_current_class, *method, *owner,
					self->register_num, expr->arguments, *expr, func, expr);
			}
			error_at("'" + m_current_class->name + "." + method->name +
				"()' needs an instance, and a 'static func' has none", expr,
				"Make '" + method->name + "()' static too, or pass the instance as an argument");
		}
	}
	// preload() lowers to the same LOAD_RESOURCE as a constant-path load().
	const bool is_preload = expr->function_name == "preload" && !is_local_function("preload");
	const bool is_load = expr->function_name == "load" && !is_local_function("load");
	if ((is_load || is_preload) && expr->arguments.size() == 1)
	{
		if (const std::string* path = constant_string(expr->arguments[0].get(), func)) {
			return gen_load_resource(*path, func);
		}
		if (is_preload) {
			error_at("preload() needs a constant path", expr,
				"The engine requires one too. Use load() for a path the program computes");
		}
	}
	if (is_preload && expr->arguments.size() != 1) {
		error_at("preload() takes exactly 1 argument", expr);
	}
	reject_named_arguments(*expr, "'" + expr->function_name + "'", expr);

	std::vector<int> arg_regs;
	for (const auto& arg : expr->arguments) {
		arg_regs.push_back(gen_expr(arg.get(), func));
	}

	if (is_inline_primitive_constructor(expr->function_name)) {
		int result = gen_inline_constructor(expr->function_name, arg_regs, func, expr);
		for (int reg : arg_regs) {
			free_register(func, reg);
		}
		return result;
	}

	if (is_host_constructor(expr->function_name)) {
		int result = gen_host_constructor(expr->function_name, arg_regs, func, expr);
		for (int reg : arg_regs) {
			free_register(func, reg);
		}
		return result;
	}

	// typeof(): guest-side tag read, not a globals.h entry.
	if (expr->function_name == "typeof") {
		if (arg_regs.size() != 1) {
			error_at("typeof() takes exactly 1 argument", expr);
		}
		const IRInstruction::TypeHint known = get_register_type(func, arg_regs[0]);
		if (known != IRInstruction::TypeHint_NONE) {
			free_register(func, arg_regs[0]);
			return gen_int_immediate(static_cast<int64_t>(known), func);
		}
		int result_reg = alloc_register(func);
		IRInstruction instr(IROpcode::TYPE_OF);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::reg(arg_regs[0]));
		instr.type_hint = Variant::INT;
		func.ir.instructions.push_back(instr);
		set_register_type(func, result_reg, Variant::INT);
		free_register(func, arg_regs[0]);
		return result_reg;
	}

	if (expr->function_name == "get_node") {
		if (arg_regs.size() > 1) {
			error_at("get_node() takes at most 1 argument", expr);
		}

		// ECALL_GET_NODE takes raw characters: compile-time paths only.
		// Run-time paths go through VCALL.
		if (!arg_regs.empty()) {
			auto* literal = dynamic_cast<const LiteralExpr*>(expr->arguments[0].get());
			if (literal == nullptr || literal->lit_type != LiteralExpr::Type::STRING) {
				int self_reg = gen_get_node(".", func);
				int result_reg = alloc_register(func);
				IRInstruction vcall(IROpcode::VCALL);
				vcall.operands.push_back(IRValue::reg(result_reg));
				vcall.operands.push_back(IRValue::reg(self_reg));
				vcall.operands.push_back(ir_str("get_node"));
				vcall.operands.push_back(IRValue::imm(1));
				vcall.operands.push_back(IRValue::reg(arg_regs[0]));
				func.ir.instructions.push_back(vcall);
				free_register(func, self_reg);
				free_register(func, arg_regs[0]);
				return result_reg;
			}
			free_register(func, arg_regs[0]);
			return gen_get_node(std::get<std::string>(literal->value), func);
		}

		return gen_get_node(".", func);
	}

	reject_test_reference(expr->function_name, expr);
	if (is_local_function(expr->function_name)) {
		return emit_local_call(expr->function_name, std::move(arg_regs), func, expr);
	}

	// Run-time path: passed as Variant, same resource-allowed callback as literal.
	if (expr->function_name == "load") {
		if (arg_regs.size() != 1) {
			error_at("load() takes exactly 1 argument", expr);
		}

		int result_reg = alloc_register(func);
		IRInstruction instr(IROpcode::LOAD_RESOURCE_VAR, IRValue::reg(result_reg),
			IRValue::reg(arg_regs[0]));
		instr.type_hint = Variant::OBJECT;
		func.ir.instructions.push_back(instr);
		set_register_type(func, result_reg, Variant::OBJECT);
		free_register(func, arg_regs[0]);
		return result_reg;
	}

	// After local functions (user print() wins), before self-call fallback.
	if (is_global_function(expr->function_name)) {
		int result = gen_global_function(expr, arg_regs, func);
		for (int reg : arg_regs) {
			free_register(func, reg);
		}
		return result;
	}

	// Callable variable call (after script functions and @GlobalScope).
	if (find_variable(func, expr->function_name) != nullptr ||
	    is_global_variable(expr->function_name)) {
		VariableExpr name_expr(expr->function_name);
		name_expr.line = expr->line;
		name_expr.column = expr->column;
		int callable_reg = gen_variable(&name_expr, func);
		return gen_callable_variable_call(expr, callable_reg, arg_regs, func);
	}

	// A signal is not callable; the self-call fallback would be silently dropped.
	if (find_signal(expr->function_name) != nullptr) {
		error_at("'" + expr->function_name + "' is a signal, which cannot be called", expr,
			"Emit it with '" + expr->function_name + ".emit(...)'");
	}

	// Refuse unimplemented globals; the self-call fallback would be silently dropped.
	if (const char* reason = unimplemented_global_reason(expr->function_name)) {
		error_at("'" + expr->function_name + "' is not supported: " + reason, expr,
			"Calling it would compile to self." + expr->function_name +
			"(), which Godot accepts and silently ignores");
	}

	int self_reg = -1;
	if (m_current_class != nullptr && native_base(*m_current_class) != nullptr) {
		if (Variable* self = find_variable(func, "self")) {
			self_reg = gen_native_base_load(self->register_num, func);
		}
	}
	if (self_reg < 0) {
		self_reg = gen_get_node(".", func);
	}

	int result_reg = alloc_register(func);

	IRInstruction vcall_instr(IROpcode::VCALL);
	vcall_instr.operands.push_back(IRValue::reg(result_reg));
	vcall_instr.operands.push_back(IRValue::reg(self_reg));
	vcall_instr.operands.push_back(ir_str(expr->function_name));
	vcall_instr.operands.push_back(IRValue::imm(arg_regs.size()));
	for (int arg_reg : arg_regs) {
		vcall_instr.operands.push_back(IRValue::reg(arg_reg));
	}
	func.ir.instructions.push_back(vcall_instr);

	free_register(func, self_reg);
	for (int reg : arg_regs) {
		free_register(func, reg);
	}

	return result_reg;
}

void CodeGenerator::gen_builtin_method(const BuiltinMethod& method, int result_reg,
	int obj_reg, const std::vector<int>& arg_regs, FunctionContext& func)
{
	const int size_reg = method.empty_test ? alloc_register(func) : result_reg;

	switch (method.lowering) {
		case MethodLowering::ARRAY_SIZE:
		case MethodLowering::STRING_SIZE: {
			if (method.lowering == MethodLowering::STRING_SIZE &&
				func.string_character_registers.count(obj_reg) != 0)
			{
				auto& one = func.ir.instructions.emplace_back(IROpcode::LOAD_IMM,
					IRValue::reg(size_reg), IRValue::imm(1));
				one.type_hint = Variant::INT;
			} else {
				IRInstruction call(IROpcode::CALL_SYSCALL);
				call.operands.push_back(IRValue::reg(size_reg));
				call.operands.push_back(IRValue::imm(
					method.lowering == MethodLowering::ARRAY_SIZE ? ECALL_ARRAY_SIZE : ECALL_STRING_SIZE));
				call.operands.push_back(IRValue::reg(obj_reg));
				call.type_hint = Variant::INT;
				func.ir.instructions.push_back(call);
			}
			break;
		}
		case MethodLowering::DICT_OP: {
			IRInstruction call(IROpcode::CALL_SYSCALL);
			call.operands.push_back(IRValue::reg(size_reg));
			call.operands.push_back(IRValue::imm(ECALL_DICTIONARY_OPS));
			call.operands.push_back(IRValue::imm(method.op));
			call.operands.push_back(IRValue::reg(obj_reg));
			for (int reg : arg_regs) {
				call.operands.push_back(IRValue::reg(reg));
			}
			call.type_hint = method.empty_test ? Variant::INT : method.result_type;
			func.ir.instructions.push_back(call);
			break;
		}
		case MethodLowering::NONE:
			break;
	}

	if (method.empty_test) {
		set_register_type(func, size_reg, Variant::INT);
		int zero_reg = gen_int_immediate(0, func);
		auto& cmp = func.ir.instructions.emplace_back(IROpcode::CMP_EQ, IRValue::reg(result_reg),
			IRValue::reg(size_reg), IRValue::reg(zero_reg));
		cmp.type_hint = Variant::INT;
		free_register(func, zero_reg);
		free_register(func, size_reg);
	}

	if (!method.has_result) {
		// VCALL answers nil for void methods; match that so the slot is not stale.
		func.ir.instructions.emplace_back(IROpcode::LOAD_NIL, IRValue::reg(result_reg));
		return;
	}
	if (method.result_type != Variant::NIL) {
		set_register_type(func, result_reg, method.result_type);
	}
}

static constexpr const char* NATIVE_BASE_KEY = "@base";

int CodeGenerator::gen_member_call(const MemberCallExpr* expr, FunctionContext& func) {
	VariableExpr chain_object("");
	const Expr* object_expr = expr->object.get();
	if (const std::string* member = chain_qualified_member(object_expr, func)) {
		chain_object.name = *member;
		chain_object.line = object_expr->line;
		chain_object.column = object_expr->column;
		object_expr = &chain_object;
	}
	if (!expr->is_method_call && expr->arguments.empty()) {
		// Trait.Enum.MEMBER is a compile-time namespace, including a qualified
		// cross-file trait name such as Traits.Damageable.State.DEAD.
		std::function<bool(const Expr*, std::string&)> qualified_name =
			[&](const Expr* value, std::string& out) {
				if (auto* variable = dynamic_cast<const VariableExpr*>(value)) {
					if (find_variable(func, variable->name) != nullptr) return false;
					out = variable->name;
					return true;
				}
				auto* member = dynamic_cast<const MemberCallExpr*>(value);
				if (member == nullptr || member->is_method_call || !member->arguments.empty() ||
					!qualified_name(member->object.get(), out)) return false;
				out += "." + member->member_name;
				return true;
			};
		std::string enum_name;
		if (qualified_name(object_expr, enum_name)) {
			if (auto found = m_enums.find(enum_name); found != m_enums.end()) {
				const EnumDecl::Member* member = found->second->find_member(expr->member_name);
				if (member == nullptr) error_at("Enum '" + enum_name +
					"' has no member named '" + expr->member_name + "'", expr);
				return gen_enum_member(*member, func);
			}
		}

		// A trait-typed value also exposes its enum namespace (`value.State.DEAD`).
		if (auto* qualified = dynamic_cast<const MemberCallExpr*>(object_expr)) {
			if (auto* owner = dynamic_cast<const VariableExpr*>(qualified->object.get());
				owner != nullptr && !qualified->is_method_call && qualified->arguments.empty()) {
				std::vector<const TraitDecl*> candidates;
				if (Variable* local = find_variable(func, owner->name)) {
					if (auto found = func.register_traits.find(local->register_num);
						found != func.register_traits.end())
						candidates.insert(candidates.end(), found->second.begin(), found->second.end());
					if (auto found = func.declared_traits.find(local->register_num);
						found != func.declared_traits.end())
						candidates.insert(candidates.end(), found->second.begin(), found->second.end());
				} else if (auto global = m_global_variables.find(owner->name);
					global != m_global_variables.end() && global->second < m_global_traits.size() &&
					m_global_traits[global->second] != nullptr) {
					candidates.push_back(m_global_traits[global->second]);
				}
				const EnumDecl* resolved = nullptr;
				std::unordered_set<const TraitDecl*> seen;
				std::function<void(const TraitDecl*)> find_enum = [&](const TraitDecl* trait) {
					if (trait == nullptr || !seen.insert(trait).second) return;
					for (const EnumDecl& decl : trait->enums) {
						if (decl.name != qualified->member_name) continue;
						if (resolved != nullptr && resolved != &decl)
							error_at("Trait enum '" + qualified->member_name +
								"' is ambiguous for '" + owner->name + "'", expr);
						resolved = &decl;
					}
					for (const std::string& dependency : trait->uses) find_enum(find_trait(dependency));
				};
				for (const TraitDecl* trait : candidates) find_enum(trait);
				if (resolved != nullptr) {
					const EnumDecl::Member* member = resolved->find_member(expr->member_name);
					if (member == nullptr) error_at("Enum '" + resolved->name +
						"' has no member named '" + expr->member_name + "'", expr);
					return gen_enum_member(*member, func);
				}
			}
		}

		// `Variant.Type.TYPE_INT`: a global enum whose name carries a dot. Read
		// before the qualifier fold below, which would leave only `Variant`.
		if (auto* qualified = dynamic_cast<const MemberCallExpr*>(object_expr)) {
			if (auto* outer = dynamic_cast<const VariableExpr*>(qualified->object.get())) {
				if (!qualified->is_method_call && qualified->arguments.empty() &&
					find_variable(func, outer->name) == nullptr)
				{
					const std::string dotted = outer->name + "." + qualified->member_name;
					if (is_global_enum(dotted) && !is_global_variable(outer->name) &&
						!is_global_class(outer->name) && !is_autoload(outer->name))
					{
						return gen_global_enum_value(dotted, expr->member_name, func, expr);
					}
				}
			}
		}
		if (const VariableExpr* owner = engine_enum_qualifier(object_expr, func)) {
			object_expr = owner;
		}
	}

	// Struct.new(): resolved before object is lowered (struct is a type, not a value).
	if (expr->is_method_call && expr->member_name == "new") {
		if (auto* object = dynamic_cast<const VariableExpr*>(object_expr)) {
			if (const TraitDecl* iface = find_trait(object->name);
				iface != nullptr && find_variable(func, object->name) == nullptr) {
				error_at("Trait '" + iface->name + "' cannot be constructed", expr);
			}
			if (const StructDecl* decl = find_struct(object->name)) {
				return gen_struct_construct(*decl, expr->arguments, *expr, func, expr);
			}
			const bool names_a_type = !object->name.empty() &&
				object->name[0] >= 'A' && object->name[0] <= 'Z';
			if (names_a_type && find_variable(func, object->name) == nullptr &&
				!is_global_variable(object->name) && !m_enums.count(object->name) &&
				!is_global_class(object->name) && !is_local_function(object->name))
			{
				if (const std::string* path = global_script_class_path(object->name)) {
					return gen_script_class_new(object->name, *path, expr, func);
				}
				return gen_engine_class_new(object->name, expr, func);
			}
		}
	}

	if (auto* object = dynamic_cast<const VariableExpr*>(object_expr)) {
		if (const TraitDecl* trait = find_trait(object->name);
			trait != nullptr && find_variable(func, object->name) == nullptr) {
			if (expr->is_method_call) {
				const FunctionDecl* method = find_trait_method(*trait, expr->member_name);
				if (method == nullptr) error_at("Trait '" + trait->name + "' has no method '" +
					expr->member_name + "'", expr);
				const std::string origin = method->trait_origin.empty() ? trait->name : method->trait_origin;
				const std::string displaced = "@trait." + origin + "." + method->name;
				if (m_current_class != nullptr) {
					const StructDecl* owner = nullptr;
					const FunctionDecl* copy = find_class_method(*m_current_class, displaced, &owner);
					if (copy == nullptr) copy = find_class_method(*m_current_class, method->name, &owner);
					if (copy == nullptr || copy->trait_origin != origin)
						error_at("Trait method '" + trait->name + "." + method->name +
							"' is not available in class '" + m_current_class->name + "'", expr);
					int self_reg = -1;
					if (!copy->is_static) {
						Variable* self = find_variable(func, "self");
						if (self == nullptr) error_at("An instance trait method needs 'self'", expr);
						self_reg = self->register_num;
					}
					return gen_class_method_call(*m_current_class, *copy, *owner, self_reg,
						expr->arguments, *expr, func, expr);
				}
				std::string target = is_local_function(displaced) ? displaced : method->name;
				if (!is_local_function(displaced)) {
					for (const auto& entry : m_local_signatures) {
						const FunctionDecl* candidate = entry.second;
						if (candidate->trait_origin == origin &&
							(candidate->name == method->name || candidate->chain_name == method->name)) {
							target = entry.first;
							break;
						}
					}
				}
				if (!is_local_function(target))
					error_at("Trait method '" + trait->name + "." + method->name +
						"' is not available in this class", expr);
				std::vector<int> args;
				for (const ExprPtr& argument : expr->arguments) args.push_back(gen_expr(argument.get(), func));
				return emit_local_call(target, std::move(args), func, expr);
			}
			if (expr->arguments.empty()) {
				if (const StructField* constant = find_trait_constant(*trait, expr->member_name))
					return gen_expr(constant->default_value.get(), func);
				error_at("Trait '" + trait->name + "' has no constant '" +
					expr->member_name + "'", expr);
			}
		}
		if (names_a_chain_class(object->name, func)) {
			if (expr->is_method_call) {
				if (!is_local_function(expr->member_name)) {
					error_at("'" + object->name + "' declares no '" + expr->member_name + "()'",
						expr, m_chain.merged()
							? "Its body is merged into this program, so '" + expr->member_name +
								"()' would be a function here"
							: "'" + object->name + "' is this script, so '" + expr->member_name +
								"()' would be a function here");
				}
				reject_named_arguments(*expr, "'" + expr->member_name + "'", expr);
				std::vector<int> arg_regs;
				for (const auto& argument : expr->arguments) {
					arg_regs.push_back(gen_expr(argument.get(), func));
				}
				return emit_local_call(expr->member_name, std::move(arg_regs), func, expr);
			}
			if (expr->arguments.empty()) {
				VariableExpr member(expr->member_name);
				member.line = expr->line;
				member.column = expr->column;
				return gen_variable(&member, func);
			}
		}
	}

	// `Class.f()` and `Class.CONST`: the left-hand name is a type, not a value, so
	// neither reaches gen_expr. A static method has no receiver to pass.
	if (auto* object = dynamic_cast<const VariableExpr*>(object_expr)) {
		const StructDecl* decl = find_struct(object->name);
		if (decl != nullptr && find_variable(func, object->name) == nullptr) {
			if (expr->is_method_call) {
				const StructDecl* owner = nullptr;
				if (const FunctionDecl* method =
					find_class_method(*decl, expr->member_name, &owner))
				{
					if (!method->is_static) {
						error_at("'" + decl->name + "." + expr->member_name +
							"()' is one per instance", expr,
							"Call it on one: '" + decl->name + ".new()." +
							expr->member_name + "()'");
					}
					return gen_class_method_call(*decl, *method, *owner, -1, expr->arguments,
						*expr, func, expr);
				}
			} else if (expr->arguments.empty()) {
				if (int reg = gen_class_constant(*decl, expr->member_name, func); reg >= 0) {
					return reg;
				}
				error_at(std::string(decl->is_class ? "Class '" : "Struct '") + decl->name +
					"' declares no constant named '" +
					expr->member_name + "'", expr);
			}
		}
	}

	if (is_super(object_expr, func)) {
		if (int result = gen_super_call(expr, func); result >= 0) {
			return result;
		}
	}
	// Enum member or built-in type constant: the left-hand name is a type.
	// A declared enum is checked first, so `enum Color { RED = 5 }` shadows the
	// built-in Color rather than silently answering with the engine's constant.
	if (!expr->is_method_call && expr->arguments.empty()) {
		if (auto* object = dynamic_cast<const VariableExpr*>(object_expr)) {
			if (find_variable(func, object->name) == nullptr) {
				if (auto found = m_enums.find(object->name); found != m_enums.end()) {
					const EnumDecl* decl = found->second;
					const EnumDecl::Member* member = decl->find_member(expr->member_name);
					if (member == nullptr) {
						error_at("Enum '" + decl->name + "' has no member named '"
							+ expr->member_name + "'", expr);
					}
					return gen_enum_member(*member, func);
				}
				// A project name of the same spelling wins: an autoload or a
				// class_name is a value the script can actually reach, and the
				// enum is only a name the engine happens to also use.
				if (!is_global_variable(object->name) && is_global_enum(object->name) &&
					!is_global_class(object->name) && !is_autoload(object->name) &&
					global_script_class_path(object->name) == nullptr &&
					find_struct(object->name) == nullptr)
				{
					return gen_global_enum_value(object->name, expr->member_name, func, expr);
				}
				if (!is_global_variable(object->name) && has_builtin_constants(object->name)) {
					int reg = gen_builtin_constant(object->name, expr->member_name, func);
					if (reg >= 0) {
						return reg;
					}
					error_at(object->name + " has no constant named '" + expr->member_name + "'", expr);
				}
			}
		}
	}

	if (!expr->is_method_call && expr->arguments.empty() && is_local_function(expr->member_name)) {
		if (auto* object = dynamic_cast<const VariableExpr*>(object_expr)) {
			if (object->name == "self" && find_variable(func, "self") == nullptr) {
				return gen_make_callable(expr->member_name, -1, func);
			}
		}
	}

	// At script scope, `self.f()` names the same guest function as bare `f()`.
	// Nested-class methods have an actual instance receiver and stay on their
	// class-method path below.
	if (expr->is_method_call && m_current_class == nullptr &&
		is_local_function(expr->member_name))
	{
		if (auto* object = dynamic_cast<const VariableExpr*>(object_expr)) {
			if (object->name == "self" && find_variable(func, "self") == nullptr) {
				reject_named_arguments(*expr, "'" + expr->member_name + "'", expr);
				std::vector<int> arg_regs;
				for (const auto& argument : expr->arguments) {
					arg_regs.push_back(gen_expr(argument.get(), func));
				}
				return emit_local_call(expr->member_name, std::move(arg_regs), func, expr);
			}
		}
	}

	// Cross-file script class: instantiate to reach its constants and statics.
	if (auto* object = dynamic_cast<const VariableExpr*>(object_expr)) {
		if (find_variable(func, object->name) == nullptr) {
			if (const std::string* path = global_script_class_path(object->name)) {
				MemberCallExpr constructor(std::make_unique<VariableExpr>(object->name), "new",
					{}, true);
				const int instance = gen_script_class_new(object->name, *path, &constructor, func);
				if (!expr->is_method_call) {
					const int result = gen_member_read(instance, expr->member_name, func, expr);
					free_register(func, instance);
					return result;
				}
				reject_named_arguments(*expr, "'" + expr->member_name + "'", expr);
				std::vector<int> args;
				for (const auto& argument : expr->arguments) args.push_back(gen_expr(argument.get(), func));
				const int result = alloc_register(func);
				IRInstruction call(IROpcode::VCALL);
				call.operands = { IRValue::reg(result), IRValue::reg(instance),
					ir_str(expr->member_name), IRValue::imm(int64_t(args.size())) };
				for (int arg : args) call.operands.push_back(IRValue::reg(arg));
				func.ir.instructions.push_back(std::move(call));
				free_register(func, instance);
				for (int arg : args) free_register(func, arg);
				return result;
			}
		}
	}

	if (auto* object = dynamic_cast<const VariableExpr*>(object_expr)) {
		// Built-in Variant statics (Color.from_hsv, etc.) bypass ClassDB.
		if (expr->is_method_call) {
			const Variant::Type type = names_a_builtin_type(object->name, func);
			if (type != Variant::VARIANT_MAX) {
				std::vector<int> args;
				for (const auto& argument : expr->arguments) args.push_back(gen_expr(argument.get(), func));
				const int receiver = gen_string_value("__safegdscript_static__:" +
					std::to_string(static_cast<int>(type)), func);
				const int result = alloc_register(func);
				IRInstruction call(IROpcode::VCALL);
				call.operands = { IRValue::reg(result), IRValue::reg(receiver),
					ir_str(expr->member_name), IRValue::imm(int64_t(args.size())) };
				for (int arg : args) call.operands.push_back(IRValue::reg(arg));
				func.ir.instructions.push_back(std::move(call));
				free_register(func, receiver);
				for (int arg : args) free_register(func, arg);
				return result;
			}
		}
		if (names_an_engine_type(object->name, func)) {
			if (!expr->is_method_call) {
				return gen_engine_class_constant(object->name, expr->member_name, func);
			}
			if (expr->member_name != "new") {
				return gen_engine_class_static_call(object->name, expr, func);
			}
		}
	}

	reject_named_arguments(*expr, "'" + expr->member_name + "'", expr);

	if (expr->is_method_call) {
		if (const char* owner_method = signal_owner_method(expr->member_name)) {
			const std::string signal_name = signal_name_of(object_expr, func);
			if (!signal_name.empty()) {
				return gen_signal_owner_call(signal_name, owner_method, expr, func);
			}
		}
	}

	int obj_reg = is_super(object_expr, func)
		? gen_get_node(".", func)
		: gen_expr(object_expr, func);

	if (expr->is_method_call && expr->member_name == "copy" && expr->arguments.empty()) {
		if (const StructDecl* structure = get_register_struct(func, obj_reg);
			structure != nullptr && !structure->is_class) {
			const int result_reg = alloc_register(func);
			const int false_reg = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(false_reg),
				IRValue::imm(0));
			set_register_type(func, false_reg, Variant::BOOL);
			IRInstruction duplicate(IROpcode::VCALL);
			duplicate.operands = { IRValue::reg(result_reg), IRValue::reg(obj_reg),
				ir_str("duplicate"), IRValue::imm(1), IRValue::reg(false_reg) };
			func.ir.instructions.push_back(std::move(duplicate));
			set_register_struct(func, result_reg, structure);
			free_register(func, false_reg);
			free_register(func, obj_reg);
			return result_reg;
		}
	}

	if (expr->is_method_call) {
		if (const StructDecl* decl = get_register_struct(func, obj_reg); decl != nullptr) {
			const StructDecl* owner = nullptr;
			if (const FunctionDecl* method = find_class_method(*decl, expr->member_name, &owner)) {
				int result = gen_class_method_call(*decl, *method, *owner, obj_reg,
					expr->arguments, *expr, func, expr);
				free_register(func, obj_reg);
				return result;
			}
			if (native_base(*decl) != nullptr) {
				int base_reg = gen_native_base_load(obj_reg, func);
				free_register(func, obj_reg);
				obj_reg = base_reg;
			}
		} else if (get_register_type(func, obj_reg) == IRInstruction::TypeHint_NONE ||
			get_register_type(func, obj_reg) == Variant::DICTIONARY) {
			for (const auto& [name, structure] : m_structs) {
				(void)name;
				if (!structure->is_class && structure->find_method(expr->member_name) != nullptr) {
					error_at("Cannot call struct method '" + structure->name + "." +
						expr->member_name + "()' on an untyped value", expr,
						"Declare the receiver as '" + structure->name + "'");
				}
			}
		}
	}

	if (expr->is_method_call) {
		const TraitDecl* iface = get_register_trait(func, obj_reg, expr->member_name,
			/*proven_only=*/true);
		if (iface != nullptr) {
			const FunctionDecl* method = find_trait_method(*iface, expr->member_name);
			if (method->is_static) {
				error_at("Static trait method '" + iface->name + "." + method->name +
					"()' cannot be called through an instance", expr);
			}
			std::vector<int> trait_args;
			if (expr->arguments.size() > method->parameters.size()) {
				error_at("'" + iface->name + "." + method->name + "()' takes " +
					std::to_string(method->parameters.size()) + " arguments, got " +
					std::to_string(expr->arguments.size()), expr);
			}
			for (size_t i = 0; i < method->parameters.size(); i++) {
				int arg = -1;
				if (i < expr->arguments.size()) {
					arg = gen_expr(expr->arguments[i].get(), func);
				} else if (method->parameters[i].default_value) {
					arg = gen_expr(method->parameters[i].default_value.get(), func);
				} else {
					error_at("Missing argument '" + method->parameters[i].name +
						"' in call to '" + iface->name + "." + method->name + "()'", expr);
				}
				if (const TraitDecl* expected =
					find_trait(method->parameters[i].type_hint.sole_name())) {
					arg = require_trait_value(arg, *expected,
						"argument '" + method->parameters[i].name + "'", func,
						expr->line, expr->column,
						method->parameters[i].type_hint.nullable);
				} else if (method->parameters[i].type_hint.is_union()) {
					arg = coerce_to_declared_type(arg,
						type_set_from(method->parameters[i].type_hint), func,
						"argument '" + method->parameters[i].name + "'", expr->line,
						expr->column, method->parameters[i].type_hint.to_string());
				} else if (!method->parameters[i].type_hint.empty()) {
					arg = coerce_to_declared_type(arg,
						single_type_from(method->parameters[i].type_hint), func,
						"argument '" + method->parameters[i].name + "'", expr->line,
						expr->column);
				}
				trait_args.push_back(arg);
			}
			int result_reg = alloc_register(func);
			IRInstruction call(IROpcode::VCALL);
			call.operands = { IRValue::reg(result_reg), IRValue::reg(obj_reg),
				ir_str(method->name), IRValue::imm(int64_t(trait_args.size())) };
			for (int arg : trait_args) call.operands.push_back(IRValue::reg(arg));
			func.ir.instructions.push_back(std::move(call));
			apply_declared_type(result_reg, method->return_type, func);
			free_register(func, obj_reg);
			for (int arg : trait_args) free_register(func, arg);
			return result_reg;
		}
		if (func.trait_only_registers.count(obj_reg)) {
			// Declared but unproven: the slot is nullable, or never assigned.
			if (const TraitDecl* declared =
					get_register_trait(func, obj_reg, expr->member_name)) {
				error_at("Cannot call '" + declared->name + "." + expr->member_name +
					"()' on a value that may be null", expr,
					"Check it against null (or with 'is " + declared->name +
					"') before calling");
			}
			if (const TraitDecl* declared = get_register_trait(func, obj_reg)) {
				error_at("'" + declared->name + "' has no method '" +
					expr->member_name + "'", expr);
			}
		}
	}

	std::vector<int> arg_regs;
	for (const auto& arg : expr->arguments) {
		arg_regs.push_back(gen_expr(arg.get(), func));
	}

	if (!expr->is_method_call && arg_regs.empty()) {
		const TraitDecl* member_trait = nullptr;
		const VarDeclStmt* trait_var = nullptr;
		const SignalDecl* trait_signal = nullptr;
		auto inspect = [&](const auto& set) {
			for (const TraitDecl* trait : set) {
				if ((trait_var = find_trait_var(*trait, expr->member_name)) != nullptr ||
					(trait_signal = find_trait_signal(*trait, expr->member_name)) != nullptr ||
					find_trait_constant(*trait, expr->member_name) != nullptr) {
					member_trait = trait;
					return true;
				}
			}
			return false;
		};
		if (auto known = func.register_traits.find(obj_reg); known != func.register_traits.end())
			inspect(known->second);
		if (member_trait == nullptr) {
			if (auto declared = func.declared_traits.find(obj_reg); declared != func.declared_traits.end())
				inspect(declared->second);
		}
		if (member_trait != nullptr) {
			if (const StructField* constant = find_trait_constant(*member_trait, expr->member_name)) {
				free_register(func, obj_reg);
				return gen_expr(constant->default_value.get(), func);
			}
		}
		int result = gen_member_read(obj_reg, expr->member_name, func, expr);
		if (trait_var != nullptr) apply_declared_type(result, trait_var->type_hint, func);
		if (trait_signal != nullptr) set_register_type(func, result, Variant::SIGNAL);
		if (member_trait == nullptr && func.trait_only_registers.count(obj_reg)) {
			if (const TraitDecl* trait = get_register_trait(func, obj_reg)) {
				error_at("'" + trait->name + "' has no member '" +
					expr->member_name + "'", expr);
			}
		}
		free_register(func, obj_reg);
		return result;
	}

	int result_reg = alloc_register(func);

	if (expr->is_method_call) {
		const BuiltinMethod method = find_builtin_method(
			get_register_type(func, obj_reg), expr->member_name, arg_regs.size());
		if (method.valid()) {
			gen_builtin_method(method, result_reg, obj_reg, arg_regs, func);
			if (auto it = func.array_element_structs.find(obj_reg);
				it != func.array_element_structs.end()) {
				const std::string& called = expr->member_name;
				if (called == "front" || called == "back" || called == "pop_back" ||
					called == "pop_front" || called == "pick_random") {
					set_register_struct(func, result_reg, it->second);
				}
			}
			if (auto it = func.array_element_traits.find(obj_reg);
				it != func.array_element_traits.end()) {
				const std::string& called = expr->member_name;
				if (called == "front" || called == "back" || called == "pop_back" ||
					called == "pop_front" || called == "pick_random") {
					require_trait_value(result_reg, *it->second,
						"an element of Array[" + it->second->name + "]", func,
						expr->line, expr->column);
					func.trait_only_registers.insert(result_reg);
				}
			}
			if (expr->member_name == "values") {
				if (auto it = func.dictionary_value_structs.find(obj_reg);
					it != func.dictionary_value_structs.end()) {
					func.array_element_structs[result_reg] = it->second;
				}
				if (auto it = func.dictionary_value_traits.find(obj_reg);
					it != func.dictionary_value_traits.end()) {
					func.array_element_traits[result_reg] = it->second;
				}
			}
			free_register(func, obj_reg);
			for (int reg : arg_regs) {
				free_register(func, reg);
			}
			return result_reg;
		}
	}

	// Array.append: dedicated syscall, avoids StringName lookup per element.
	if (expr->is_method_call && arg_regs.size() == 1 &&
		(expr->member_name == "append" || expr->member_name == "push_back") &&
		get_register_type(func, obj_reg) == Variant::ARRAY)
	{
		if (auto it = func.array_element_structs.find(obj_reg);
			it != func.array_element_structs.end()) {
			arg_regs[0] = require_struct_value(arg_regs[0], *it->second,
				"an element of Array[" + it->second->name + "]", func,
					expr->line, expr->column);
		}
		if (auto it = func.array_element_traits.find(obj_reg);
			it != func.array_element_traits.end()) {
			arg_regs[0] = require_trait_value(arg_regs[0], *it->second,
				"an element of Array[" + it->second->name + "]", func,
				expr->line, expr->column);
		}
		func.ir.instructions.emplace_back(IROpcode::ARRAY_APPEND, IRValue::reg(result_reg),
			IRValue::reg(obj_reg), IRValue::reg(arg_regs[0]));
		free_register(func, obj_reg);
		free_register(func, arg_regs[0]);
		return result_reg;
	}
	if (expr->is_method_call) {
		if (auto it = func.array_element_structs.find(obj_reg);
			it != func.array_element_structs.end()) {
			int value_index = -1;
			if ((expr->member_name == "push_front" || expr->member_name == "append" ||
				expr->member_name == "push_back") && arg_regs.size() == 1) {
				value_index = 0;
			} else if (expr->member_name == "insert" && arg_regs.size() == 2) {
				value_index = 1;
			}
			if (value_index >= 0) {
				arg_regs[size_t(value_index)] = require_struct_value(
					arg_regs[size_t(value_index)], *it->second,
					"an element of Array[" + it->second->name + "]", func,
					expr->line, expr->column);
			}
		}
		if (auto it = func.array_element_traits.find(obj_reg);
			it != func.array_element_traits.end()) {
			int value_index = -1;
			if ((expr->member_name == "push_front" || expr->member_name == "append" ||
				expr->member_name == "push_back") && arg_regs.size() == 1) {
				value_index = 0;
			} else if (expr->member_name == "insert" && arg_regs.size() == 2) {
				value_index = 1;
			}
			if (value_index >= 0) {
				arg_regs[size_t(value_index)] = require_trait_value(
					arg_regs[size_t(value_index)], *it->second,
					"an element of Array[" + it->second->name + "]", func,
					expr->line, expr->column);
			}
		}
	}

	IRInstruction vcall_instr(IROpcode::VCALL);
	vcall_instr.operands.push_back(IRValue::reg(result_reg));
	vcall_instr.operands.push_back(IRValue::reg(obj_reg));
	vcall_instr.operands.push_back(ir_str(expr->member_name));
	vcall_instr.operands.push_back(IRValue::imm(arg_regs.size()));
	for (int arg_reg : arg_regs) {
		vcall_instr.operands.push_back(IRValue::reg(arg_reg));
	}
	func.ir.instructions.push_back(std::move(vcall_instr));

	free_register(func, obj_reg);
	for (int reg : arg_regs) {
		free_register(func, reg);
	}

	return result_reg;
}

// Both ARRAY and INT types required: backend reads Variants directly, no check.
bool CodeGenerator::is_array_element_access(int obj_reg, int idx_reg, FunctionContext& func) {
	return get_register_type(func, obj_reg) == Variant::ARRAY &&
		get_register_type(func, idx_reg) == Variant::INT;
}

int CodeGenerator::gen_index(const IndexExpr* expr, FunctionContext& func) {
	int obj_reg = gen_expr(expr->object.get(), func);
	const StructDecl* element_struct = nullptr;
	const TraitDecl* element_trait = nullptr;
	if (auto it = func.array_element_structs.find(obj_reg);
		it != func.array_element_structs.end()) {
		element_struct = it->second;
	} else if (auto it = func.dictionary_value_structs.find(obj_reg);
		it != func.dictionary_value_structs.end()) {
		element_struct = it->second;
	}
	if (auto it = func.array_element_traits.find(obj_reg);
		it != func.array_element_traits.end()) element_trait = it->second;
	else if (auto it = func.dictionary_value_traits.find(obj_reg);
		it != func.dictionary_value_traits.end()) element_trait = it->second;
	check_struct_subscript(obj_reg, expr->index.get(), func);
	int idx_reg = -1;
	int result_reg = -1;
	if (!gen_constant_key_read(obj_reg, expr->index.get(), func, result_reg)) {
		idx_reg = gen_expr(expr->index.get(), func);
		result_reg = gen_element_read(obj_reg, idx_reg, func, expr);
	}
	if (element_struct != nullptr) {
		set_register_struct(func, result_reg, element_struct);
	}
	if (element_trait != nullptr) {
		result_reg = require_trait_value(result_reg, *element_trait,
			"an element of a typed container", func, expr->line, expr->column);
		func.trait_only_registers.insert(result_reg);
	}

	free_register(func, obj_reg);
	if (idx_reg >= 0) {
		free_register(func, idx_reg);
	}

	return result_reg;
}

int CodeGenerator::gen_array_literal(const ArrayLiteralExpr* expr, FunctionContext& func) {
	std::vector<int> elem_regs;
	for (const auto& elem : expr->elements) {
		int reg = gen_expr(elem.get(), func);
		elem_regs.push_back(reg);
	}

	int result_reg = alloc_register(func);

	IRInstruction instr(IROpcode::MAKE_ARRAY);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::imm(static_cast<int>(elem_regs.size())));
	for (int reg : elem_regs) {
		instr.operands.push_back(IRValue::reg(reg));
	}

	func.ir.instructions.push_back(instr);
	set_register_type(func, result_reg, Variant::ARRAY);

	for (int reg : elem_regs) {
		free_register(func, reg);
	}

	return result_reg;
}

int CodeGenerator::gen_dictionary_literal(const DictionaryLiteralExpr* expr, FunctionContext& func) {
	std::vector<int> key_regs;
	std::vector<int> value_regs;
	for (const auto& [key, value] : expr->elements) {
		int key_reg = gen_expr(key.get(), func);
		int value_reg = gen_expr(value.get(), func);
		key_regs.push_back(key_reg);
		value_regs.push_back(value_reg);
	}

	int result_reg = alloc_register(func);

	IRInstruction instr(IROpcode::MAKE_DICTIONARY);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::imm(static_cast<int>(key_regs.size())));
	for (size_t i = 0; i < key_regs.size(); i++) {
		instr.operands.push_back(IRValue::reg(key_regs[i]));
		instr.operands.push_back(IRValue::reg(value_regs[i]));
	}

	func.ir.instructions.push_back(instr);
	set_register_type(func, result_reg, Variant::DICTIONARY);

	for (int reg : key_regs) {
		free_register(func, reg);
	}
	for (int reg : value_regs) {
		free_register(func, reg);
	}

	return result_reg;
}

// @export needs both setter and getter; synthesize a storage pass-through for the missing half.
void CodeGenerator::emit_missing_export_accessors(IRProgram& ir_program) {
	for (size_t i = 0; i < ir_program.globals.size(); i++) {
		IRGlobalVar& global = ir_program.globals[i];
		if (!global.publishes_to_host()) {
			continue;
		}
		const bool has_setter = !global.setter_function.empty();
		const bool has_getter = !global.getter_function.empty();
		if (has_setter == has_getter) {
			continue;
		}

		IRFunction fn;
		fn.max_registers = 1;
		if (has_setter) {
			fn.name = "@" + global.name + "_getter";
			fn.instructions.emplace_back(IROpcode::LOAD_GLOBAL,
				IRValue::reg(IRFunction::RETURN_REGISTER), IRValue::imm(static_cast<int64_t>(i)));
			global.getter_function = fn.name;
		} else {
			fn.name = "@" + global.name + "_setter";
			fn.parameters.push_back("value");
			fn.instructions.emplace_back(IROpcode::STORE_GLOBAL,
				IRValue::imm(static_cast<int64_t>(i)), IRValue::reg(0));
			fn.instructions.emplace_back(IROpcode::LOAD_NIL,
				IRValue::reg(IRFunction::RETURN_REGISTER));
			global.setter_function = fn.name;
		}
		fn.instructions.emplace_back(IROpcode::RETURN);

		FunctionSignature signature;
		signature.name = fn.name;
		ir_program.signatures.push_back(std::move(signature));
		ir_program.functions.push_back(std::move(fn));
	}
}


void CodeGenerator::collect_property_accessors(const Program& program) {
	m_global_setters.assign(program.globals.size(), std::string());
	m_global_getters.assign(program.globals.size(), std::string());
	m_accessor_properties.clear();
	m_direct_globals.clear();

	for (size_t i = 0; i < program.globals.size(); i++) {
		const auto& global = program.globals[i];
		if (!global.has_accessors()) {
			continue;
		}

		const auto require_named = [&](const std::string& fn, size_t arity, const char* role,
			bool may_suspend) {
			auto sig = m_local_signatures.find(fn);
			if (sig == m_local_signatures.end()) {
				error_at("Property '" + global.name + "' names '" + fn + "' as its " + role +
					", but this script declares no such function", global.line, global.column);
			}
			if (sig->second->parameters.size() != arity) {
				error_at("The " + std::string(role) + " '" + fn + "' of property '" + global.name +
					"' must take " + (arity == 1 ? "one parameter, the assigned value"
					                             : "no parameters"),
					global.line, global.column);
			}
			if (sig->second->is_coroutine && !may_suspend) {
				error_at("The " + std::string(role) + " '" + fn + "' of property '" + global.name +
					"' contains an await, so it cannot run on a property access",
					global.line, global.column);
			}
		};
		if (!global.setter_name.empty()) {
			// A derived method may override a named base setter with a coroutine.
			// Godot starts that coroutine and lets the property assignment return;
			// CALL_HOSTED below provides the same fire-and-resume behavior.
			require_named(global.setter_name, 1, "setter", true);
		}
		if (!global.getter_name.empty()) {
			require_named(global.getter_name, 0, "getter", false);
		}

		m_global_setters[i] = global.setter_body ? global.setter_body->name : global.setter_name;
		m_global_getters[i] = global.getter_body ? global.getter_body->name : global.getter_name;

		for (const std::string& fn : { m_global_setters[i], m_global_getters[i] }) {
			if (!fn.empty()) {
				m_accessor_properties[fn].push_back(i);
			}
		}
	}
}

// Inside its own accessor, the property name means storage (no recursion).
void CodeGenerator::enter_accessor_scope(const std::string& function_name) {
	m_direct_globals.clear();
	auto it = m_accessor_properties.find(function_name);
	if (it == m_accessor_properties.end()) {
		return;
	}
	m_direct_globals.insert(it->second.begin(), it->second.end());
}

const std::string& CodeGenerator::global_setter(size_t index) const {
	static const std::string none;
	return m_direct_globals.count(index) ? none : m_global_setters[index];
}

const std::string& CodeGenerator::global_getter(size_t index) const {
	static const std::string none;
	return m_direct_globals.count(index) ? none : m_global_getters[index];
}

int CodeGenerator::gen_property_get(size_t index, FunctionContext& func) {
	// Untyped: getter return may differ from declared type.
	const int result_reg = alloc_register(func);
	IRInstruction call(IROpcode::CALL);
	call.operands.push_back(ir_str(m_global_getters[index]));
	call.operands.push_back(IRValue::reg(result_reg));
	call.operands.push_back(IRValue::imm(0));
	func.ir.instructions.push_back(call);
	return result_reg;
}

void CodeGenerator::gen_property_set(size_t index, int value_reg, FunctionContext& func) {
	const int result_reg = alloc_register(func);
	auto sig = m_local_signatures.find(m_global_setters[index]);
	const bool hosted = sig != m_local_signatures.end() && sig->second->is_coroutine;
	IRInstruction call(hosted ? IROpcode::CALL_HOSTED : IROpcode::CALL);
	call.operands.push_back(ir_str(m_global_setters[index]));
	call.operands.push_back(IRValue::reg(result_reg));
	call.operands.push_back(IRValue::imm(1));
	call.operands.push_back(IRValue::reg(value_reg));
	func.ir.instructions.push_back(call);
	free_register(func, result_reg);
}

bool CodeGenerator::type_hint_names_a_class(const std::string& type_hint) const {
	if (type_hint.empty()) {
		return false;
	}
	if (type_hint_from_string(type_hint) != IRInstruction::TypeHint_NONE) {
		return false;
	}
	return find_struct(type_hint) == nullptr && find_trait(type_hint) == nullptr &&
		m_enums.find(type_hint) == m_enums.end();
}

void CodeGenerator::mark_global_holds_object(int64_t global_idx) {
	if (global_idx >= 0 && size_t(global_idx) < m_global_holds_object.size()) {
		m_global_holds_object[size_t(global_idx)] = true;
	}
}

const StructDecl* CodeGenerator::find_struct(const std::string& name) const {
	auto it = m_structs.find(name);
	return it == m_structs.end() ? nullptr : it->second;
}

const TraitDecl* CodeGenerator::find_trait(const std::string& name) const {
	auto it = m_traits.find(name);
	return it == m_traits.end() ? nullptr : it->second;
}

const FunctionDecl* CodeGenerator::find_trait_method(const TraitDecl& trait,
	const std::string& name) const {
	std::unordered_set<const TraitDecl*> seen;
	std::function<const FunctionDecl*(const TraitDecl&)> find = [&](const TraitDecl& at) {
		if (!seen.insert(&at).second) return static_cast<const FunctionDecl*>(nullptr);
		if (const FunctionDecl* own = at.find_method(name)) return own;
		for (const std::string& dependency : at.uses)
			if (const TraitDecl* next = find_trait(dependency))
				if (const FunctionDecl* found = find(*next)) return found;
		return static_cast<const FunctionDecl*>(nullptr);
	};
	return find(trait);
}

const VarDeclStmt* CodeGenerator::find_trait_var(const TraitDecl& trait,
	const std::string& name) const {
	std::unordered_set<const TraitDecl*> seen;
	std::function<const VarDeclStmt*(const TraitDecl&)> find = [&](const TraitDecl& at) {
		if (!seen.insert(&at).second) return static_cast<const VarDeclStmt*>(nullptr);
		if (const VarDeclStmt* own = at.find_var(name)) return own;
		for (const std::string& dependency : at.uses)
			if (const TraitDecl* next = find_trait(dependency))
				if (const VarDeclStmt* found = find(*next)) return found;
		return static_cast<const VarDeclStmt*>(nullptr);
	};
	return find(trait);
}

const StructField* CodeGenerator::find_trait_constant(const TraitDecl& trait,
	const std::string& name) const {
	std::unordered_set<const TraitDecl*> seen;
	std::function<const StructField*(const TraitDecl&)> find = [&](const TraitDecl& at) {
		if (!seen.insert(&at).second) return static_cast<const StructField*>(nullptr);
		if (const StructField* own = at.find_constant(name)) return own;
		for (const std::string& dependency : at.uses)
			if (const TraitDecl* next = find_trait(dependency))
				if (const StructField* found = find(*next)) return found;
		return static_cast<const StructField*>(nullptr);
	};
	return find(trait);
}

const SignalDecl* CodeGenerator::find_trait_signal(const TraitDecl& trait,
	const std::string& name) const {
	std::unordered_set<const TraitDecl*> seen;
	std::function<const SignalDecl*(const TraitDecl&)> find = [&](const TraitDecl& at) {
		if (!seen.insert(&at).second) return static_cast<const SignalDecl*>(nullptr);
		if (const SignalDecl* own = at.find_signal(name)) return own;
		for (const std::string& dependency : at.uses)
			if (const TraitDecl* next = find_trait(dependency))
				if (const SignalDecl* found = find(*next)) return found;
		return static_cast<const SignalDecl*>(nullptr);
	};
	return find(trait);
}

std::string CodeGenerator::trait_required_base(const TraitDecl& trait) const {
	const auto satisfies = [&](const std::string& actual, const std::string& required) {
		if (actual == required) return true;
		return engine_class_derives_from(actual, required);
	};
	std::string required;
	std::unordered_set<const TraitDecl*> seen;
	std::function<void(const TraitDecl&)> collect = [&](const TraitDecl& part) {
		if (!seen.insert(&part).second) return;
		if (!part.base_name.empty()) {
			if (required.empty() || satisfies(part.base_name, required)) {
				required = part.base_name;
			} else if (!satisfies(required, part.base_name)) {
				error_at("Trait '" + trait.name + "' requires unrelated base classes '" +
					required + "' and '" + part.base_name + "' through its composition",
					trait.line, trait.column,
					"Compile with engine ancestry metadata to accept a subclass relationship");
			}
		}
		for (const std::string& dependency : part.uses) {
			if (const TraitDecl* next = find_trait(dependency)) collect(*next);
		}
	};
	collect(trait);
	return required;
}

std::vector<const TraitDecl*> CodeGenerator::used_traits(
	const StructDecl& decl) const
{
	std::vector<const TraitDecl*> result;
	std::unordered_set<const TraitDecl*> seen;
	for (const StructDecl* at = &decl; at != nullptr; at = class_base(*at)) {
		for (const std::string& name : at->uses) {
			if (const TraitDecl* iface = find_trait(name); iface != nullptr && seen.insert(iface).second) {
				result.push_back(iface);
			}
		}
	}
	return result;
}

bool CodeGenerator::declaration_uses(const StructDecl& decl,
	const TraitDecl& iface) const
{
	const std::vector<const TraitDecl*> all = used_traits(decl);
	return std::find(all.begin(), all.end(), &iface) != all.end();
}

void CodeGenerator::validate_trait_member(const std::string& kind,
	const std::string& name, const std::vector<FunctionDecl>& methods,
	const StructDecl* decl, const std::vector<std::string>& names,
	int line, int column) const
{
	auto accepts = [&](const TypeExpr& wider, const TypeExpr& narrower) {
		if (wider.empty() || wider.single_name() == "Variant") return true;
		if (narrower.empty() || narrower.single_name() == "Variant") return false;
		if (wider.to_string() == narrower.to_string()) return true;
		// Nominal names do not become compatible merely because both are OBJECT.
		const bool wider_nominal = find_struct(wider.sole_name()) || find_trait(wider.sole_name());
		const bool narrower_nominal = find_struct(narrower.sole_name()) || find_trait(narrower.sole_name());
		if ((wider_nominal || narrower_nominal) && wider.sole_name() != "Object") return false;
		const TypeSet w = type_set_from(wider, line, column);
		const TypeSet n = type_set_from(narrower, line, column);
		return !w.any() && !n.any() && (n.mask & ~w.mask) == 0;
	};

	for (const std::string& trait_name : names) {
		const TraitDecl* iface = find_trait(trait_name);
		if (iface == nullptr) {
			error_at("Trait '" + trait_name + "' is not declared in this compilation",
				line, column);
		}
		for (const FunctionDecl& required : iface->methods) {
			const FunctionDecl* actual = nullptr;
			if (decl != nullptr) {
				actual = find_class_method(*decl, required.name);
			} else {
				for (const FunctionDecl& candidate : methods) {
					if (candidate.name == required.name) { actual = &candidate; break; }
				}
			}
			const std::string prefix = kind + " '" + name + "' uses '" +
				iface->name + "' but '" + required.name;
			if (actual == nullptr) {
				if (required.is_abstract) {
					error_at(kind + " '" + name + "' uses '" + iface->name +
						"' but does not implement abstract method '" + required.name + "()'",
						line, column);
				}
				error_at(prefix + "' is not available", line, column);
			}
			if (actual->is_static != required.is_static) {
				error_at(prefix + (actual->is_static ? "' is static; the trait declares an instance method"
					: "' is an instance method; the trait declares it static"),
					line, column);
			}
			if (actual->parameters.size() != required.parameters.size()) {
				error_at(prefix + "' takes " + std::to_string(actual->parameters.size()) +
					" parameters, trait declares " +
					std::to_string(required.parameters.size()), line, column);
			}
			for (size_t i = 0; i < required.parameters.size(); i++) {
				if (!accepts(actual->parameters[i].type_hint, required.parameters[i].type_hint)) {
					error_at(prefix + "' parameter '" + required.parameters[i].name +
						"' has incompatible type '" + actual->parameters[i].type_hint.to_string() +
						"'; trait declares '" + required.parameters[i].type_hint.to_string() +
						"'", line, column);
				}
			}
			if (!required.return_type.empty() && actual->return_type.empty()) {
				error_at(prefix + "' has an untyped return; trait declares '" +
					required.return_type.to_string() + "'", line, column);
			}
			if (!actual->return_type.empty() && !required.return_type.empty() &&
				!accepts(required.return_type, actual->return_type)) {
				error_at(prefix + "' returns '" + actual->return_type.to_string() +
					"'; trait declares '" + required.return_type.to_string() + "'",
					line, column);
			}
		}
	}
}

void CodeGenerator::validate_uses(const Program& program) const {
	const auto satisfies_base = [&](const std::string& actual, const std::string& required) {
		if (actual == required) return true;
		return engine_class_derives_from(actual, required);
	};
	for (const TraitDecl& trait : program.traits) {
		(void)trait_required_base(trait);
	}
	for (const StructDecl& decl : program.structs) {
		for (const std::string& name : decl.uses) {
			const TraitDecl* trait = find_trait(name);
			if (trait == nullptr || trait->base_name.empty()) continue;
			const std::string* base = native_base(decl);
			if (base == nullptr || !satisfies_base(*base, trait->base_name)) {
				error_at("Class '" + decl.name + "' cannot use trait '" + trait->name +
					"': it requires base '" + trait->base_name + "'",
					decl.line, decl.column);
			}
		}
		validate_trait_member(decl.is_class ? "Class" : "Struct", decl.name,
			decl.methods, &decl, decl.uses, decl.line, decl.column);
	}
	for (const std::string& name : program.uses) {
		const TraitDecl* trait = find_trait(name);
		if (trait == nullptr || trait->base_name.empty()) continue;
		if (!satisfies_base(program.native_base_class, trait->base_name)) {
			error_at("Class '" + (program.class_name.empty() ? std::string("this script") : program.class_name) +
				"' cannot use trait '" + trait->name + "': it requires base '" +
				trait->base_name + "'", program.class_name_line, program.class_name_column);
		}
	}
	validate_trait_member("Class", program.class_name.empty() ? "this script" : program.class_name,
		program.functions, nullptr, program.uses,
		program.class_name_line, program.class_name_column);
}

const StructDecl* CodeGenerator::class_base(const StructDecl& decl) const {
	if (!decl.is_class || decl.base_name.empty()) {
		return nullptr;
	}
	return find_struct(decl.base_name);
}

const std::string* CodeGenerator::native_base(const StructDecl& decl) const {
	for (const StructDecl* at = &decl; at != nullptr; at = class_base(*at)) {
		if (auto found = m_native_bases.find(at); found != m_native_bases.end()) {
			return &found->second;
		}
	}
	return nullptr;
}

int CodeGenerator::gen_native_base_load(int self_reg, FunctionContext& func) {
	int base_reg = gen_dict_get(self_reg, NATIVE_BASE_KEY, func);
	set_register_type(func, base_reg, Variant::OBJECT);
	return base_reg;
}

void CodeGenerator::reject_static_member_access(const std::string& name,
	int line, int column) const
{
	if (!m_in_static_function) {
		return;
	}
	auto it = m_global_variables.find(name);
	if (it == m_global_variables.end() || it->second >= m_global_is_member.size() ||
		!m_global_is_member[it->second]) {
		return;
	}
	error_at("Cannot use the member variable '" + name + "' from a 'static func'",
		line, column,
		"'" + name + "' is one per instance, and a static function runs without one. "
		"Declare it 'static var', or make this an instance function");
}

// A bare name that resolves to nothing in the script is a property of whatever
// the script extends, as it is in GDScript. Inside a lifted class method that
// is the class's `@base`; in a top-level function it is the owner, which the
// script's own `extends` names. Without an `extends` there is nothing to reach
// and the caller reports the name as undefined.
int CodeGenerator::gen_implicit_base_load(FunctionContext& func) {
	if (m_in_static_function) {
		return -1;
	}
	if (m_current_class != nullptr) {
		if (native_base(*m_current_class) == nullptr) {
			return -1;
		}
		Variable* self = find_variable(func, "self");
		return self == nullptr ? -1 : gen_native_base_load(self->register_num, func);
	}
	if (m_script_base_class.empty()) {
		return -1;
	}
	return gen_get_node(".", func);
}

std::vector<const StructField*> CodeGenerator::struct_fields(const StructDecl& decl) const {
	std::vector<const StructField*> out;
	if (const StructDecl* base = class_base(decl)) {
		out = struct_fields(*base);
	}
	for (const StructField& field : decl.fields) {
		out.push_back(&field);
	}
	return out;
}

const StructField* CodeGenerator::find_struct_field(const StructDecl& decl,
	const std::string& name) const
{
	for (const StructField* field : struct_fields(decl)) {
		if (field->name == name) {
			return field;
		}
	}
	return nullptr;
}

int CodeGenerator::struct_field_index(const StructDecl& decl, const std::string& name) const {
	const std::vector<const StructField*> fields = struct_fields(decl);
	for (size_t i = 0; i < fields.size(); i++) {
		if (fields[i]->name == name) {
			return int(i);
		}
	}
	return -1;
}

std::string CodeGenerator::struct_field_list(const StructDecl& decl) const {
	std::string list;
	for (const StructField* field : struct_fields(decl)) {
		if (!list.empty()) {
			list += ", ";
		}
		list += field->name;
	}
	return list.empty() ? "(none)" : list;
}

const FunctionDecl* CodeGenerator::find_class_method(const StructDecl& decl,
	const std::string& name, const StructDecl** owner) const
{
	for (const StructDecl* at = &decl; at != nullptr; at = class_base(*at)) {
		if (const FunctionDecl* method = at->find_method(name)) {
			if (owner != nullptr) {
				*owner = at;
			}
			return method;
		}
	}
	return nullptr;
}

// A class constant is compile-time only, like the file's own consts: it folds at
// the use site and nothing of it reaches the IR. Keyed under 'Class.NAME', which
// no source-level name can spell.
void CodeGenerator::register_class_constants(const Program& program) {
	for (const StructDecl& decl : program.structs) {
		for (const StructField& constant : decl.constants) {
			IRGlobalVar folded;
			folded.name = decl.name + "." + constant.name;
			folded.is_const = true;
			if (!constant.type_hint.empty()) {
				folded.type_hint = single_type_from(constant.type_hint);
			}
			if (!fold_global_initializer(constant.default_value.get(), folded, nullptr, &decl)
				|| folded.init_type == IRGlobalVar::InitType::RUNTIME) {
				error_at("The constant '" + decl.name + "." + constant.name +
					"' is not a compile-time value", constant.line, constant.column,
					"A struct or class holds no storage of its own, so its constants have to fold. "
					"Move it to a file-level 'const', or make it a field with a default");
			}
			coerce_folded_initializer(folded, constant.type_hint,
				constant.line, constant.column);
			folded.value_type = derive_global_value_type(folded);
			m_class_constants[folded.name] = std::move(folded);
		}
	}
}

// Walks the declared chain, so a constant is inherited like a field.
int CodeGenerator::gen_class_constant(const StructDecl& decl, const std::string& name,
	FunctionContext& func)
{
	for (const StructDecl* at = &decl; at != nullptr; at = class_base(*at)) {
		auto it = m_class_constants.find(at->name + "." + name);
		if (it != m_class_constants.end()) {
			return gen_folded_const(it->second, func);
		}
	}
	return -1;
}

std::string CodeGenerator::lifted_method_name(const StructDecl& decl, const std::string& method) {
	return "@" + decl.name + "." + method;
}

void CodeGenerator::register_classes(const Program& program) {
	m_native_bases.clear();
	for (const StructDecl& decl : program.structs) {
		if (!decl.is_class || decl.base_name.empty()) {
			continue;
		}
		const StructDecl* base = find_struct(decl.base_name);
		if (base == nullptr) {
			if (m_restricted) {
				error_at("Class '" + decl.name + "' extends '" + decl.base_name +
					"', which needs a Sandbox that allows engine classes", decl.line, decl.column,
					"A restricted Sandbox refuses every class. Either declare '" + decl.base_name +
					"' in this file or drop the 'extends'.");
			}
			if (is_global_class(decl.base_name)) {
				error_at("Class '" + decl.name + "' extends the singleton '" + decl.base_name + "'",
					decl.line, decl.column, "A singleton is one object, not a class to derive from");
			}
			m_native_bases[&decl] = decl.base_name;
			continue;
		}
		if (!base->is_class) {
			error_at("Class '" + decl.name + "' extends struct '" + decl.base_name + "'",
				decl.line, decl.column, "A struct declares no methods to inherit");
		}
		const StructDecl* at = base;
		for (size_t steps = 0; at != nullptr; steps++) {
			if (at == &decl || steps > program.structs.size()) {
				error_at("Class '" + decl.name + "' extends itself through '" +
					decl.base_name + "'", decl.line, decl.column);
			}
			at = (at->is_class && !at->base_name.empty()) ? find_struct(at->base_name) : nullptr;
		}
	}

	for (const StructDecl& decl : program.structs) {
		const StructDecl* base = class_base(decl);
		if (base == nullptr) {
			continue;
		}
		for (const StructField& field : decl.fields) {
			if (find_struct_field(*base, field.name) != nullptr) {
				error_at("Class '" + decl.name + "' redeclares field '" + field.name +
					"', which it inherits from '" + base->name + "'",
					field.line, field.column);
			}
		}
	}
}

const StructField& CodeGenerator::require_struct_field(const StructDecl& decl,
	const std::string& field_name, int line, int column) const
{
	if (const StructField* field = find_struct_field(decl, field_name)) {
		return *field;
	}
	error_at((decl.is_class ? "Class '" : "Struct '") + decl.name + "' has no field '" +
		field_name + "'", line, column,
		"Fields of '" + decl.name + "' are: " + struct_field_list(decl));
}

void CodeGenerator::check_struct_subscript(int obj_reg, const Expr* index, FunctionContext& func) {
	const StructDecl* decl = get_register_struct(func, obj_reg);
	if (decl == nullptr || native_base(*decl) != nullptr) {
		return;
	}
	if (const std::string* key = constant_string(index, func)) {
		require_struct_field(*decl, *key, index->line, index->column);
	}
}

void CodeGenerator::set_register_struct(FunctionContext& func, int reg, const StructDecl* decl) {
	if (decl == nullptr) {
		func.register_structs.erase(reg);
		return;
	}
	func.register_structs[reg] = decl;
	func.trait_only_registers.erase(reg);
	for (const TraitDecl* iface : used_traits(*decl)) {
		add_register_trait(func, reg, iface);
	}
	set_register_type(func, reg, Variant::DICTIONARY);
}

const StructDecl* CodeGenerator::get_register_struct(const FunctionContext& func, int reg) const {
	auto it = func.register_structs.find(reg);
	return it == func.register_structs.end() ? nullptr : it->second;
}

void CodeGenerator::add_register_trait(FunctionContext& func, int reg,
	const TraitDecl* decl)
{
	if (decl != nullptr) func.register_traits[reg].insert(decl);
}

const TraitDecl* CodeGenerator::get_register_trait(const FunctionContext& func,
	int reg, const std::string& method, bool proven_only) const
{
	auto it = func.register_traits.find(reg);
	if (it != func.register_traits.end()) {
		for (const TraitDecl* iface : it->second) {
			if (method.empty() || find_trait_method(*iface, method) != nullptr) return iface;
		}
	}
	if (proven_only) {
		return nullptr;
	}
	auto declared = func.declared_traits.find(reg);
	if (declared != func.declared_traits.end()) {
		for (const TraitDecl* iface : declared->second) {
			if (method.empty() || find_trait_method(*iface, method) != nullptr) return iface;
		}
	}
	return nullptr;
}

const SignalDecl* CodeGenerator::find_signal(const std::string& name) const {
	auto it = m_signals.find(name);
	return it == m_signals.end() ? nullptr : it->second;
}

void CodeGenerator::reject_signal_collision(const std::string& what, const std::string& name,
	int line, int column) const
{
	if (m_signals.count(name) == 0) {
		return;
	}
	error_at(what + " '" + name + "' has the same name as a signal in this script", line, column,
		"'" + name + "' already names the signal declared in this script");
}

// All parameters required; an emitter supplies every argument.
FunctionSignature CodeGenerator::build_signal_signature(const SignalDecl& decl) const {
	FunctionSignature sig;
	sig.name = decl.name;
	sig.line = decl.line;
	sig.description = decl.doc_comment;
	sig.return_type = int32_t(FunctionParameter::ANY_TYPE);

	for (const Parameter& param : decl.parameters) {
		FunctionParameter out;
		out.name = param.name;
		out.type = published_type_from(param.type_hint);
		sig.parameters.push_back(std::move(out));
	}

	sig.required_arguments = sig.parameters.size();
	return sig;
}

// Lowers to self.get(name), for the uses that need the Signal itself: await, or
// passing it on. One scoped variant per use.
int CodeGenerator::gen_signal_value(const std::string& name, FunctionContext& func,
	const Expr* site)
{
	int self_reg = gen_get_node(".", func);
	int result_reg = gen_member_read(self_reg, name, func, site);
	free_register(func, self_reg);
	set_register_type(func, result_reg, Variant::SIGNAL);
	return result_reg;
}

// `sig` and `self.sig`, where sig is a declared signal that no local shadows.
std::string CodeGenerator::signal_name_of(const Expr* expr, FunctionContext& func) {
	if (auto* var = dynamic_cast<const VariableExpr*>(expr)) {
		if (find_variable(func, var->name) == nullptr && find_signal(var->name) != nullptr) {
			return var->name;
		}
		return {};
	}
	if (auto* member = dynamic_cast<const MemberCallExpr*>(expr)) {
		auto* object = dynamic_cast<const VariableExpr*>(member->object.get());
		if (!member->is_method_call && member->arguments.empty() &&
			object != nullptr && object->name == "self" &&
			find_variable(func, object->name) == nullptr &&
			find_signal(member->member_name) != nullptr)
		{
			return member->member_name;
		}
	}
	return {};
}

int CodeGenerator::class_field_self(const std::string& name, FunctionContext& func) {
	if (m_current_class == nullptr || find_struct_field(*m_current_class, name) == nullptr) {
		return -1;
	}
	Variable* self = find_variable(func, "self");
	return self != nullptr ? self->register_num : -1;
}

bool CodeGenerator::is_super(const Expr* expr, FunctionContext& func) {
	auto* var = dynamic_cast<const VariableExpr*>(expr);
	return var != nullptr && var->name == "super" && find_variable(func, "super") == nullptr;
}

// The Object equivalent, taking the signal name first and the same arguments after.
const char* CodeGenerator::signal_owner_method(const std::string& member) {
	if (member == "emit") return "emit_signal";
	if (member == "connect") return "connect";
	if (member == "disconnect") return "disconnect";
	if (member == "is_connected") return "is_connected";
	return nullptr;
}

// sig.emit(a) becomes self.emit_signal("sig", a). Variant::SIGNAL is not inlined into
// a GuestVariant, so a materialised Signal costs a scoped variant, capped per call at
// Sandbox::MAX_REFS and never recycled; the read is a host call, so nothing hoists it
// out of a loop. Also moves the restriction check from is_allowed_property on each
// signal name to is_allowed_method on emit_signal/connect.
int CodeGenerator::gen_signal_owner_call(const std::string& signal_name,
	const char* owner_method, const MemberCallExpr* expr, FunctionContext& func)
{
	int self_reg = gen_get_node(".", func);

	int name_reg = alloc_register(func);
	IRInstruction load_name(IROpcode::LOAD_STRING, IRValue::reg(name_reg),
		IRValue::imm(add_string_constant(signal_name)));
	load_name.type_hint = Variant::STRING;
	func.ir.instructions.push_back(load_name);
	set_register_type(func, name_reg, Variant::STRING);

	std::vector<int> arg_regs{ name_reg };
	for (const auto& arg : expr->arguments) {
		arg_regs.push_back(gen_expr(arg.get(), func));
	}

	int result_reg = alloc_register(func);
	IRInstruction vcall(IROpcode::VCALL);
	vcall.operands.push_back(IRValue::reg(result_reg));
	vcall.operands.push_back(IRValue::reg(self_reg));
	vcall.operands.push_back(ir_str(owner_method));
	vcall.operands.push_back(IRValue::imm(int64_t(arg_regs.size())));
	for (int reg : arg_regs) {
		vcall.operands.push_back(IRValue::reg(reg));
	}
	func.ir.instructions.push_back(vcall);

	free_register(func, self_reg);
	for (int reg : arg_regs) {
		free_register(func, reg);
	}

	// Signal.emit() answers null; Object.emit_signal() answers an error code.
	if (owner_method == std::string_view("emit_signal")) {
		IRInstruction nil(IROpcode::LOAD_NIL, IRValue::reg(result_reg));
		nil.type_hint = Variant::NIL;
		func.ir.instructions.push_back(nil);
		set_register_type(func, result_reg, Variant::NIL);
	}
	return result_reg;
}

FunctionSignature CodeGenerator::build_signature(const FunctionDecl& decl) const {
	FunctionSignature sig;
	sig.name = decl.name;
	sig.line = decl.line;
	sig.description = decl.doc_comment;
	sig.is_coroutine = decl.is_coroutine;
	sig.is_static = decl.is_static;
	// Coroutine return type is Variant (may be Signal or declared type).
	sig.return_type = decl.is_coroutine
		? int32_t(FunctionParameter::ANY_TYPE)
		: published_type_from(decl.return_type);
	if (!decl.is_coroutine && find_struct(decl.return_type.single_name()) != nullptr) {
		sig.return_class_name = decl.return_type.single_name();
	} else if (!decl.is_coroutine) {
		if (const TraitDecl* trait = find_trait(decl.return_type.sole_name()); trait != nullptr)
			sig.return_class_name = trait_required_base(*trait);
	}

	for (const Parameter& param : decl.parameters) {
		FunctionParameter out;
		out.name = param.name;
		out.type = published_type_from(param.type_hint);
		if (find_struct(param.type_hint.single_name()) != nullptr) {
			out.class_name = param.type_hint.single_name();
		} else if (const TraitDecl* trait = find_trait(param.type_hint.sole_name()); trait != nullptr) {
			out.class_name = trait_required_base(*trait);
		}

		IRGlobalVar folded;
		if (param.default_value && fold_global_initializer(param.default_value.get(), folded)) {
			using InitType = IRGlobalVar::InitType;
			using DefaultKind = FunctionParameter::DefaultKind;
			switch (folded.init_type) {
				case InitType::INT: out.default_kind = DefaultKind::INT; break;
				case InitType::FLOAT: out.default_kind = DefaultKind::FLOAT; break;
				case InitType::BOOL: out.default_kind = DefaultKind::BOOL; break;
				case InitType::STRING: out.default_kind = DefaultKind::STRING; break;
				case InitType::NULL_VAL: out.default_kind = DefaultKind::NIL; break;
				case InitType::EMPTY_ARRAY: out.default_kind = DefaultKind::EMPTY_ARRAY; break;
				case InitType::EMPTY_DICT: out.default_kind = DefaultKind::EMPTY_DICT; break;
				case InitType::NONE:
				case InitType::RUNTIME: break;
			}
			out.default_value = folded.init_value;
		}
		sig.parameters.push_back(std::move(out));
	}

	sig.required_arguments = sig.parameters.size();
	while (sig.required_arguments > 0 && sig.parameters[sig.required_arguments - 1].optional()) {
		sig.required_arguments--;
	}
	return sig;
}

ClassSignature CodeGenerator::build_class_signature(const StructDecl& decl,
	const std::string& engine_base) const
{
	ClassSignature out;
	out.name = decl.name;
	out.base_name = class_base(decl) != nullptr ? decl.base_name : std::string();
	out.native_base = engine_base;
	out.line = decl.line;
	out.is_struct = !decl.is_class;
	out.description = decl.doc_comment;
	for (const TraitDecl* iface : used_traits(decl)) {
		out.uses.push_back(iface->name);
	}
	for (const StructField* field : struct_fields(decl)) {
		ClassField published;
		published.name = field->name;
		published.type = published_type_from(field->type_hint);
		if (find_struct(field->type_hint.single_name()) != nullptr) {
			published.class_name = field->type_hint.single_name();
		} else if (const TraitDecl* trait = find_trait(field->type_hint.sole_name()); trait != nullptr) {
			published.class_name = trait_required_base(*trait);
		}
		published.description = field->doc_comment;
		out.fields.push_back(std::move(published));
	}
	for (const FunctionDecl& method : decl.methods) {
		out.methods.push_back(ClassMethod{ method.name, method.is_static });
	}
	return out;
}

ClassSignature CodeGenerator::build_trait_signature(const TraitDecl& decl) const {
	ClassSignature out;
	out.name = decl.name;
	out.line = decl.line;
	out.description = decl.doc_comment;
	out.is_trait = true;
	out.native_base = trait_required_base(decl);
	std::unordered_set<const TraitDecl*> seen;
	std::vector<const TraitDecl*> composition;
	std::function<void(const TraitDecl&)> collect = [&](const TraitDecl& trait) {
		for (const std::string& name : trait.uses) {
			const TraitDecl* dependency = find_trait(name);
			if (dependency != nullptr && seen.insert(dependency).second) {
				collect(*dependency);
				out.uses.push_back(dependency->name);
				composition.push_back(dependency);
			}
		}
	};
	collect(decl);
	composition.push_back(&decl);
	std::unordered_set<std::string> methods;
	std::unordered_set<std::string> fields;
	std::unordered_set<std::string> signals;
	for (const TraitDecl* part : composition) {
		for (const FunctionDecl& method : part->methods) {
			if (!methods.insert(method.name).second) continue;
			out.methods.push_back(ClassMethod{ method.name, method.is_static });
			out.trait_methods.push_back(build_signature(method));
		}
		for (const VarDeclStmt& variable : part->vars) {
			if (!fields.insert(variable.name).second) continue;
			ClassField field;
			field.name = variable.name;
			field.type = published_type_from(variable.type_hint);
			if (const TraitDecl* trait = find_trait(variable.type_hint.sole_name()); trait != nullptr)
				field.class_name = trait_required_base(*trait);
			field.description = variable.doc_comment;
			out.trait_fields.push_back(std::move(field));
		}
		for (const SignalDecl& signal : part->signals) {
			if (signals.insert(signal.name).second)
				out.trait_signals.push_back(build_signal_signature(signal));
		}
	}
	return out;
}

void CodeGenerator::apply_declared_type(int reg, const TypeExpr& type_hint, FunctionContext& func) {
	if (type_hint.empty()) {
		return;
	}
	if (type_hint.is_union()) {
		func.declared_sets[reg] = type_set_from(type_hint);
		if (const TraitDecl* iface = find_trait(type_hint.sole_name())) {
			func.declared_traits[reg].insert(iface);
			func.trait_only_registers.insert(reg);
			if (!type_hint.nullable) {
				add_register_trait(func, reg, iface);
			}
		}
		if (const StructDecl* decl = find_struct(type_hint.sole_name())) {
			func.declared_structs[reg] = decl;
		}
		set_register_type(func, reg, IRInstruction::TypeHint_NONE);
		func.register_structs.erase(reg);
		return;
	}
	if (const TraitDecl* iface = find_trait(type_hint.single_name())) {
		func.declared_traits[reg].insert(iface);
		add_register_trait(func, reg, iface);
		func.trait_only_registers.insert(reg);
		set_register_type(func, reg, IRInstruction::TypeHint_NONE);
		return;
	}
	if (const StructDecl* decl = find_struct(type_hint.single_name())) {
		set_register_struct(func, reg, decl);
		return;
	}
	const IRInstruction::TypeHint type = single_type_from(type_hint);
	if (type != IRInstruction::TypeHint_NONE) {
		set_register_type(func, reg, type);
	}
	if (type_hint.single_name() == "Array" && type_hint.arguments.size() == 1) {
		if (const StructDecl* element = find_struct(type_hint.arguments[0].single_name())) {
			func.array_element_structs[reg] = element;
		}
		if (const TraitDecl* element = find_trait(type_hint.arguments[0].sole_name())) {
			func.array_element_traits[reg] = element;
		}
	} else if (type_hint.single_name() == "Dictionary" && type_hint.arguments.size() == 2) {
		if (const StructDecl* value = find_struct(type_hint.arguments[1].single_name())) {
			func.dictionary_value_structs[reg] = value;
		}
		if (const TraitDecl* value = find_trait(type_hint.arguments[1].sole_name())) {
			func.dictionary_value_traits[reg] = value;
		}
	}
}

void CodeGenerator::coerce_parameters(const std::vector<Parameter>& parameters,
	FunctionContext& func)
{
	for (const auto& param : parameters) {
		if (const TraitDecl* iface = find_trait(param.type_hint.sole_name())) {
			Variable* var = find_variable(func, param.name);
			if (var != nullptr && m_struct_checks) {
				func.register_traits.erase(var->register_num);
				func.trait_only_registers.erase(var->register_num);
				require_trait_value(var->register_num, *iface,
					"parameter '" + param.name + "'", func, param.line, param.column,
					param.type_hint.nullable);
			}
			if (var != nullptr) {
				func.declared_traits[var->register_num].insert(iface);
				if (!param.type_hint.nullable) add_register_trait(func, var->register_num, iface);
				func.trait_only_registers.insert(var->register_num);
			}
			continue;
		}
		if (const StructDecl* structure = find_struct(param.type_hint.single_name());
			structure != nullptr && !structure->is_class) {
			Variable* var = find_variable(func, param.name);
			if (var != nullptr && m_struct_checks) {
				// The annotation established tracking for code generation, but a host
				// caller has not proved the Dictionary shape yet.
				func.register_structs.erase(var->register_num);
				func.register_types.erase(var->register_num);
				require_struct_value(var->register_num, *structure,
					"Argument '" + param.name + "'", func, param.line, param.column);
			}
			if (var != nullptr) func.declared_structs[var->register_num] = structure;
			continue;
		}
		if (param.type_hint.is_union()) {
			Variable* var = find_variable(func, param.name);
			if (var != nullptr) {
				const TypeSet declared =
					type_set_from(param.type_hint, param.line, param.column);
				// The annotation established the register's declared set, but a host
				// caller has not proved it yet, so this guard must not elide itself.
				func.declared_sets.erase(var->register_num);
				coerce_to_declared_type(var->register_num, declared, func,
					"parameter '" + param.name + "'", param.line, param.column,
					param.type_hint.to_string());
				func.declared_sets[var->register_num] = declared;
			}
			continue;
		}
		const IRInstruction::TypeHint declared = single_type_from(param.type_hint);
		if (declared != Variant::INT && declared != Variant::FLOAT && declared != Variant::BOOL) {
			continue;
		}
		Variable* var = find_variable(func, param.name);
		if (var == nullptr) {
			continue;
		}
		const int src = var->register_num;
		const int dst = alloc_register(func);
		IRInstruction coerce(IROpcode::COERCE, IRValue::reg(dst), IRValue::reg(src));
		coerce.type_hint = declared;
		func.ir.instructions.push_back(coerce);
		// The parameter lives in the coerced register from here on, and that is
		// where an assignment to it lands: the debugger has to read it there, and
		// the declared type belongs to the same slot.
		if (var->debug_index < func.ir.debug_locals.size()) {
			func.ir.debug_locals[var->debug_index].register_num = dst;
		}
		set_register_type(func, dst, declared);
		var->register_num = dst;
	}
}

void CodeGenerator::reject_named_arguments(const NamedArguments& names, const std::string& what,
	const Expr* site) const
{
	for (const auto& name : names.argument_names) {
		if (name.empty()) {
			continue;
		}
		error_at("Named arguments are only supported when constructing a struct, and " +
			what + " is not one", site,
			"Pass the value by position, without '" + name + " ='");
	}
}

int CodeGenerator::gen_dict_get(int obj_reg, const std::string& key, FunctionContext& func) {
	int result_reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::DICT_GET_CONST, IRValue::reg(result_reg),
		IRValue::reg(obj_reg), IRValue::imm(add_string_constant(key)));
	return result_reg;
}

int CodeGenerator::gen_dict_has(int obj_reg, const std::string& key, FunctionContext& func) {
	int result_reg = alloc_register(func);
	IRInstruction has(IROpcode::DICT_HAS_CONST, IRValue::reg(result_reg),
		IRValue::reg(obj_reg), IRValue::imm(add_string_constant(key)));
	has.type_hint = Variant::BOOL;
	func.ir.instructions.push_back(std::move(has));
	set_register_type(func, result_reg, Variant::BOOL);
	return result_reg;
}

void CodeGenerator::gen_dict_set(int obj_reg, const std::string& key, int value_reg,
	FunctionContext& func)
{
	func.ir.instructions.emplace_back(IROpcode::DICT_SET_CONST, IRValue::reg(obj_reg),
		IRValue::imm(add_string_constant(key)), IRValue::reg(value_reg));
}

const std::string* CodeGenerator::constant_dictionary_key(int obj_reg, const Expr* index,
	FunctionContext& func) const
{
	if (index == nullptr || get_register_type(func, obj_reg) != Variant::DICTIONARY) {
		return nullptr;
	}
	// &"x" / ^"x" are StringName / NodePath, not String.
	const auto* literal = dynamic_cast<const LiteralExpr*>(index);
	if (literal == nullptr || literal->lit_type != LiteralExpr::Type::STRING ||
		literal->string_type != LiteralExpr::StringType::PLAIN) {
		return nullptr;
	}
	return &std::get<std::string>(literal->value);
}

bool CodeGenerator::gen_constant_key_read(int obj_reg, const Expr* index, FunctionContext& func,
	int& result_reg)
{
	const std::string* key = constant_dictionary_key(obj_reg, index, func);
	if (key == nullptr) {
		return false;
	}
	result_reg = gen_dict_get(obj_reg, *key, func);
	return true;
}

bool CodeGenerator::gen_constant_key_store(int obj_reg, const Expr* index, int value_reg,
	FunctionContext& func)
{
	const std::string* key = constant_dictionary_key(obj_reg, index, func);
	if (key == nullptr) {
		return false;
	}
	// String key (not StringName) to match what GDScript would store.
	func.ir.instructions.emplace_back(IROpcode::DICT_SET_CONST_STR, IRValue::reg(obj_reg),
		IRValue::imm(add_string_constant(*key)), IRValue::reg(value_reg));
	return true;
}

int CodeGenerator::gen_default_value(const TypeExpr& type_hint, FunctionContext& func) {
	if (type_hint.is_union()) {
		if (type_hint.nullable) {
			int reg = alloc_register(func);
			IRInstruction nil(IROpcode::LOAD_NIL, IRValue::reg(reg));
			nil.type_hint = Variant::NIL;
			func.ir.instructions.push_back(nil);
			set_register_type(func, reg, Variant::NIL);
			return reg;
		}
		TypeExpr first;
		first.names.push_back(type_hint.names.front());
		int reg = gen_default_value(first, func);
		if (reg >= 0 || type_set_from(first).only() != Variant::OBJECT) {
			return reg;
		}
		reg = alloc_register(func);
		IRInstruction nil(IROpcode::LOAD_NIL, IRValue::reg(reg));
		nil.type_hint = Variant::NIL;
		func.ir.instructions.push_back(nil);
		set_register_type(func, reg, Variant::NIL);
		return reg;
	}
	const std::string& name = type_hint.single_name();
	switch (single_type_from(type_hint)) {
		case Variant::INT: {
			int reg = alloc_register(func);
			IRInstruction load(IROpcode::LOAD_IMM, IRValue::reg(reg), IRValue::imm(0));
			load.type_hint = Variant::INT;
			func.ir.instructions.push_back(load);
			set_register_type(func, reg, Variant::INT);
			return reg;
		}
		case Variant::FLOAT: {
			int reg = alloc_register(func);
			IRInstruction load(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(reg), IRValue::fimm(0.0));
			load.type_hint = Variant::FLOAT;
			func.ir.instructions.push_back(load);
			set_register_type(func, reg, Variant::FLOAT);
			return reg;
		}
		case Variant::BOOL: {
			int reg = alloc_register(func);
			IRInstruction load(IROpcode::LOAD_BOOL, IRValue::reg(reg), IRValue::imm(0));
			load.type_hint = Variant::BOOL;
			func.ir.instructions.push_back(load);
			set_register_type(func, reg, Variant::BOOL);
			return reg;
		}
		case Variant::STRING: {
			int reg = alloc_register(func);
			IRInstruction load(IROpcode::LOAD_STRING, IRValue::reg(reg),
				IRValue::imm(add_string_constant("")));
			load.type_hint = Variant::STRING;
			func.ir.instructions.push_back(load);
			set_register_type(func, reg, Variant::STRING);
			return reg;
		}
		default:
			break;
	}

	// Guest-constructible: zero-argument form.
	if (is_inline_primitive_constructor(name)) {
		return gen_inline_constructor(name, {}, func, nullptr);
	}
	if (is_host_constructor(name)) {
		return gen_host_constructor(name, {}, func, nullptr);
	}

	// No guest-constructible default.
	return -1;
}

int CodeGenerator::gen_field_default(const StructDecl& decl, const StructField& field,
	FunctionContext& func)
{
	if (field.default_value) {
		int reg = gen_expr(field.default_value.get(), func);
		if (const TraitDecl* iface = find_trait(field.type_hint.sole_name())) {
			reg = require_trait_value(reg, *iface,
				"field '" + field.name + "' of struct '" + decl.name + "'", func,
				field.line, field.column, field.type_hint.nullable);
		} else if (!field.type_hint.empty() && find_struct(field.type_hint.single_name()) == nullptr) {
			reg = field.type_hint.is_union()
				? coerce_to_declared_type(reg,
					type_set_from(field.type_hint, field.line, field.column), func,
					"field '" + field.name + "' of struct '" + decl.name + "'",
					field.line, field.column, field.type_hint.to_string())
				: coerce_to_declared_type(reg, single_type_from(field.type_hint), func,
					"field '" + field.name + "' of struct '" + decl.name + "'",
					field.line, field.column);
		}
		apply_declared_type(reg, field.type_hint, func);
		return reg;
	}

	// Nested struct: default to a fresh instance. A field declared with a class
	// type holds an object, and GDScript leaves that null.
	if (const StructDecl* nested = find_struct(field.type_hint.single_name()); nested != nullptr && !nested->is_class) {
		for (const StructDecl* active : m_struct_default_stack) {
			if (active != nested) {
				continue;
			}
			error_at("Struct '" + nested->name + "' contains itself through field '" +
				decl.name + "." + field.name + "'", field.line, field.column,
				"A struct is a value, so it cannot hold one of its own type by default. "
				"Give the field a default value such as 'null'.");
		}
		m_struct_default_stack.push_back(nested);
		int reg = gen_struct_construct(*nested, {}, NamedArguments{}, func, nullptr);
		m_struct_default_stack.pop_back();
		return reg;
	}

	int reg = gen_default_value(field.type_hint, func);
	if (reg >= 0) {
		return reg;
	}

	// No constructible default; NIL.
	reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(reg), IRValue::imm(0));
	return reg;
}

int CodeGenerator::gen_class_construct(const StructDecl& decl, const std::vector<ExprPtr>& arguments,
	const NamedArguments& names, FunctionContext& func, const Expr* site)
{
	const std::vector<const StructField*> fields = struct_fields(decl);

	m_struct_default_stack.push_back(&decl);
	std::vector<int> value_regs;
	value_regs.reserve(fields.size());
	for (const StructField* field : fields) {
		value_regs.push_back(gen_field_default(decl, *field, func));
	}
	m_struct_default_stack.pop_back();

	const std::string* base_class = native_base(decl);
	int base_reg = -1;
	int base_key_reg = -1;
	if (base_class != nullptr) {
		base_reg = alloc_register(func);
		IRInstruction create(IROpcode::CALL_SYSCALL);
		create.operands.push_back(IRValue::reg(base_reg));
		create.operands.push_back(IRValue::imm(ECALL_NODE_CREATE));
		create.operands.push_back(IRValue::imm(add_string_constant(*base_class)));
		create.operands.push_back(IRValue::imm(static_cast<int64_t>(base_class->length())));
		func.ir.instructions.push_back(create);
		set_register_type(func, base_reg, Variant::OBJECT);
	}

	// A class instance carries the name it was made from, so a value the compiler
	// no longer tracks -- one read back out of a container, an untyped parameter --
	// still answers `is`. A struct is a plain Dictionary and carries nothing.
	int class_reg = -1;
	int class_key_reg = -1;
	if (decl.is_class) {
		class_key_reg = alloc_register(func);
		IRInstruction load_key(IROpcode::LOAD_STRING, IRValue::reg(class_key_reg),
			IRValue::imm(add_string_constant(CLASS_NAME_KEY)));
		load_key.type_hint = Variant::STRING;
		func.ir.instructions.push_back(load_key);
		set_register_type(func, class_key_reg, Variant::STRING);

		class_reg = alloc_register(func);
		IRInstruction load_name(IROpcode::LOAD_STRING, IRValue::reg(class_reg),
			IRValue::imm(add_string_constant(decl.name)));
		load_name.type_hint = Variant::STRING;
		func.ir.instructions.push_back(load_name);
		set_register_type(func, class_reg, Variant::STRING);
	}

	const size_t entries = fields.size() + (base_class != nullptr ? 1 : 0)
		+ (class_reg >= 0 ? 1 : 0);
	int result_reg = alloc_register(func);
	IRInstruction make(IROpcode::MAKE_DICTIONARY);
	make.operands.push_back(IRValue::reg(result_reg));
	make.operands.push_back(IRValue::imm(static_cast<int>(entries)));

	std::vector<int> key_regs;
	if (class_reg >= 0) {
		make.operands.push_back(IRValue::reg(class_key_reg));
		make.operands.push_back(IRValue::reg(class_reg));
	}
	if (base_class != nullptr) {
		base_key_reg = alloc_register(func);
		IRInstruction load_key(IROpcode::LOAD_STRING, IRValue::reg(base_key_reg),
			IRValue::imm(add_string_constant(NATIVE_BASE_KEY)));
		load_key.type_hint = Variant::STRING;
		func.ir.instructions.push_back(load_key);
		set_register_type(func, base_key_reg, Variant::STRING);
		make.operands.push_back(IRValue::reg(base_key_reg));
		make.operands.push_back(IRValue::reg(base_reg));
	}
	for (size_t i = 0; i < fields.size(); i++) {
		int key_reg = alloc_register(func);
		IRInstruction load_key(IROpcode::LOAD_STRING, IRValue::reg(key_reg),
			IRValue::imm(add_string_constant(fields[i]->name)));
		load_key.type_hint = Variant::STRING;
		func.ir.instructions.push_back(load_key);
		set_register_type(func, key_reg, Variant::STRING);
		key_regs.push_back(key_reg);

		make.operands.push_back(IRValue::reg(key_reg));
		make.operands.push_back(IRValue::reg(value_regs[i]));
	}

	make.type_hint = Variant::DICTIONARY;
	func.ir.instructions.push_back(make);
	set_register_type(func, result_reg, Variant::DICTIONARY);
	set_register_struct(func, result_reg, &decl);

	for (int reg : key_regs) {
		free_register(func, reg);
	}
	for (int reg : value_regs) {
		free_register(func, reg);
	}
	if (base_key_reg >= 0) {
		free_register(func, base_key_reg);
	}
	if (base_reg >= 0) {
		free_register(func, base_reg);
	}
	if (class_key_reg >= 0) {
		free_register(func, class_key_reg);
	}
	if (class_reg >= 0) {
		free_register(func, class_reg);
	}

	// Before _init(), so the class is a script instance while _init() runs, the
	// way add_child(self) inside GDScript's _init() already works.
	if (base_class != nullptr) {
		int bind_reg = alloc_register(func);
		IRInstruction bind(IROpcode::CALL_SYSCALL);
		bind.operands.push_back(IRValue::reg(bind_reg));
		bind.operands.push_back(IRValue::imm(ECALL_CLASS_BIND));
		bind.operands.push_back(IRValue::imm(add_string_constant(decl.name)));
		bind.operands.push_back(IRValue::imm(static_cast<int64_t>(decl.name.length())));
		bind.operands.push_back(IRValue::reg(result_reg));
		func.ir.instructions.push_back(bind);
		free_register(func, bind_reg);
	}

	const StructDecl* owner = nullptr;
	const FunctionDecl* init = find_class_method(decl, "_init", &owner);
	if (init == nullptr) {
		if (!arguments.empty()) {
			error_at("Class '" + decl.name + "' declares no _init(), so new() takes no arguments",
				site);
		}
		return result_reg;
	}

	int discarded = gen_class_method_call(decl, *init, *owner, result_reg, arguments, names,
		func, site);
	free_register(func, discarded);
	return result_reg;
}

// True when every `return` in the body is a bare `self` and the body ends in
// one, so no path answers anything else. Falling off the end answers null,
// which is why the last statement has to be one of the returns.
static bool every_return_is_self(const std::vector<StmtPtr>& body) {
	for (const StmtPtr& stmt : body) {
		if (auto* ret = dynamic_cast<const ReturnStmt*>(stmt.get())) {
			auto* value = dynamic_cast<const VariableExpr*>(ret->value.get());
			if (value == nullptr || value->name != "self") {
				return false;
			}
		} else if (auto* if_stmt = dynamic_cast<const IfStmt*>(stmt.get())) {
			if (!every_return_is_self(if_stmt->then_branch)
				|| !every_return_is_self(if_stmt->else_branch)) {
				return false;
			}
		} else if (auto* while_stmt = dynamic_cast<const WhileStmt*>(stmt.get())) {
			if (!every_return_is_self(while_stmt->body)) {
				return false;
			}
		} else if (auto* for_stmt = dynamic_cast<const ForStmt*>(stmt.get())) {
			if (!every_return_is_self(for_stmt->body)) {
				return false;
			}
		} else if (auto* match_stmt = dynamic_cast<const MatchStmt*>(stmt.get())) {
			for (const auto& branch : match_stmt->branches) {
				if (!every_return_is_self(branch.body)) {
					return false;
				}
			}
		}
	}
	return true;
}

static bool returns_only_self(const std::vector<StmtPtr>& body) {
	if (body.empty()) {
		return false;
	}
	auto* last = dynamic_cast<const ReturnStmt*>(body.back().get());
	if (last == nullptr) {
		return false;
	}
	return every_return_is_self(body);
}

int CodeGenerator::gen_class_method_call(const StructDecl& decl, const FunctionDecl& method,
	const StructDecl& owner, int self_reg, const std::vector<ExprPtr>& arguments,
	const NamedArguments& names, FunctionContext& func, const Expr* site)
{
	for (size_t i = 0; i < arguments.size(); i++) {
		if (!names.argument_name(i).empty()) {
			error_at("'" + decl.name + "." + method.name + "()' takes no named arguments", site,
				"Only a struct names its arguments");
		}
	}
	if (arguments.size() > method.parameters.size()) {
		error_at("Too many arguments to '" + decl.name + "." + method.name + "()': expected at most " +
			std::to_string(method.parameters.size()) + ", got " + std::to_string(arguments.size()),
			site);
	}

	// A static method has no receiver, so reaching it through an instance
	// (`S.new().unit()`) must not shift every argument by one.
	if (method.is_static) {
		self_reg = -1;
	}

	std::vector<int> arg_regs;
	if (self_reg >= 0) {
		arg_regs.push_back(self_reg);
	}
	for (const auto& argument : arguments) {
		arg_regs.push_back(gen_expr(argument.get(), func));
	}
	for (size_t i = arguments.size(); i < method.parameters.size(); i++) {
		if (!method.parameters[i].default_value) {
			error_at("Missing argument '" + method.parameters[i].name + "' in call to '" +
				decl.name + "." + method.name + "()'", site);
		}
		arg_regs.push_back(gen_expr(method.parameters[i].default_value.get(), func));
	}

	int result_reg = alloc_register(func);
	if (!method.is_coroutine) {
		apply_declared_type(result_reg, method.return_type, func);
		// A method that only ever returns `self` answers the receiver, so the call
		// site keeps tracking the instance and `a.bump().bump()` stays a pair of
		// direct calls instead of a VCALL on a Dictionary.
		if (self_reg >= 0 && method.return_type.empty() && returns_only_self(method.body)) {
			set_register_struct(func, result_reg, get_register_struct(func, self_reg));
		}
	}

	IRInstruction call(method.is_coroutine ? IROpcode::CALL_HOSTED : IROpcode::CALL);
	call.operands.push_back(ir_str(lifted_method_name(owner, method.name)));
	call.operands.push_back(IRValue::reg(result_reg));
	call.operands.push_back(IRValue::imm(int64_t(arg_regs.size())));
	for (int reg : arg_regs) {
		call.operands.push_back(IRValue::reg(reg));
	}
	func.ir.instructions.push_back(call);

	for (size_t i = 1; i < arg_regs.size(); i++) {
		free_register(func, arg_regs[i]);
	}
	return result_reg;
}

int CodeGenerator::gen_chain_super_call(const std::string& name,
	const std::vector<ExprPtr>& arguments, const NamedArguments& names,
	FunctionContext& func, const Expr* site)
{
	const ChainInfo::Origin* origin = m_chain.super_of(name, m_current_chain_link);
	if (origin == nullptr) {
		return -1;
	}
	reject_named_arguments(names, "'" + name + "'", site);
	std::vector<int> arg_regs;
	for (const auto& argument : arguments) {
		arg_regs.push_back(gen_expr(argument.get(), func));
	}
	return emit_local_call(origin->symbol, std::move(arg_regs), func, site);
}

int CodeGenerator::gen_super_call(const MemberCallExpr* expr, FunctionContext& func) {
	if (m_current_class == nullptr) {
		if (!expr->is_method_call) {
			if (m_chain.merged() && is_global_variable(expr->member_name)) {
				VariableExpr member(expr->member_name);
				member.line = expr->line;
				member.column = expr->column;
				return gen_variable(&member, func);
			}
			return -1;
		}
		return gen_chain_super_call(expr->member_name, expr->arguments, *expr, func, expr);
	}
	const StructDecl* base = class_base(*m_current_class);
	const std::string* engine_base = native_base(*m_current_class);
	if (base == nullptr && engine_base == nullptr) {
		error_at("Class '" + m_current_class->name + "' extends nothing, so it has no super", expr,
			"Call '" + expr->member_name + "()' on self instead");
	}
	if (!expr->is_method_call && base != nullptr) {
		error_at("'super." + expr->member_name + "' is the field 'self." + expr->member_name +
			"'", expr, "A class instance holds one value per field, base and derived alike");
	}

	const StructDecl* owner = nullptr;
	const FunctionDecl* method = base != nullptr
		? find_class_method(*base, expr->member_name, &owner) : nullptr;
	if (method == nullptr && engine_base == nullptr) {
		error_at("'" + base->name + "' declares no '" + expr->member_name + "()'", expr);
	}

	Variable* self = find_variable(func, "self");
	if (self == nullptr) {
		error_at("super is only reachable from a class method", expr, script_level_super_hint());
	}
	if (method != nullptr) {
		return gen_class_method_call(*base, *method, *owner, self->register_num, expr->arguments,
			*expr, func, expr);
	}

	int base_reg = gen_native_base_load(self->register_num, func);
	if (!expr->is_method_call) {
		int result_reg = gen_vget(base_reg, expr->member_name, func);
		free_register(func, base_reg);
		return result_reg;
	}
	reject_named_arguments(*expr, "'" + expr->member_name + "'", expr);
	std::vector<int> arg_regs;
	for (const auto& argument : expr->arguments) {
		arg_regs.push_back(gen_expr(argument.get(), func));
	}
	int result_reg = alloc_register(func);
	IRInstruction vcall(IROpcode::VCALL);
	vcall.super_call = true;
	vcall.operands.push_back(IRValue::reg(result_reg));
	vcall.operands.push_back(IRValue::reg(base_reg));
	vcall.operands.push_back(ir_str(expr->member_name));
	vcall.operands.push_back(IRValue::imm(arg_regs.size()));
	for (int arg_reg : arg_regs) {
		vcall.operands.push_back(IRValue::reg(arg_reg));
	}
	func.ir.instructions.push_back(vcall);
	free_register(func, base_reg);
	for (int reg : arg_regs) {
		free_register(func, reg);
	}
	return result_reg;
}

int CodeGenerator::gen_super_init(const CallExpr* expr, FunctionContext& func) {
	if (m_current_class == nullptr) {
		if (m_chain.merged()) {
			if (int result = gen_chain_super_call(m_current_chain_function, expr->arguments,
				*expr, func, expr); result >= 0)
			{
				return result;
			}
			if (!m_script_base_class.empty()) {
				int result_reg = alloc_register(func);
				func.ir.instructions.emplace_back(IROpcode::LOAD_NIL, IRValue::reg(result_reg));
				set_register_type(func, result_reg, Variant::NIL);
				return result_reg;
			}
		}
		error_at("super() has no base to call: a sandboxed program is the whole script", expr,
			script_level_super_hint());
	}
	const StructDecl* base = class_base(*m_current_class);
	if (base == nullptr) {
		if (native_base(*m_current_class) != nullptr) {
			if (!expr->arguments.empty()) {
				error_at("super() takes no arguments: '" + *native_base(*m_current_class) +
					"' is constructed empty", expr);
			}
			int result_reg = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::LOAD_NIL, IRValue::reg(result_reg));
			set_register_type(func, result_reg, Variant::NIL);
			return result_reg;
		}
		error_at("Class '" + m_current_class->name + "' extends nothing, so it has no super()", expr);
	}
	const StructDecl* owner = nullptr;
	const FunctionDecl* init = find_class_method(*base, "_init", &owner);
	if (init == nullptr) {
		error_at("'" + base->name + "' declares no _init(), so there is no super() to call", expr);
	}
	Variable* self = find_variable(func, "self");
	if (self == nullptr) {
		error_at("super() is only reachable from a class method", expr);
	}
	return gen_class_method_call(*base, *init, *owner, self->register_num, expr->arguments,
		*expr, func, expr);
}

int CodeGenerator::gen_struct_construct(const StructDecl& decl, const std::vector<ExprPtr>& arguments,
	const NamedArguments& names, FunctionContext& func, const Expr* site)
{
	if (decl.is_class) {
		return gen_class_construct(decl, arguments, names, func, site);
	}
	int copy_probe_reg = -1;
	if (arguments.size() == 1 && names.argument_name(0).empty()) {
		int source_reg = gen_expr(arguments[0].get(), func);
		if (get_register_struct(func, source_reg) == &decl) {
			const int result_reg = alloc_register(func);
			const int false_reg = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(false_reg),
				IRValue::imm(0));
			set_register_type(func, false_reg, Variant::BOOL);
			IRInstruction duplicate(IROpcode::VCALL);
			duplicate.operands = { IRValue::reg(result_reg), IRValue::reg(source_reg),
				ir_str("duplicate"), IRValue::imm(1), IRValue::reg(false_reg) };
			func.ir.instructions.push_back(std::move(duplicate));
			set_register_struct(func, result_reg, &decl);
			free_register(func, false_reg);
			free_register(func, source_reg);
			return result_reg;
		}
		// Retain the already evaluated value for the ordinary one-field form.
		copy_probe_reg = source_reg;
	}

	const std::vector<const StructField*> fields = struct_fields(decl);

	// Resolve field indices before evaluation; report bad call sites before lowering.
	std::vector<int> field_of_argument(arguments.size(), -1);
	std::vector<bool> field_supplied(fields.size(), false);

	for (size_t i = 0; i < arguments.size(); i++) {
		const Expr* argument = arguments[i].get();
		const std::string& name = names.argument_name(i);
		int field_index = -1;

		if (name.empty()) {
			if (i >= fields.size()) {
				error_at("Too many values constructing '" + decl.name + "': it has " +
					std::to_string(fields.size()) +
					(fields.size() == 1 ? " field" : " fields"), site,
					"Fields of '" + decl.name + "' are: " + struct_field_list(decl));
			}
			field_index = static_cast<int>(i);
		} else {
			require_struct_field(decl, name, argument->line, argument->column);
			field_index = struct_field_index(decl, name);
		}

		if (field_supplied[field_index]) {
			error_at("Field '" + fields[field_index]->name + "' of '" + decl.name +
				"' is given a value twice", argument);
		}
		field_supplied[field_index] = true;
		field_of_argument[i] = field_index;
	}

	// Evaluate in call-site order; named args may differ from field order.
	std::vector<int> value_regs(fields.size(), -1);
	for (size_t i = 0; i < arguments.size(); i++) {
		const StructField& field = *fields[field_of_argument[i]];
		int reg = i == 0 && copy_probe_reg >= 0
			? copy_probe_reg : gen_expr(arguments[i].get(), func);
		if (const TraitDecl* iface = find_trait(field.type_hint.sole_name())) {
			reg = require_trait_value(reg, *iface,
				"field '" + field.name + "' of struct '" + decl.name + "'", func,
				arguments[i]->line, arguments[i]->column, field.type_hint.nullable);
		} else if (!field.type_hint.empty() && find_struct(field.type_hint.single_name()) == nullptr) {
			reg = field.type_hint.is_union()
				? coerce_to_declared_type(reg, type_set_from(field.type_hint), func,
					"field '" + field.name + "' of struct '" + decl.name + "'",
					arguments[i]->line, arguments[i]->column, field.type_hint.to_string())
				: coerce_to_declared_type(reg, single_type_from(field.type_hint), func,
					"field '" + field.name + "' of struct '" + decl.name + "'",
					arguments[i]->line, arguments[i]->column);
		}
		apply_declared_type(reg, field.type_hint, func);
		value_regs[field_of_argument[i]] = reg;
	}

	// Unsupplied fields get declaration defaults; m_struct_default_stack guards recursion.
	m_struct_default_stack.push_back(&decl);
	for (size_t i = 0; i < fields.size(); i++) {
		if (value_regs[i] < 0) {
			value_regs[i] = gen_field_default(decl, *fields[i], func);
		}
	}
	m_struct_default_stack.pop_back();

	int result_reg = alloc_register(func);
	IRInstruction make(IROpcode::MAKE_DICTIONARY_KEYED);
	make.operands.push_back(IRValue::reg(result_reg));
	make.operands.push_back(IRValue::imm(static_cast<int>(fields.size())));
	for (size_t i = 0; i < fields.size(); i++) {
		make.operands.push_back(IRValue::imm(add_string_constant(fields[i]->name)));
		make.operands.push_back(IRValue::reg(value_regs[i]));
	}

	make.type_hint = Variant::DICTIONARY;
	func.ir.instructions.push_back(make);
	set_register_struct(func, result_reg, &decl);

	for (int reg : value_regs) {
		free_register(func, reg);
	}
	return result_reg;
}

int CodeGenerator::alloc_register(FunctionContext& func) {
	return func.next_register++;
}

void CodeGenerator::free_register(FunctionContext& func, int reg) {
	(void) func;
	(void) reg;
}

std::string CodeGenerator::make_label(const std::string& prefix) {
	return prefix + "_" + std::to_string(m_next_label++);
}

int CodeGenerator::add_string_constant(const std::string& str) {
	for (size_t i = 0; i < m_string_constants.size(); i++) {
		if (m_string_constants[i] == str) {
			return static_cast<int>(i);
		}
	}

	m_string_constants.push_back(str);
	return static_cast<int>(m_string_constants.size() - 1);
}

void CodeGenerator::push_scope(FunctionContext& func) {
	Scope new_scope;
	if (func.scopes.empty()) {
		new_scope.parent_scope_idx = SIZE_MAX;
	} else {
		new_scope.parent_scope_idx = func.scopes.size() - 1;
	}
	func.scopes.push_back(new_scope);
}

void CodeGenerator::pop_scope(FunctionContext& func) {
	if (func.scopes.empty()) {
		throw CompilerException(ErrorType::CODEGEN_ERROR, "Cannot pop scope: scope stack is empty");
	}
	for (const auto &entry : func.scopes.back().variables) {
		const size_t index = entry.second.debug_index;
		if (index < func.ir.debug_locals.size()) {
			func.ir.debug_locals[index].end_instruction = func.ir.instructions.size();
		}
	}
	func.scopes.pop_back();
}

CodeGenerator::Variable* CodeGenerator::find_variable(FunctionContext& func, const std::string& name) {
	for (int i = static_cast<int>(func.scopes.size()) - 1; i >= 0; i--) {
		auto it = func.scopes[i].variables.find(name);
		if (it != func.scopes[i].variables.end()) {
			return &it->second;
		}
	}
	return nullptr;
}

void CodeGenerator::reject_reclassification(const Variable& var, int value_reg,
	const FunctionContext& func, const Stmt* site)
{
	const IRInstruction::TypeHint held = get_register_type(func, var.register_num);
	const IRInstruction::TypeHint incoming = get_register_type(func, value_reg);
	if (held == IRInstruction::TypeHint_NONE || incoming == IRInstruction::TypeHint_NONE ||
		held == incoming)
	{
		return;
	}
	// Widenings (int->float, bool->int/float) are not reclassifications.
	if ((held == Variant::FLOAT && incoming == Variant::INT) ||
		(incoming == Variant::BOOL && (held == Variant::INT || held == Variant::FLOAT)))
	{
		return;
	}
	if (constructs_implicitly_from(incoming, held)) {
		return;
	}
	error_at("Variable '" + var.name + "' has type " + std::string(variant_type_name(held)) +
		" and is being assigned a value of type " + std::string(variant_type_name(incoming)) +
		". Reclassification is disabled in SafeGDScript", site,
		"A variable keeps the type it was declared with. GDScript would allow this -- an "
		"untyped 'var' is a Variant there -- but that type is what makes arithmetic, "
		"comparison and 'match' on '" + var.name + "' compile to instructions instead of "
		"host calls. Use a second variable, or declare 'var " + var.name + ": Variant' to "
		"ask for a slot that holds anything");
}

void CodeGenerator::declare_variable(FunctionContext& func, const std::string& name, int register_num, bool is_const,
	const Stmt* site, bool is_variant, bool is_parameter)
{
	if (func.scopes.empty()) {
		throw CompilerException(ErrorType::CODEGEN_ERROR, "Cannot declare variable: no scope active");
	}

	auto& current_scope = func.scopes.back();
	if (current_scope.variables.find(name) != current_scope.variables.end()) {
		error_at("Variable '" + name + "' is already declared in this scope", site);
	}

	IRFunction::DebugLocal debug;
	debug.name = name;
	debug.register_num = register_num;
	debug.begin_instruction = func.ir.instructions.size();
	debug.parameter = is_parameter;
	const size_t debug_index = func.ir.debug_locals.size();
	func.ir.debug_locals.push_back(std::move(debug));
	// A register inherits the declared union of the value copied into it, but a
	// slot that outlives one value does not: the declaration re-adds its own.
	func.declared_sets.erase(register_num);
	current_scope.variables[name] = {name, register_num, IRInstruction::TypeHint_NONE,
			is_const, is_variant, debug_index};
}

void CodeGenerator::set_register_type(FunctionContext& func, int reg, IRInstruction::TypeHint type) {
	func.register_types[reg] = type;
	for (IRFunction::DebugLocal &local : func.ir.debug_locals) {
		if (local.register_num == reg && local.end_instruction == SIZE_MAX) {
			local.type_hint = type;
		}
	}
}

IRInstruction::TypeHint CodeGenerator::get_register_type(const FunctionContext& func, int reg) const {
	auto it = func.register_types.find(reg);
	if (it != func.register_types.end()) {
		return it->second;
	}
	return IRInstruction::TypeHint_NONE;
}

// Inline constructor table. components=0 means container (empty or from Array).
namespace {
struct InlineConstructor {
	const char* name;
	IROpcode opcode;
	IRInstruction::TypeHint variant_type;
	int components;
	bool integer;  // int32 components, not real_t
	double defaults[4];
};

const InlineConstructor INLINE_CONSTRUCTORS[] = {
	{ "Vector2",  IROpcode::MAKE_VECTOR2,  Variant::VECTOR2,  2, false, { 0, 0, 0, 0 } },
	{ "Vector2i", IROpcode::MAKE_VECTOR2I, Variant::VECTOR2I, 2, true,  { 0, 0, 0, 0 } },
	{ "Vector3",  IROpcode::MAKE_VECTOR3,  Variant::VECTOR3,  3, false, { 0, 0, 0, 0 } },
	{ "Vector3i", IROpcode::MAKE_VECTOR3I, Variant::VECTOR3I, 3, true,  { 0, 0, 0, 0 } },
	{ "Vector4",  IROpcode::MAKE_VECTOR4,  Variant::VECTOR4,  4, false, { 0, 0, 0, 0 } },
	{ "Vector4i", IROpcode::MAKE_VECTOR4I, Variant::VECTOR4I, 4, true,  { 0, 0, 0, 0 } },
	{ "Rect2",    IROpcode::MAKE_RECT2,    Variant::RECT2,    4, false, { 0, 0, 0, 0 } },
	{ "Rect2i",   IROpcode::MAKE_RECT2I,   Variant::RECT2I,   4, true,  { 0, 0, 0, 0 } },
	{ "Plane",    IROpcode::MAKE_PLANE,    Variant::PLANE,    4, false, { 0, 0, 0, 0 } },
	// Color() defaults to opaque black (0, 0, 0, 1).
	{ "Color",    IROpcode::MAKE_COLOR,    Variant::COLOR,    4, false, { 0, 0, 0, 1 } },

	{ "Array",              IROpcode::MAKE_ARRAY,                Variant::ARRAY,                0, false, {} },
	{ "Dictionary",         IROpcode::MAKE_DICTIONARY,           Variant::DICTIONARY,           0, false, {} },
	{ "PackedByteArray",    IROpcode::MAKE_PACKED_BYTE_ARRAY,    Variant::PACKED_BYTE_ARRAY,    0, false, {} },
	{ "PackedInt32Array",   IROpcode::MAKE_PACKED_INT32_ARRAY,   Variant::PACKED_INT32_ARRAY,   0, false, {} },
	{ "PackedInt64Array",   IROpcode::MAKE_PACKED_INT64_ARRAY,   Variant::PACKED_INT64_ARRAY,   0, false, {} },
	{ "PackedFloat32Array", IROpcode::MAKE_PACKED_FLOAT32_ARRAY, Variant::PACKED_FLOAT32_ARRAY, 0, false, {} },
	{ "PackedFloat64Array", IROpcode::MAKE_PACKED_FLOAT64_ARRAY, Variant::PACKED_FLOAT64_ARRAY, 0, false, {} },
	{ "PackedStringArray",  IROpcode::MAKE_PACKED_STRING_ARRAY,  Variant::PACKED_STRING_ARRAY,  0, false, {} },
	{ "PackedVector2Array", IROpcode::MAKE_PACKED_VECTOR2_ARRAY, Variant::PACKED_VECTOR2_ARRAY, 0, false, {} },
	{ "PackedVector3Array", IROpcode::MAKE_PACKED_VECTOR3_ARRAY, Variant::PACKED_VECTOR3_ARRAY, 0, false, {} },
	{ "PackedColorArray",   IROpcode::MAKE_PACKED_COLOR_ARRAY,   Variant::PACKED_COLOR_ARRAY,   0, false, {} },
	{ "PackedVector4Array", IROpcode::MAKE_PACKED_VECTOR4_ARRAY, Variant::PACKED_VECTOR4_ARRAY, 0, false, {} },
};

// Diagnostic text listing accepted constructor arities.
std::string accepted_constructor_arities(const std::string& name, int components) {
	if (name == "Color") {
		return "0, 2, 3 or 4 arguments";
	}
	if (name == "Plane") {
		return "0, 1, 2 or 4 arguments";
	}
	if (name == "Rect2" || name == "Rect2i") {
		return "0, 2 or 4 arguments";
	}
	return "0 or " + std::to_string(components) + " arguments";
}

bool is_numeric_scalar(IRInstruction::TypeHint type) {
	return type == Variant::INT || type == Variant::FLOAT || type == Variant::BOOL;
}

const char* packed_array_constructor_name(IRInstruction::TypeHint type) {
	switch (type) {
		case Variant::PACKED_BYTE_ARRAY: return "PackedByteArray";
		case Variant::PACKED_INT32_ARRAY: return "PackedInt32Array";
		case Variant::PACKED_INT64_ARRAY: return "PackedInt64Array";
		case Variant::PACKED_FLOAT32_ARRAY: return "PackedFloat32Array";
		case Variant::PACKED_FLOAT64_ARRAY: return "PackedFloat64Array";
		case Variant::PACKED_STRING_ARRAY: return "PackedStringArray";
		case Variant::PACKED_VECTOR2_ARRAY: return "PackedVector2Array";
		case Variant::PACKED_VECTOR3_ARRAY: return "PackedVector3Array";
		case Variant::PACKED_VECTOR4_ARRAY: return "PackedVector4Array";
		case Variant::PACKED_COLOR_ARRAY: return "PackedColorArray";
		default: return nullptr;
	}
}

struct HostConstructor {
	const char* name;
	IRInstruction::TypeHint variant_type;
};

const HostConstructor HOST_CONSTRUCTORS[] = {
	{ "Transform2D", Variant::TRANSFORM2D },
	{ "Transform3D", Variant::TRANSFORM3D },
	{ "Basis",       Variant::BASIS },
	{ "Quaternion",  Variant::QUATERNION },
	{ "AABB",        Variant::AABB },
	{ "Projection",  Variant::PROJECTION },
	{ "StringName",  Variant::STRING_NAME },
	{ "NodePath",    Variant::NODE_PATH },
	{ "RID",         Variant::RID },
	{ "Signal",      Variant::SIGNAL },
};

const HostConstructor* find_host_constructor(const std::string& name) {
	for (const HostConstructor& entry : HOST_CONSTRUCTORS) {
		if (name == entry.name) {
			return &entry;
		}
	}
	return nullptr;
}

const InlineConstructor* find_inline_constructor(const std::string& name) {
	for (const InlineConstructor& entry : INLINE_CONSTRUCTORS) {
		if (name == entry.name) {
			return &entry;
		}
	}
	return nullptr;
}
} // namespace

bool CodeGenerator::is_inline_primitive_constructor(const std::string& name) const {
	return find_inline_constructor(name) != nullptr;
}

bool CodeGenerator::is_host_constructor(const std::string& name) const {
	return find_host_constructor(name) != nullptr;
}

int CodeGenerator::gen_host_constructor(const std::string& name, const std::vector<int>& arg_regs,
	FunctionContext& func, const Expr* site)
{
	const HostConstructor* info = find_host_constructor(name);
	if (info == nullptr) {
		throw CompilerException(ErrorType::CODEGEN_ERROR,
			"is_host_constructor() accepts '" + name + "' but there is no table entry for it");
	}
	return gen_host_constructor_typed(name, info->variant_type, arg_regs, func, site);
}

int CodeGenerator::gen_host_constructor_typed(const std::string& name,
	IRInstruction::TypeHint variant_type, const std::vector<int>& arg_regs,
	FunctionContext& func, const Expr* site)
{
	if (arg_regs.size() > MAX_HOST_CONSTRUCTOR_ARGS) {
		error_at(name + "() takes at most " + std::to_string(MAX_HOST_CONSTRUCTOR_ARGS) +
			" arguments, got " + std::to_string(arg_regs.size()), site);
	}

	int result_reg = alloc_register(func);
	IRInstruction instr(IROpcode::CONSTRUCT);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::imm(static_cast<int64_t>(variant_type)));
	instr.operands.push_back(IRValue::imm(static_cast<int64_t>(arg_regs.size())));
	for (int arg_reg : arg_regs) {
		instr.operands.push_back(IRValue::reg(arg_reg));
	}
	instr.type_hint = variant_type;
	func.ir.instructions.push_back(instr);
	set_register_type(func, result_reg, variant_type);
	return result_reg;
}

bool CodeGenerator::is_global_function(const std::string& name) const {
	return find_global_function(name) != nullptr;
}

int CodeGenerator::gen_global_function(const CallExpr* expr, std::vector<int>& arg_regs, FunctionContext& func) {
	const GlobalFunction* info = find_global_function(expr->function_name);
	if (info == nullptr) {
		throw CompilerException(ErrorType::CODEGEN_ERROR,
			"is_global_function() accepts '" + expr->function_name + "' but there is no table entry for it");
	}
	if (m_restricted && info->unrestricted_only) {
		error_at("'" + expr->function_name + "' is not supported in a restricted Sandbox: "
			"it mutates the project's shared RNG state", expr);
	}

	const size_t given = arg_regs.size();
	if (given < info->min_args || given > info->max_args) {
		std::string expected;
		if (info->min_args == info->max_args) {
			expected = std::to_string(info->min_args);
		} else if (info->max_args == 63) {
			expected = "at least " + std::to_string(info->min_args);
		} else {
			expected = std::to_string(info->min_args) + " to " + std::to_string(info->max_args);
		}
		error_at(expr->function_name + "() takes " + expected + " argument" +
			(info->min_args == 1 && info->max_args == 1 ? "" : "s") + ", got " +
			std::to_string(given), expr);
	}
	if (info->fn == GlobalFn::STR) {
		return gen_str_call(arg_regs, func);
	}

	if (info->kind == GlobalKind::PRINT) {
		for (int& arg_reg : arg_regs) {
			if (const StructDecl* structure = get_register_struct(func, arg_reg);
				structure != nullptr && !structure->is_class) {
				arg_reg = gen_struct_string(arg_reg, *structure, func, expr);
			}
		}
		int result_reg = alloc_register(func);

		IRInstruction instr(IROpcode::PRINT);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(info->utility_op));
		instr.operands.push_back(IRValue::imm(arg_regs.size()));
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		func.ir.instructions.push_back(instr);

		set_register_type(func, result_reg, Variant::NIL);
		return result_reg;
	}

	// min/max variadic: fold to chain of two-argument calls.
	if ((info->fn == GlobalFn::MIN || info->fn == GlobalFn::MAX) && given > 2) {
		int accumulated = arg_regs[0];
		for (size_t i = 1; i < arg_regs.size(); i++) {
			const std::vector<int> pair { accumulated, arg_regs[i] };
			const int folded = gen_global_call(*info, pair, func, expr);
			if (i > 1) {
				free_register(func, accumulated);
			}
			accumulated = folded;
		}
		return accumulated;
	}

	return gen_global_call(*info, arg_regs, func, expr);
}

int CodeGenerator::gen_global_call(const GlobalFunction& info, const std::vector<int>& arg_regs,
	FunctionContext& func, const Expr* site)
{
	(void)site;

	// NUMERIC: resolved to int or float form by operand types; unresolved stays runtime.
	const GlobalFunction* chosen = &info;
	if (info.kind == GlobalKind::NUMERIC) {
		bool all_integer = true;
		bool all_known = true;
		for (int reg : arg_regs) {
			const IRInstruction::TypeHint hint = get_register_type(func, reg);
			if (hint == Variant::INT) {
				continue;
			}
			all_integer = false;
			if (hint != Variant::FLOAT) {
				all_known = false;
			}
		}
		if (all_integer || all_known) {
			chosen = &global_function(resolve_numeric_form(info, all_integer));
		}
	}

	// CAST: inline when operand is a known numeric/bool, host otherwise (String parse).
	if (info.kind == GlobalKind::CAST) {
		const IRInstruction::TypeHint hint = arg_regs.empty()
			? IRInstruction::TypeHint_NONE
			: get_register_type(func, arg_regs[0]);
		chosen = &global_function(resolve_cast_form(info, hint));
	}

	// The walk element is already the UTF-32 value ord() returns. Only safe
	// for the marker the walk installs; other Strings go through Godot.
	if (info.fn == GlobalFn::ORD && arg_regs.size() == 1 &&
		func.codepoint_value_registers.count(arg_regs[0]) != 0)
	{
		set_register_type(func, arg_regs[0], Variant::INT);
		return arg_regs[0];
	}

	// A resolved numeric cast is only a payload conversion.  Do not lower it to
	// an identity GLOBAL_CALL: that would load the converted value into the
	// global-call register bank and immediately store it again.  Keeping the
	// conversion in IR also lets constant folding erase casts of literals.
	if (arg_regs.size() == 1 &&
		(chosen->fn == GlobalFn::INT_IDENTITY || chosen->fn == GlobalFn::FLOAT_IDENTITY))
	{
		const int source = arg_regs[0];
		const IRInstruction::TypeHint from = get_register_type(func, source);
		const IRInstruction::TypeHint to = chosen->fn == GlobalFn::INT_IDENTITY
			? Variant::INT : Variant::FLOAT;
		if (from == to) {
			return source;
		}
		if (from == Variant::BOOL || from == Variant::INT || from == Variant::FLOAT) {
			const int converted = alloc_register(func);
			IRInstruction convert(IROpcode::CONVERT, IRValue::reg(converted),
				IRValue::reg(source), IRValue::imm(from));
			convert.type_hint = to;
			func.ir.instructions.push_back(convert);
			set_register_type(func, converted, to);
			return converted;
		}
	}

	// Implicit numeric widening emitted as CONVERT here, foldable by optimizer.
	std::vector<int> call_args = arg_regs;
	std::vector<int> converted;
	IRInstruction::TypeHint wanted = IRInstruction::TypeHint_NONE;
	switch (chosen->kind) {
		case GlobalKind::INT_OP:
		case GlobalKind::SYSCALL_INT:
			wanted = Variant::INT;
			break;
		case GlobalKind::FLOAT_OP:
		case GlobalKind::SYSCALL:
			wanted = Variant::FLOAT;
			break;
		case GlobalKind::PRINT:
		case GlobalKind::NUMERIC:
		case GlobalKind::CAST:
		case GlobalKind::HOST:
			break;
	}

	bool typed = wanted != IRInstruction::TypeHint_NONE;
	if (typed) {
		for (int& reg : call_args) {
			const IRInstruction::TypeHint hint = get_register_type(func, reg);
			if (hint == wanted) {
				continue;
			}
			if ((wanted == Variant::FLOAT && (hint == Variant::INT || hint == Variant::BOOL)) ||
				(wanted == Variant::INT && (hint == Variant::FLOAT || hint == Variant::BOOL))) {
				const int widened = alloc_register(func);
				IRInstruction convert(IROpcode::CONVERT, IRValue::reg(widened), IRValue::reg(reg),
					IRValue::imm(hint));
				convert.type_hint = wanted;
				func.ir.instructions.push_back(convert);
				set_register_type(func, widened, wanted);
				converted.push_back(widened);
				reg = widened;
				continue;
			}
			typed = false;
			break;
		}
	}

	int result_reg = alloc_register(func);

	IRInstruction instr(IROpcode::GLOBAL_CALL);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::imm(static_cast<int64_t>(chosen->fn)));
	instr.operands.push_back(IRValue::imm(typed ? 1 : 0));
	instr.operands.push_back(IRValue::imm(call_args.size()));
	for (int arg_reg : call_args) {
		instr.operands.push_back(IRValue::reg(arg_reg));
	}

	IRInstruction::TypeHint result_type = IRInstruction::TypeHint_NONE;
	switch (chosen->result) {
		case GlobalResult::NIL: result_type = Variant::NIL; break;
		case GlobalResult::BOOL: result_type = Variant::BOOL; break;
		case GlobalResult::INT: result_type = Variant::INT; break;
		case GlobalResult::FLOAT: result_type = Variant::FLOAT; break;
		case GlobalResult::STRING: result_type = Variant::STRING; break;
		case GlobalResult::NUMERIC: break;
		// Clear type hint after conversion.
		case GlobalResult::VARIANT: break;
	}
	instr.type_hint = result_type;
	func.ir.instructions.push_back(instr);

	for (int reg : converted) {
		free_register(func, reg);
	}
	if (result_type != IRInstruction::TypeHint_NONE) {
		set_register_type(func, result_reg, result_type);
	}
	return result_reg;
}

bool CodeGenerator::is_inline_member_access(IRInstruction::TypeHint type, const std::string& member) const {
	if (type == IRInstruction::TypeHint_NONE) {
		return false;
	}
	return find_builtin_member(static_cast<uint32_t>(type), member).valid();
}

int CodeGenerator::gen_builtin_constant(const std::string& type, const std::string& name,
	FunctionContext& func)
{
	const InlineConstructor* info = find_inline_constructor(type);
	const BuiltinConstant* constant = find_builtin_constant(type, name);
	if (info == nullptr || constant == nullptr) {
		return -1;
	}

	std::vector<int> components;
	for (int i = 0; i < info->components; i++) {
		components.push_back(info->integer
			? gen_int_immediate(static_cast<int64_t>(constant->components[i]), func)
			: gen_float_immediate(constant->components[i], func));
	}

	int result_reg = gen_inline_constructor(type, components, func, nullptr);
	for (int reg : components) {
		free_register(func, reg);
	}
	return result_reg;
}

int CodeGenerator::gen_inline_constructor(const std::string& name, const std::vector<int>& arg_regs,
	FunctionContext& func, const Expr* site)
{
	const InlineConstructor* info = find_inline_constructor(name);
	if (info == nullptr) {
		throw CompilerException(ErrorType::CODEGEN_ERROR,
			"is_inline_primitive_constructor() accepts '" + name + "' but there is no table entry for it");
	}

	const int given = static_cast<int>(arg_regs.size());
	int result_reg = alloc_register(func);

	// Containers: empty inline, or converted by Godot's Variant constructor.
	if (info->components == 0) {
		if (given > 1) {
			error_at(name + "() takes 0 or 1 arguments, got " + std::to_string(given), site,
				name == "Array" || name == "Dictionary"
					? std::string("Write the elements as a literal instead")
					: "Pass the elements as one Array: " + name + "([...])");
		}
		if (given == 1 && (name == "Array" || name == "Dictionary")) {
			free_register(func, result_reg);
			return gen_host_constructor_typed(name, info->variant_type, arg_regs, func, site);
		}

		IRInstruction instr(info->opcode);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(given));
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		instr.type_hint = info->variant_type;
		func.ir.instructions.push_back(instr);
		set_register_type(func, result_reg, info->variant_type);
		return result_reg;
	}

	// Component types: fill slots from arguments, freed after emit.
	std::vector<int> components;
	std::vector<int> owned;

	auto load_default = [&](int index) {
		int reg = alloc_register(func);
		if (info->integer) {
			IRInstruction load(IROpcode::LOAD_IMM, IRValue::reg(reg),
				IRValue::imm(static_cast<int64_t>(info->defaults[index])));
			load.type_hint = Variant::INT;
			func.ir.instructions.push_back(load);
			set_register_type(func, reg, Variant::INT);
		} else {
			IRInstruction load(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(reg),
				IRValue::fimm(info->defaults[index]));
			load.type_hint = Variant::FLOAT;
			func.ir.instructions.push_back(load);
			set_register_type(func, reg, Variant::FLOAT);
		}
		owned.push_back(reg);
		return reg;
	};
	auto read_member = [&](int obj_reg, const char* member) {
		int reg = gen_member_read(obj_reg, member, func, site);
		owned.push_back(reg);
		return reg;
	};

	if (given == 0) {
		for (int i = 0; i < info->components; i++) {
			components.push_back(load_default(i));
		}
	} else if (given == info->components) {
		components.assign(arg_regs.begin(), arg_regs.end());
	} else if (name == "Color" && given == 3) {
		components = { arg_regs[0], arg_regs[1], arg_regs[2], load_default(3) };
	} else if (name == "Color" && given == 2) {
		// Color(from, alpha)
		components = { read_member(arg_regs[0], "r"), read_member(arg_regs[0], "g"),
			read_member(arg_regs[0], "b"), arg_regs[1] };
	} else if ((name == "Rect2" || name == "Rect2i") && given == 2) {
		// Rect2(position, size), both Vector2
		components = { read_member(arg_regs[0], "x"), read_member(arg_regs[0], "y"),
			read_member(arg_regs[1], "x"), read_member(arg_regs[1], "y") };
	} else if (name == "Plane" && (given == 1 || given == 2)) {
		// Plane(normal) / Plane(normal, d)
		components = { read_member(arg_regs[0], "x"), read_member(arg_regs[0], "y"),
			read_member(arg_regs[0], "z"), given == 2 ? arg_regs[1] : load_default(3) };
	} else if (given == 1 && !is_numeric_scalar(get_register_type(func, arg_regs[0]))) {
		for (int reg : owned) {
			free_register(func, reg);
		}
		free_register(func, result_reg);
		return gen_host_constructor_typed(name, info->variant_type, arg_regs, func, site);
	} else {
		error_at(name + "() takes " + accepted_constructor_arities(name, info->components) +
			", got " + std::to_string(given), site);
	}

	IRInstruction instr(info->opcode);
	instr.operands.push_back(IRValue::reg(result_reg));
	for (int reg : components) {
		instr.operands.push_back(IRValue::reg(reg));
	}
	instr.type_hint = info->variant_type;
	func.ir.instructions.push_back(instr);
	set_register_type(func, result_reg, info->variant_type);

	for (int reg : owned) {
		free_register(func, reg);
	}

	return result_reg;
}

int CodeGenerator::gen_inline_member_get(int obj_reg, IRInstruction::TypeHint obj_type, const std::string& member, FunctionContext& func) {
	int result_reg = alloc_register(func);

	IRInstruction instr(IROpcode::VGET_INLINE);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::reg(obj_reg));
	instr.operands.push_back(ir_str(member));
	instr.operands.push_back(IRValue::imm(static_cast<int>(obj_type)));

	func.ir.instructions.push_back(instr);

	const BuiltinMember layout = find_builtin_member(static_cast<uint32_t>(obj_type), member);
	set_register_type(func, result_reg, static_cast<IRInstruction::TypeHint>(layout.result_type));

	return result_reg;
}

void CodeGenerator::gen_inline_member_set(int obj_reg, IRInstruction::TypeHint obj_type,
	const std::string& member, int value_reg, FunctionContext& func, bool stamp_type)
{
	IRInstruction instr(IROpcode::VSET_INLINE);
	instr.operands.push_back(IRValue::reg(obj_reg));
	instr.operands.push_back(ir_str(member));
	instr.operands.push_back(IRValue::imm(static_cast<int>(obj_type)));
	instr.operands.push_back(IRValue::reg(value_reg));
	instr.operands.push_back(IRValue::imm(stamp_type ? 1 : 0));
	func.ir.instructions.push_back(instr);
}

// Packed arrays have size()/get() via VCALL; String has neither.
bool CodeGenerator::is_packed_array_type(IRInstruction::TypeHint type) {
	switch (type) {
		case Variant::PACKED_BYTE_ARRAY:
		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_INT64_ARRAY:
		case Variant::PACKED_FLOAT32_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY:
		case Variant::PACKED_STRING_ARRAY:
		case Variant::PACKED_VECTOR2_ARRAY:
		case Variant::PACKED_VECTOR3_ARRAY:
		case Variant::PACKED_VECTOR4_ARRAY:
		case Variant::PACKED_COLOR_ARRAY:
			return true;
		default:
			return false;
	}
}

std::vector<IRInstruction::TypeHint> CodeGenerator::inline_member_types(const std::string& member) const {
	size_t count = 0;
	const uint32_t* candidates = builtin_member_candidates(count);

	std::vector<IRInstruction::TypeHint> types;
	for (size_t i = 0; i < count; i++) {
		const auto type = static_cast<IRInstruction::TypeHint>(candidates[i]);
		if (is_inline_member_access(type, member)) {
			types.push_back(type);
		}
	}
	return types;
}

std::vector<CodeGenerator::InlineMemberGroup> CodeGenerator::inline_member_groups(
	const std::string& member) const
{
	std::vector<InlineMemberGroup> groups;
	std::vector<BuiltinMember> layouts;
	for (IRInstruction::TypeHint type : inline_member_types(member)) {
		const BuiltinMember layout = find_builtin_member(uint32_t(type), member);
		size_t slot = 0;
		for (; slot < layouts.size(); slot++) {
			const BuiltinMember& seen = layouts[slot];
			if (seen.first_component == layout.first_component && seen.count == layout.count &&
				seen.result_type == layout.result_type && seen.integer == layout.integer)
			{
				break;
			}
		}
		if (slot == layouts.size()) {
			layouts.push_back(layout);
			groups.push_back(InlineMemberGroup{});
		}
		groups[slot].types.push_back(type);
	}
	return groups;
}

void CodeGenerator::emit_group_type_test(int obj_reg, const InlineMemberGroup& group,
	const std::string& next_label, FunctionContext& func)
{
	int64_t mask = 0;
	for (IRInstruction::TypeHint type : group.types) {
		mask |= int64_t(1) << static_cast<int>(type);
	}
	const int test_reg = alloc_register(func);
	func.ir.instructions.emplace_back(group.types.size() == 1
			? IROpcode::TYPE_TEST : IROpcode::TYPE_TEST_MASK,
		IRValue::reg(test_reg), IRValue::reg(obj_reg),
		IRValue::imm(group.types.size() == 1
			? static_cast<int64_t>(group.types.front()) : mask));
	set_register_type(func, test_reg, Variant::BOOL);
	emit_conditional_branch(IROpcode::BRANCH_ZERO, test_reg, next_label, func);
	free_register(func, test_reg);
}

int CodeGenerator::gen_vget(int obj_reg, const std::string& member, FunctionContext& func) {
	int result_reg = alloc_register(func);
	int str_idx = add_string_constant(member);

	IRInstruction instr(IROpcode::VGET);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::reg(obj_reg));
	instr.operands.push_back(IRValue::imm(str_idx));
	instr.operands.push_back(IRValue::imm(static_cast<int64_t>(member.length())));
	func.ir.instructions.push_back(instr);

	return result_reg;
}

void CodeGenerator::gen_vset(int obj_reg, const std::string& member, int value_reg, FunctionContext& func) {
	int str_idx = add_string_constant(member);

	IRInstruction instr(IROpcode::VSET);
	instr.operands.push_back(IRValue::reg(obj_reg));
	instr.operands.push_back(IRValue::imm(str_idx));
	instr.operands.push_back(IRValue::imm(static_cast<int64_t>(member.length())));
	instr.operands.push_back(IRValue::reg(value_reg));
	func.ir.instructions.push_back(instr);
}

// Untyped `.x`: branch on tag to element read, inline payload or VGET (Object-only).
int CodeGenerator::gen_dynamic_member_get(int obj_reg, const std::string& member, FunctionContext& func) {
	const std::vector<InlineMemberGroup> groups = inline_member_groups(member);
	const std::string end_label = make_label("member_get_end");
	int result_reg = alloc_register(func);

	// Dictionary: element read, not VGET.
	{
		const std::string next_label = make_label("member_get_next");
		int test_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(test_reg),
			IRValue::reg(obj_reg), IRValue::imm(static_cast<int64_t>(Variant::DICTIONARY)));
		set_register_type(func, test_reg, Variant::BOOL);
		emit_conditional_branch(IROpcode::BRANCH_ZERO, test_reg, next_label, func);
		free_register(func, test_reg);

		int element_reg = gen_dict_get(obj_reg, member, func);
		func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(result_reg),
			IRValue::reg(element_reg));
		free_register(func, element_reg);

		// A class instance holds its own fields as keys and the engine object it
		// extends under `@base`. A name that is not a key is a property of that
		// object. Only emitted when the script declares such a class; a plain
		// Dictionary then pays one lookup, and only for a key it does not have.
		if (has_engine_based_classes()) {
			const std::string base_label = make_label("member_get_base_done");
			int found_reg = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(found_reg),
				IRValue::reg(result_reg), IRValue::imm(static_cast<int64_t>(Variant::NIL)));
			set_register_type(func, found_reg, Variant::BOOL);
			emit_conditional_branch(IROpcode::BRANCH_ZERO, found_reg, base_label, func);
			free_register(func, found_reg);

			int base_reg = gen_dict_get(obj_reg, NATIVE_BASE_KEY, func);
			int is_object_reg = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(is_object_reg),
				IRValue::reg(base_reg), IRValue::imm(static_cast<int64_t>(Variant::OBJECT)));
			set_register_type(func, is_object_reg, Variant::BOOL);
			emit_conditional_branch(IROpcode::BRANCH_ZERO, is_object_reg, base_label, func);
			free_register(func, is_object_reg);

			set_register_type(func, base_reg, Variant::OBJECT);
			int property_reg = gen_vget(base_reg, member, func);
			func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(result_reg),
				IRValue::reg(property_reg));
			free_register(func, property_reg);

			func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(base_label));
			free_register(func, base_reg);
		}
		func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(end_label));
		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(next_label));
	}

	for (const InlineMemberGroup& group : groups) {
		const std::string next_label = make_label("member_get_next");
		emit_group_type_test(obj_reg, group, next_label, func);

		int inline_reg = gen_inline_member_get(obj_reg, group.types.front(), member, func);
		func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(result_reg),
			IRValue::reg(inline_reg));
		free_register(func, inline_reg);
		func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(end_label));
		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(next_label));
	}

	int property_reg = gen_vget(obj_reg, member, func);
	func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(result_reg),
		IRValue::reg(property_reg));
	free_register(func, property_reg);
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end_label));

	// Result type unknown: paths yield int, float or property.
	return result_reg;
}

void CodeGenerator::gen_dynamic_member_set(int obj_reg, const std::string& member, int value_reg,
	FunctionContext& func)
{
	const std::vector<InlineMemberGroup> groups = inline_member_groups(member);
	const std::string end_label = make_label("member_set_end");

	// Dictionary: element write, not VSET.
	{
		const std::string next_label = make_label("member_set_next");
		int test_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(test_reg),
			IRValue::reg(obj_reg), IRValue::imm(static_cast<int64_t>(Variant::DICTIONARY)));
		set_register_type(func, test_reg, Variant::BOOL);
		emit_conditional_branch(IROpcode::BRANCH_ZERO, test_reg, next_label, func);
		free_register(func, test_reg);

		// The read's mirror: a name the instance does not declare is written to the
		// engine object it extends, not added as a key.
		if (has_engine_based_classes()) {
			const std::string element_label = make_label("member_set_element");
			int base_reg = gen_dict_get(obj_reg, NATIVE_BASE_KEY, func);
			int is_object_reg = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(is_object_reg),
				IRValue::reg(base_reg), IRValue::imm(static_cast<int64_t>(Variant::OBJECT)));
			set_register_type(func, is_object_reg, Variant::BOOL);
			emit_conditional_branch(IROpcode::BRANCH_ZERO, is_object_reg, element_label, func);
			free_register(func, is_object_reg);

			int has_reg = gen_dict_has(obj_reg, member, func);
			emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, has_reg, element_label, func);
			free_register(func, has_reg);

			set_register_type(func, base_reg, Variant::OBJECT);
			gen_vset(base_reg, member, value_reg, func);
			func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(end_label));

			func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(element_label));
			free_register(func, base_reg);
		}
		gen_dict_set(obj_reg, member, value_reg, func);
		func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(end_label));
		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(next_label));
	}

	for (const InlineMemberGroup& group : groups) {
		const std::string next_label = make_label("member_set_next");
		emit_group_type_test(obj_reg, group, next_label, func);

		gen_inline_member_set(obj_reg, group.types.front(), member, value_reg, func,
			group.types.size() == 1);
		func.ir.instructions.emplace_back(IROpcode::JUMP, ir_label(end_label));
		func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(next_label));
	}

	gen_vset(obj_reg, member, value_reg, func);
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(end_label));
}

int CodeGenerator::gen_member_read(int obj_reg, const std::string& member, FunctionContext& func,
	const Expr* site)
{
	const IRInstruction::TypeHint obj_type = get_register_type(func, obj_reg);

	if (is_inline_member_access(obj_type, member)) {
		return gen_inline_member_get(obj_reg, obj_type, member, func);
	}

	// Struct field: validate and apply declared type.
	if (const StructDecl* decl = get_register_struct(func, obj_reg)) {
		if (find_struct_field(*decl, member) == nullptr && native_base(*decl) != nullptr) {
			int base_reg = gen_native_base_load(obj_reg, func);
			int result_reg = gen_vget(base_reg, member, func);
			free_register(func, base_reg);
			return result_reg;
		}
		const StructField& field = require_struct_field(*decl, member,
			site ? site->line : 0, site ? site->column : 0);
		int result_reg = gen_dict_get(obj_reg, member, func);
		apply_declared_type(result_reg, field.type_hint, func);
		return result_reg;
	}

	// Dictionary: element read, not VGET (Object-only).
	if (obj_type == Variant::DICTIONARY) {
		return gen_dict_get(obj_reg, member, func);
	}

	// Unknown tag: decide at run time. A Dictionary reaching VGET throws.
	if (obj_type == IRInstruction::TypeHint_NONE) {
		return gen_dynamic_member_get(obj_reg, member, func);
	}

	return gen_vget(obj_reg, member, func);
}

bool CodeGenerator::gen_member_store(int obj_reg, const std::string& member, int value_reg,
	FunctionContext& func)
{
	// Struct/Dictionary: element write, no write-back needed.
	if (const StructDecl* decl = get_register_struct(func, obj_reg)) {
		if (find_struct_field(*decl, member) == nullptr && native_base(*decl) != nullptr) {
			int base_reg = gen_native_base_load(obj_reg, func);
			gen_vset(base_reg, member, value_reg, func);
			free_register(func, base_reg);
			return false;
		}
		gen_dict_set(obj_reg, member, value_reg, func);
		return false;
	}
	if (get_register_type(func, obj_reg) == Variant::DICTIONARY) {
		gen_dict_set(obj_reg, member, value_reg, func);
		return false;
	}

	const IRInstruction::TypeHint obj_type = get_register_type(func, obj_reg);

	if (is_inline_member_access(obj_type, member)) {
		gen_inline_member_set(obj_reg, obj_type, member, value_reg, func);
		return true;
	}

	// Unknown tag: decide at run time. A Dictionary reaching VSET throws.
	if (obj_type == IRInstruction::TypeHint_NONE) {
		gen_dynamic_member_set(obj_reg, member, value_reg, func);
		return true;
	}

	gen_vset(obj_reg, member, value_reg, func);
	return false;
}

// ECALL_STRING_AT; index coerced to int (matches engine `s[1.0]` behavior).
void CodeGenerator::gen_string_at(int dest, int obj_reg, int idx_reg, FunctionContext& func,
	const Expr* site)
{
	int index_reg = idx_reg;
	bool owned = false;
	if (get_register_type(func, idx_reg) != Variant::INT) {
		index_reg = gen_global_call(*find_global_function("int"), { idx_reg }, func, site);
		owned = true;
	}

	IRInstruction at(IROpcode::CALL_SYSCALL);
	at.operands.push_back(IRValue::reg(dest));
	at.operands.push_back(IRValue::imm(ECALL_STRING_AT));
	at.operands.push_back(IRValue::reg(obj_reg));
	at.operands.push_back(IRValue::reg(index_reg));
	func.ir.instructions.push_back(at);

	if (owned) {
		free_register(func, index_reg);
	}
}

void CodeGenerator::gen_variant_get(int dest, int obj_reg, int idx_reg, FunctionContext& func) {
	IRInstruction get(IROpcode::CALL_SYSCALL);
	get.operands.push_back(IRValue::reg(dest));
	get.operands.push_back(IRValue::imm(ECALL_VARIANT_GET));
	get.operands.push_back(IRValue::reg(obj_reg));
	get.operands.push_back(IRValue::reg(idx_reg));
	func.ir.instructions.push_back(get);
}

int CodeGenerator::gen_element_read(int obj_reg, int idx_reg, FunctionContext& func,
	const Expr* site)
{
	if (get_register_type(func, obj_reg) == Variant::STRING) {
		int result_reg = alloc_register(func);
		gen_string_at(result_reg, obj_reg, idx_reg, func, site);
		set_register_type(func, result_reg, Variant::STRING);
		return result_reg;
	}

	if (is_array_element_access(obj_reg, idx_reg, func)) {
		int element_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::ARRAY_GET, IRValue::reg(element_reg),
			IRValue::reg(obj_reg), IRValue::reg(idx_reg));
		return element_reg;
	}

	if (get_register_type(func, obj_reg) == Variant::DICTIONARY) {
		constexpr int64_t DICT_OP_GET = 0;
		return gen_dictionary_op(DICT_OP_GET, obj_reg, idx_reg,
			IRInstruction::TypeHint_NONE, func);
	}

	// `[]` is a Variant operation, not a call to a container's get() method.
	// Preserve the key as a Variant and let Godot select the indexed, keyed, or
	// named operation for every remaining statically known or unknown type.
	int result_reg = alloc_register(func);
	gen_variant_get(result_reg, obj_reg, idx_reg, func);
	set_register_type(func, result_reg, IRInstruction::TypeHint_NONE);
	return result_reg;
}

void CodeGenerator::gen_element_store(int obj_reg, int idx_reg, int value_reg, FunctionContext& func,
	const Expr* site)
{
	// Guest Strings are shared handles; character mutation would alias.
	if (get_register_type(func, obj_reg) == Variant::STRING) {
		error_at("Cannot assign to a character of a String", site ? site->line : 0,
			site ? site->column : 0,
			"Build a new String instead, e.g. 's.substr(0, i) + c + s.substr(i + 1)'");
	}

	if (is_array_element_access(obj_reg, idx_reg, func)) {
		func.ir.instructions.emplace_back(IROpcode::ARRAY_SET, IRValue::reg(obj_reg),
			IRValue::reg(idx_reg), IRValue::reg(value_reg));
		return;
	}

	if (get_register_type(func, obj_reg) == Variant::DICTIONARY) {
		func.ir.instructions.emplace_back(IROpcode::DICT_SET, IRValue::reg(obj_reg),
			IRValue::reg(idx_reg), IRValue::reg(value_reg));
		return;
	}

	int result_reg = alloc_register(func);
	IRInstruction instr(IROpcode::VCALL);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::reg(obj_reg));
	instr.operands.push_back(ir_str("set"));
	instr.operands.push_back(IRValue::imm(2));
	instr.operands.push_back(IRValue::reg(idx_reg));
	instr.operands.push_back(IRValue::reg(value_reg));
	func.ir.instructions.push_back(instr);
	free_register(func, result_reg);
}

std::string CodeGenerator::script_level_super_hint() const {
	if (!m_script_base_class.empty() && global_script_class_path(m_script_base_class) != nullptr) {
		return "'" + m_script_base_class + "' is a script in another file, and none of its "
			"body is compiled into this program. Only a class declared in this file has a "
			"super(). Call into '" + m_script_base_class + "' through an instance of it instead";
	}
	return "Only a class declared in this file has one";
}

std::unordered_set<std::string> CodeGenerator::get_global_classes() {
	return {
		"AudioServer",
		"CameraServer",
		"DisplayServer",
		"NavigationServer2D",
		"NavigationServer3D",
		"PhysicsServer2D",
		"PhysicsServer3D",
		"TextServerManager",
		"ClassDB",
		"EditorInterface",
		"Engine",
		"EngineDebugger",
		"GDExtensionManager",
		"Geometry2D",
		"Geometry3D",
		"Input",
		"InputMap",
		"IP",
		"Marshalls",
		"NativeMenu",
		"NavigationMeshGenerator",
		"OS",
		"Performance",
		"PhysicsServer2DManager",
		"PhysicsServer3DManager",
		"ProjectSettings",
		"RenderingServer",
		"ResourceLoader",
		"ResourceSaver",
		"ResourceUID",
		"ThemeDB",
		"Time",
		"TranslationServer",
		"WorkerThreadPool",
		"XRServer",
	};
}

bool CodeGenerator::is_global_class(const std::string& name) const {
	static const auto global_classes = get_global_classes();
	return global_classes.find(name) != global_classes.end();
}

bool CodeGenerator::names_an_engine_type(const std::string& name, FunctionContext& func) {
	if (name.empty() || name[0] < 'A' || name[0] > 'Z') {
		return false;
	}
	return find_variable(func, name) == nullptr &&
		m_enums.count(name) == 0 && !is_global_enum(name) && !has_builtin_constants(name) &&
		!is_global_variable(name) && !is_global_class(name) &&
		!is_autoload(name) && !is_local_function(name) &&
		find_struct(name) == nullptr &&
		global_script_class_path(name) == nullptr &&
		!is_inline_primitive_constructor(name) &&
		!is_host_constructor(name) && find_signal(name) == nullptr;
}

Variant::Type CodeGenerator::names_a_builtin_type(const std::string& name, FunctionContext& func) {
	if (name.empty() || name[0] < 'A' || name[0] > 'Z') {
		return Variant::VARIANT_MAX;
	}
	const bool shadowed = find_variable(func, name) != nullptr ||
		m_enums.count(name) != 0 || is_global_variable(name) ||
		is_autoload(name) || is_local_function(name) ||
		find_struct(name) != nullptr || find_signal(name) != nullptr ||
		global_script_class_path(name) != nullptr;
	if (shadowed) {
		return Variant::VARIANT_MAX;
	}
	return Variant::type_from_name(name);
}

bool CodeGenerator::is_autoload(const std::string& name) const {
	return m_autoloads.find(name) != m_autoloads.end();
}

bool CodeGenerator::names_a_chain_class(const std::string& name, FunctionContext& func) {
	return !name.empty() && m_chain.names_a_link(name) &&
		find_variable(func, name) == nullptr && !is_global_variable(name);
}

const VariableExpr* CodeGenerator::engine_enum_qualifier(const Expr* expr, FunctionContext& func) {
	auto* member = dynamic_cast<const MemberCallExpr*>(expr);
	if (member == nullptr || member->is_method_call || !member->arguments.empty()) {
		return nullptr;
	}
	const std::string& enum_name = member->member_name;
	if (enum_name.empty() || enum_name[0] < 'A' || enum_name[0] > 'Z') {
		return nullptr;
	}
	if (enum_name.find_first_of("abcdefghijklmnopqrstuvwxyz") == std::string::npos) {
		return nullptr;
	}
	auto* owner = dynamic_cast<const VariableExpr*>(member->object.get());
	if (owner == nullptr || find_variable(func, owner->name) != nullptr) {
		return nullptr;
	}
	if (!is_global_class(owner->name) && !names_an_engine_type(owner->name, func)) {
		return nullptr;
	}
	return owner;
}

const std::string* CodeGenerator::chain_qualified_member(const Expr* expr, FunctionContext& func) {
	auto* member = dynamic_cast<const MemberCallExpr*>(expr);
	if (member == nullptr || member->is_method_call || !member->arguments.empty()) {
		return nullptr;
	}
	auto* object = dynamic_cast<const VariableExpr*>(member->object.get());
	if (object == nullptr || !names_a_chain_class(object->name, func)) {
		return nullptr;
	}
	return &member->member_name;
}

const std::string* CodeGenerator::global_script_class_path(const std::string& name) const {
	const auto it = m_global_script_classes.find(name);
	return it != m_global_script_classes.end() ? &it->second : nullptr;
}

void CodeGenerator::reject_test_reference(const std::string& name, const Expr* site) {
	if (m_in_test_function || m_test_functions.find(name) == m_test_functions.end()) {
		return;
	}
	error_at("'" + name + "' is a @test function, and only another @test may reach it", site,
		"A shipping build drops every @test, so this call would have nothing to reach");
}

bool CodeGenerator::is_local_function(const std::string& name) const {
	return m_local_functions.find(name) != m_local_functions.end();
}

int CodeGenerator::coerce_to_declared_type(int reg, IRInstruction::TypeHint declared,
	FunctionContext& func, const std::string& what, const Stmt* site)
{
	return coerce_to_declared_type(reg, declared, func, what,
		site != nullptr ? site->line : 0, site != nullptr ? site->column : 0);
}

int CodeGenerator::coerce_to_declared_type(int reg, IRInstruction::TypeHint declared,
	FunctionContext& func, const std::string& what, int line, int column,
	const std::string& display)
{
	if (declared == IRInstruction::TypeHint_NONE) {
		return reg;
	}
	const IRInstruction::TypeHint actual = get_register_type(func, reg);
	if (actual == declared) {
		return reg;
	}
	if (actual == IRInstruction::TypeHint_NONE) {
		// Match GDScript's OPCODE_ASSIGN_TYPED_BUILTIN for declared scalars.
		if (declared != Variant::INT && declared != Variant::FLOAT &&
			declared != Variant::BOOL) {
			return reg;
		}
		const int converted = alloc_register(func);
		IRInstruction coerce(IROpcode::COERCE, IRValue::reg(converted), IRValue::reg(reg));
		coerce.type_hint = declared;
		func.ir.instructions.push_back(coerce);
		set_register_type(func, converted, declared);
		free_register(func, reg);
		return converted;
	}
	if (actual == Variant::NIL && declared == Variant::OBJECT) {
		return reg;
	}

	// INT->FLOAT, BOOL->INT/FLOAT: payload size mismatch without explicit convert.
	const bool widening = (declared == Variant::FLOAT && actual == Variant::INT) ||
		(actual == Variant::BOOL && (declared == Variant::INT || declared == Variant::FLOAT));
	if (widening) {
		int converted = alloc_register(func);
		IRInstruction convert(IROpcode::CONVERT, IRValue::reg(converted), IRValue::reg(reg),
			IRValue::imm(actual));
		convert.type_hint = declared;
		func.ir.instructions.push_back(convert);
		set_register_type(func, converted, declared);
		free_register(func, reg);
		return converted;
	}

	if (actual == Variant::ARRAY) {
		if (const char* packed = packed_array_constructor_name(declared)) {
			const int converted = gen_inline_constructor(packed, { reg }, func, nullptr);
			free_register(func, reg);
			return converted;
		}
	}

	if (constructs_implicitly_from(actual, declared)) {
		const int converted = gen_host_constructor_typed(variant_type_name(declared),
			declared, { reg }, func, nullptr);
		free_register(func, reg);
		return converted;
	}

	const std::string expected = display.empty()
		? std::string(variant_type_name(declared)) : display;
	if (actual == Variant::NIL) {
		error_at("Cannot assign null to " + what + " of type " + expected, line, column,
			"Declare it as '" + expected + "?' to allow null");
	}
	error_at("Cannot assign a value of type " + std::string(variant_type_name(actual)) +
		" to " + what + " of type " + expected, line, column);
}

int CodeGenerator::coerce_to_declared_type(int reg, TypeSet declared,
	FunctionContext& func, const std::string& what, const Stmt* site,
	const std::string& display)
{
	return coerce_to_declared_type(reg, declared, func, what,
		site != nullptr ? site->line : 0, site != nullptr ? site->column : 0, display);
}

int CodeGenerator::coerce_to_declared_type(int reg, TypeSet declared,
	FunctionContext& func, const std::string& what, int line, int column,
	const std::string& display)
{
	if (declared.any()) {
		return reg;
	}
	const std::string expected = display.empty() ? declared.to_string() : display;
	const IRInstruction::TypeHint actual = get_register_type(func, reg);
	if (actual != IRInstruction::TypeHint_NONE) {
		if (declared.contains(static_cast<Variant::Type>(actual))) {
			return reg;
		}
		if (declared.is_nullable_single() && actual != Variant::NIL) {
			return coerce_to_declared_type(reg,
				static_cast<IRInstruction::TypeHint>(declared.non_null().only()), func,
				what, line, column, expected);
		}
		error_at("Cannot assign a value of type " + std::string(variant_type_name(actual)) +
			" to " + what + " of type " + expected, line, column);
	}

	// A source with a declared union of its own already passed a guard for that
	// set, so a destination listing every one of its tags needs no second test.
	if (const auto proved = func.declared_sets.find(reg);
		proved != func.declared_sets.end() && !proved->second.any() &&
		(proved->second.mask & ~declared.mask) == 0) {
		return reg;
	}

	const int accepted = alloc_register(func);
	IRInstruction test(IROpcode::TYPE_TEST_MASK, IRValue::reg(accepted), IRValue::reg(reg),
		IRValue::imm(static_cast<int64_t>(declared.mask)));
	test.type_hint = Variant::BOOL;
	func.ir.instructions.push_back(test);
	set_register_type(func, accepted, Variant::BOOL);
	const std::string passed = make_label("union_type_ok");
	emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, accepted, passed, func);
	free_register(func, accepted);

	IRInstruction fail(IROpcode::THROW, ir_str("TypeError"),
		ir_str("Cannot assign a value to " + what + " of type " + expected));
	fail.operands.push_back(IRValue::imm(0));
	func.ir.instructions.push_back(fail);
	func.ir.instructions.emplace_back(IROpcode::LABEL, ir_label(passed));
	return reg;
}

void CodeGenerator::coerce_folded_initializer(IRGlobalVar& global,
	const TypeExpr& declared_expr, int line, int column) const {
	if (global.declared_set != 0) {
		IRGlobalVar probe = global;
		probe.type_hint = IRInstruction::TypeHint_NONE;
		const IRInstruction::TypeHint actual = derive_global_value_type(probe);
		const TypeSet declared{global.declared_set};
		if (declared.is_nullable_single() && actual == Variant::INT &&
			declared.non_null().only() == Variant::FLOAT) {
			global.init_type = IRGlobalVar::InitType::FLOAT;
			global.init_value = static_cast<double>(std::get<int64_t>(global.init_value));
			return;
		}
		if (actual != IRInstruction::TypeHint_NONE &&
			!declared.contains(static_cast<Variant::Type>(actual))) {
			error_at("Global variable '" + global.name + "' is declared as " +
				declared_expr.to_string() + " but initialized with a value of type " +
				std::string(variant_type_name(actual)), line, column);
		}
		return;
	}
	if (global.type_hint == IRInstruction::TypeHint_NONE) {
		return;
	}
	if (global.init_type == IRGlobalVar::InitType::NULL_VAL) {
		if (global.type_hint == Variant::OBJECT) {
			return;
		}
		const std::string expected = declared_expr.to_string();
		error_at("Cannot assign null to global '" + global.name + "' of type " + expected,
			line, column, "Declare it as '" + expected + "?' to allow null");
	}

	// `var f: float = 0` folds INT to FLOAT.
	if (global.type_hint == Variant::FLOAT && global.init_type == IRGlobalVar::InitType::INT) {
		global.init_type = IRGlobalVar::InitType::FLOAT;
		global.init_value = static_cast<double>(std::get<int64_t>(global.init_value));
		return;
	}

	IRGlobalVar probe = global;
	probe.type_hint = IRInstruction::TypeHint_NONE;
	const IRInstruction::TypeHint actual = derive_global_value_type(probe);
	if (actual != IRInstruction::TypeHint_NONE && actual != global.type_hint) {
		error_at("Global variable '" + global.name + "' is declared as " +
			std::string(variant_type_name(global.type_hint)) +
			" but initialized with a value of type " + std::string(variant_type_name(actual)),
			line, column);
	}
}

void CodeGenerator::apply_default_initializer(IRGlobalVar& global, FunctionContext& init_func,
	size_t global_index, bool& has_global_init)
{
	global.value_type = global.type_hint;
	if (global.type_hint == IRInstruction::TypeHint_NONE) {
		return;
	}

	switch (global.type_hint) {
		case Variant::INT:
			global.init_type = IRGlobalVar::InitType::INT;
			global.init_value = int64_t(0);
			return;
		case Variant::FLOAT:
			global.init_type = IRGlobalVar::InitType::FLOAT;
			global.init_value = 0.0;
			return;
		case Variant::BOOL:
			global.init_type = IRGlobalVar::InitType::BOOL;
			global.init_value = false;
			return;
		case Variant::STRING:
			global.init_type = IRGlobalVar::InitType::STRING;
			global.init_value = std::string();
			return;
		case Variant::ARRAY:
			global.init_type = IRGlobalVar::InitType::EMPTY_ARRAY;
			return;
		case Variant::DICTIONARY:
			global.init_type = IRGlobalVar::InitType::EMPTY_DICT;
			return;
		default:
			break;
	}

	// Packed arrays: no compile-time representation, built by init function.
	const IROpcode make_op = packed_array_opcode(global.type_hint);
	if (make_op != IROpcode::LABEL) {
		int reg = alloc_register(init_func);
		IRInstruction make(make_op);
		make.operands.push_back(IRValue::reg(reg));
		make.operands.push_back(IRValue::imm(0));
		make.type_hint = global.type_hint;
		init_func.ir.instructions.push_back(make);
		init_func.ir.instructions.emplace_back(IROpcode::STORE_GLOBAL,
			IRValue::imm(static_cast<int64_t>(global_index)), IRValue::reg(reg));
		free_register(init_func, reg);
		global.init_type = IRGlobalVar::InitType::RUNTIME;
		has_global_init = true;
		return;
	}

	if (global.type_hint == Variant::OBJECT || global.type_hint == Variant::NIL) {
		return;
	}

	// Empty slot carries VASSIGN's INT32_MIN sentinel; first assignment would
	// adopt a scoped index that dies with the call.
	int reg = alloc_register(init_func);
	IRInstruction construct(IROpcode::CONSTRUCT, IRValue::reg(reg),
		IRValue::imm(static_cast<int64_t>(global.type_hint)), IRValue::imm(0));
	construct.type_hint = global.type_hint;
	init_func.ir.instructions.push_back(construct);
	set_register_type(init_func, reg, global.type_hint);
	init_func.ir.instructions.emplace_back(IROpcode::STORE_GLOBAL,
		IRValue::imm(static_cast<int64_t>(global_index)), IRValue::reg(reg));
	free_register(init_func, reg);
	global.init_type = IRGlobalVar::InitType::RUNTIME;
	has_global_init = true;
}

IROpcode CodeGenerator::packed_array_opcode(IRInstruction::TypeHint type) {
	switch (type) {
		case Variant::PACKED_BYTE_ARRAY: return IROpcode::MAKE_PACKED_BYTE_ARRAY;
		case Variant::PACKED_INT32_ARRAY: return IROpcode::MAKE_PACKED_INT32_ARRAY;
		case Variant::PACKED_INT64_ARRAY: return IROpcode::MAKE_PACKED_INT64_ARRAY;
		case Variant::PACKED_FLOAT32_ARRAY: return IROpcode::MAKE_PACKED_FLOAT32_ARRAY;
		case Variant::PACKED_FLOAT64_ARRAY: return IROpcode::MAKE_PACKED_FLOAT64_ARRAY;
		case Variant::PACKED_STRING_ARRAY: return IROpcode::MAKE_PACKED_STRING_ARRAY;
		case Variant::PACKED_VECTOR2_ARRAY: return IROpcode::MAKE_PACKED_VECTOR2_ARRAY;
		case Variant::PACKED_VECTOR3_ARRAY: return IROpcode::MAKE_PACKED_VECTOR3_ARRAY;
		case Variant::PACKED_VECTOR4_ARRAY: return IROpcode::MAKE_PACKED_VECTOR4_ARRAY;
		case Variant::PACKED_COLOR_ARRAY: return IROpcode::MAKE_PACKED_COLOR_ARRAY;
		default:
			// LABEL is never a construction opcode, so it doubles as "no default".
			return IROpcode::LABEL;
	}
}

static bool folded_truth(const IRGlobalVar& value, bool& out) {
	using InitType = IRGlobalVar::InitType;
	switch (value.init_type) {
		case InitType::BOOL: out = std::get<bool>(value.init_value); return true;
		case InitType::INT: out = std::get<int64_t>(value.init_value) != 0; return true;
		case InitType::FLOAT: out = std::get<double>(value.init_value) != 0.0; return true;
		case InitType::STRING: out = !std::get<std::string>(value.init_value).empty(); return true;
		case InitType::NULL_VAL:
		case InitType::EMPTY_ARRAY:
		case InitType::EMPTY_DICT: out = false; return true;
		case InitType::NONE:
		case InitType::RUNTIME: return false;
	}
	return false;
}

static bool fold_constant_binary(BinaryExpr::Op op, const IRGlobalVar& lhs,
	const IRGlobalVar& rhs, IRGlobalVar& out)
{
	using InitType = IRGlobalVar::InitType;
	using Op = BinaryExpr::Op;

	const auto set_int = [&](int64_t value) {
		out.init_type = InitType::INT;
		out.init_value = value;
		return true;
	};
	const auto set_float = [&](double value) {
		out.init_type = InitType::FLOAT;
		out.init_value = value;
		return true;
	};
	const auto set_bool = [&](bool value) {
		out.init_type = InitType::BOOL;
		out.init_value = value;
		return true;
	};

	if (lhs.init_type == InitType::STRING && rhs.init_type == InitType::STRING) {
		const std::string& left = std::get<std::string>(lhs.init_value);
		const std::string& right = std::get<std::string>(rhs.init_value);
		switch (op) {
			case Op::ADD:
				out.init_type = InitType::STRING;
				out.init_value = left + right;
				return true;
			case Op::EQ: return set_bool(left == right);
			case Op::NEQ: return set_bool(left != right);
			default: return false;
		}
	}

	if (lhs.init_type == InitType::BOOL && rhs.init_type == InitType::BOOL) {
		const bool left = std::get<bool>(lhs.init_value);
		const bool right = std::get<bool>(rhs.init_value);
		switch (op) {
			case Op::AND: return set_bool(left && right);
			case Op::OR: return set_bool(left || right);
			case Op::EQ: return set_bool(left == right);
			case Op::NEQ: return set_bool(left != right);
			default: return false;
		}
	}

	const auto is_number = [](const IRGlobalVar& value) {
		return value.init_type == InitType::INT || value.init_type == InitType::FLOAT;
	};
	if (!is_number(lhs) || !is_number(rhs)) {
		return false;
	}

	if (lhs.init_type == InitType::FLOAT || rhs.init_type == InitType::FLOAT) {
		const auto as_double = [](const IRGlobalVar& value) {
			return value.init_type == InitType::FLOAT
				? std::get<double>(value.init_value)
				: static_cast<double>(std::get<int64_t>(value.init_value));
		};
		const double left = as_double(lhs);
		const double right = as_double(rhs);
		switch (op) {
			case Op::ADD: return set_float(left + right);
			case Op::SUB: return set_float(left - right);
			case Op::MUL: return set_float(left * right);
			case Op::DIV:
				if (right == 0.0) return false;
				return set_float(left / right);
			case Op::MOD:
				if (right == 0.0) return false;
				return set_float(std::fmod(left, right));
			case Op::POW: return set_float(std::pow(left, right));
			case Op::EQ: return set_bool(left == right);
			case Op::NEQ: return set_bool(left != right);
			case Op::LT: return set_bool(left < right);
			case Op::LTE: return set_bool(left <= right);
			case Op::GT: return set_bool(left > right);
			case Op::GTE: return set_bool(left >= right);
			default: return false;
		}
	}

	const int64_t left = std::get<int64_t>(lhs.init_value);
	const int64_t right = std::get<int64_t>(rhs.init_value);
	const uint64_t uleft = static_cast<uint64_t>(left);
	const uint64_t uright = static_cast<uint64_t>(right);
	switch (op) {
		case Op::ADD: return set_int(static_cast<int64_t>(uleft + uright));
		case Op::SUB: return set_int(static_cast<int64_t>(uleft - uright));
		case Op::MUL: return set_int(static_cast<int64_t>(uleft * uright));
		case Op::DIV:
			if (right == 0 || (right == -1 && left == INT64_MIN)) return false;
			return set_int(left / right);
		case Op::MOD:
			if (right == 0 || (right == -1 && left == INT64_MIN)) return false;
			return set_int(left % right);
		case Op::POW: {
			const double result = std::pow(static_cast<double>(left), static_cast<double>(right));
			if (!std::isfinite(result) || result < static_cast<double>(INT64_MIN) ||
				result >= -static_cast<double>(INT64_MIN)) {
				return false;
			}
			return set_int(static_cast<int64_t>(result));
		}
		case Op::BIT_AND: return set_int(left & right);
		case Op::BIT_OR: return set_int(left | right);
		case Op::BIT_XOR: return set_int(left ^ right);
		case Op::SHL:
			if (right < 0) return false;
			return set_int(static_cast<int64_t>(uleft << (uright & 63)));
		case Op::SHR:
			if (right < 0) return false;
			return set_int(left >> (uright & 63));
		case Op::EQ: return set_bool(left == right);
		case Op::NEQ: return set_bool(left != right);
		case Op::LT: return set_bool(left < right);
		case Op::LTE: return set_bool(left <= right);
		case Op::GT: return set_bool(left > right);
		case Op::GTE: return set_bool(left >= right);
		case Op::AND:
		case Op::OR:
		case Op::IN: return false;
	}
	return false;
}

bool CodeGenerator::fold_global_initializer(const Expr* expr, IRGlobalVar& out,
	const FunctionContext* func, const StructDecl* owner) const
{
	using InitType = IRGlobalVar::InitType;

	const auto shadowed = [&](const std::string& name) {
		if (func == nullptr) {
			return false;
		}
		for (const Scope& scope : func->scopes) {
			if (scope.variables.count(name) != 0) {
				return true;
			}
		}
		return false;
	};
	const auto take = [&](const IRGlobalVar& value) {
		out.init_type = value.init_type;
		out.init_value = value.init_value;
		return true;
	};

	if (auto* lit = dynamic_cast<const LiteralExpr*>(expr)) {
		switch (lit->lit_type) {
			case LiteralExpr::Type::INTEGER:
				out.init_type = InitType::INT;
				out.init_value = std::get<int64_t>(lit->value);
				return true;
			case LiteralExpr::Type::FLOAT:
				out.init_type = InitType::FLOAT;
				out.init_value = std::get<double>(lit->value);
				return true;
			case LiteralExpr::Type::STRING:
				// StringName/NodePath: defer to run-time initializer.
				if (lit->string_type != LiteralExpr::StringType::PLAIN) {
					return false;
				}
				out.init_type = InitType::STRING;
				out.init_value = std::get<std::string>(lit->value);
				return true;
			case LiteralExpr::Type::BOOL:
				out.init_type = InitType::BOOL;
				out.init_value = std::get<bool>(lit->value);
				return true;
			case LiteralExpr::Type::NULL_VAL:
				out.init_type = InitType::NULL_VAL;
				return true;
		}
		return false;
	}

	// '-5' parses as NEG(5); fold here so it remains a constant.
	if (auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
		IRGlobalVar inner;
		if (!fold_global_initializer(unary->operand.get(), inner, func, owner)) {
			return false;
		}
		switch (unary->op) {
			case UnaryExpr::Op::NEG:
				if (inner.init_type == InitType::INT) {
					// Unsigned: signed negate of INT64_MIN overflows.
					const uint64_t value = static_cast<uint64_t>(std::get<int64_t>(inner.init_value));
					out.init_type = InitType::INT;
					out.init_value = static_cast<int64_t>(0u - value);
					return true;
				}
				if (inner.init_type == InitType::FLOAT) {
					out.init_type = InitType::FLOAT;
					out.init_value = -std::get<double>(inner.init_value);
					return true;
				}
				return false;
			case UnaryExpr::Op::BIT_NOT:
				if (inner.init_type == InitType::INT) {
					out.init_type = InitType::INT;
					out.init_value = ~std::get<int64_t>(inner.init_value);
					return true;
				}
				return false;
			case UnaryExpr::Op::NOT:
				out.init_type = InitType::BOOL;
				switch (inner.init_type) {
					case InitType::BOOL: out.init_value = !std::get<bool>(inner.init_value); return true;
					case InitType::INT: out.init_value = std::get<int64_t>(inner.init_value) == 0; return true;
					case InitType::FLOAT: out.init_value = std::get<double>(inner.init_value) == 0.0; return true;
					case InitType::NULL_VAL: out.init_value = true; return true;
					case InitType::STRING: out.init_value = std::get<std::string>(inner.init_value).empty(); return true;
					default: return false;
				}
		}
		return false;
	}

	if (auto* binary = dynamic_cast<const BinaryExpr*>(expr)) {
		IRGlobalVar lhs;
		IRGlobalVar rhs;
		if (!fold_global_initializer(binary->left.get(), lhs, func, owner) ||
			!fold_global_initializer(binary->right.get(), rhs, func, owner)) {
			return false;
		}
		return fold_constant_binary(binary->op, lhs, rhs, out);
	}

	if (auto* ternary = dynamic_cast<const TernaryExpr*>(expr)) {
		IRGlobalVar condition;
		IRGlobalVar when_true;
		IRGlobalVar when_false;
		bool truth = false;
		if (!fold_global_initializer(ternary->condition.get(), condition, func, owner) ||
			!fold_global_initializer(ternary->true_value.get(), when_true, func, owner) ||
			!fold_global_initializer(ternary->false_value.get(), when_false, func, owner) ||
			!folded_truth(condition, truth)) {
			return false;
		}
		return take(truth ? when_true : when_false);
	}

	if (auto* var = dynamic_cast<const VariableExpr*>(expr)) {
		if (shadowed(var->name)) {
			return false;
		}
		for (const StructDecl* at = owner; at != nullptr; at = class_base(*at)) {
			auto it = m_class_constants.find(at->name + "." + var->name);
			if (it != m_class_constants.end()) {
				return take(it->second);
			}
		}
		if (auto it = m_global_const_values.find(var->name); it != m_global_const_values.end()) {
			return take(it->second);
		}
		if (auto it = m_enum_members.find(var->name); it != m_enum_members.end()) {
			if (it->second->value_expr != nullptr) {
				return false;
			}
			out.init_type = InitType::INT;
			out.init_value = it->second->value;
			return true;
		}
		if (const GlobalConstant* constant = find_global_constant(var->name)) {
			out.init_type = constant->is_float ? InitType::FLOAT : InitType::INT;
			if (constant->is_float) {
				out.init_value = constant->float_value;
			} else {
				out.init_value = constant->int_value;
			}
			return true;
		}
		return false;
	}

	if (auto* member = dynamic_cast<const MemberCallExpr*>(expr);
		member != nullptr && !member->is_method_call && member->arguments.empty()) {
		auto* object = dynamic_cast<const VariableExpr*>(member->object.get());
		if (object == nullptr || shadowed(object->name)) {
			return false;
		}
		if (const StructDecl* decl = find_struct(object->name)) {
			for (const StructDecl* at = decl; at != nullptr; at = class_base(*at)) {
				auto it = m_class_constants.find(at->name + "." + member->member_name);
				if (it != m_class_constants.end()) {
					return take(it->second);
				}
			}
			return false;
		}
		if (auto it = m_enums.find(object->name); it != m_enums.end()) {
			const EnumDecl::Member* value = it->second->find_member(member->member_name);
			if (value == nullptr || value->value_expr != nullptr) {
				return false;
			}
			out.init_type = InitType::INT;
			out.init_value = value->value;
			return true;
		}
		if (const GlobalEnumValue* value =
			find_global_enum_value(object->name, member->member_name)) {
			out.init_type = InitType::INT;
			out.init_value = value->value;
			return true;
		}
		return false;
	}

	// Empty containers fold to InitType; backend writes directly.
	if (auto* array_lit = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
		if (array_lit->elements.empty()) {
			out.init_type = InitType::EMPTY_ARRAY;
			return true;
		}
		return false;
	}
	if (auto* dict_lit = dynamic_cast<const DictionaryLiteralExpr*>(expr)) {
		if (dict_lit->elements.empty()) {
			out.init_type = InitType::EMPTY_DICT;
			return true;
		}
		return false;
	}

	return false;
}

IRInstruction::TypeHint CodeGenerator::fused_compare_type(IRInstruction::TypeHint left,
                                                          IRInstruction::TypeHint right) {
	// Native compare only when both operands share a known type.
	if (left == IRInstruction::TypeHint_NONE || left != right) {
		return IRInstruction::TypeHint_NONE;
	}
	if (left == Variant::INT || left == Variant::FLOAT || TypeHintUtils::is_vector(left)) {
		return left;
	}
	return IRInstruction::TypeHint_NONE;
}

int CodeGenerator::gen_const_global_value(const std::string& name, FunctionContext& func) {
	auto it = m_global_const_values.find(name);
	if (it == m_global_const_values.end()) {
		return -1;
	}
	return gen_folded_const(it->second, func);
}

int CodeGenerator::gen_folded_const(const IRGlobalVar& global, FunctionContext& func) {
	// Container consts are shared handles; stays on LOAD_GLOBAL.
	using InitType = IRGlobalVar::InitType;
	switch (global.init_type) {
		case InitType::INT: {
			int reg = alloc_register(func);
			IRInstruction instr(IROpcode::LOAD_IMM, IRValue::reg(reg),
			                    IRValue::imm(std::get<int64_t>(global.init_value)));
			instr.type_hint = Variant::INT;
			func.ir.instructions.push_back(instr);
			set_register_type(func, reg, Variant::INT);
			return reg;
		}
		case InitType::FLOAT: {
			int reg = alloc_register(func);
			IRInstruction instr(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(reg),
			                    IRValue::fimm(std::get<double>(global.init_value)));
			instr.type_hint = Variant::FLOAT;
			func.ir.instructions.push_back(instr);
			set_register_type(func, reg, Variant::FLOAT);
			return reg;
		}
		case InitType::BOOL: {
			int reg = alloc_register(func);
			IRInstruction instr(IROpcode::LOAD_BOOL, IRValue::reg(reg),
			                    IRValue::imm(std::get<bool>(global.init_value) ? 1 : 0));
			instr.type_hint = Variant::BOOL;
			func.ir.instructions.push_back(instr);
			set_register_type(func, reg, Variant::BOOL);
			return reg;
		}
		case InitType::STRING: {
			int reg = alloc_register(func);
			int str_idx = add_string_constant(std::get<std::string>(global.init_value));
			IRInstruction instr(IROpcode::LOAD_STRING, IRValue::reg(reg), IRValue::imm(str_idx));
			instr.type_hint = Variant::STRING;
			func.ir.instructions.push_back(instr);
			set_register_type(func, reg, Variant::STRING);
			return reg;
		}
		case InitType::NONE:
		case InitType::NULL_VAL:
		case InitType::EMPTY_ARRAY:
		case InitType::EMPTY_DICT:
		case InitType::RUNTIME:
			return -1;
	}
	return -1;
}

IRInstruction::TypeHint CodeGenerator::derive_global_value_type(const IRGlobalVar& global) {
	// Explicit type hint wins.
	if (global.type_hint != IRInstruction::TypeHint_NONE) {
		return global.type_hint;
	}
	switch (global.init_type) {
		case IRGlobalVar::InitType::INT: return Variant::INT;
		case IRGlobalVar::InitType::FLOAT: return Variant::FLOAT;
		case IRGlobalVar::InitType::BOOL: return Variant::BOOL;
		case IRGlobalVar::InitType::STRING: return Variant::STRING;
		case IRGlobalVar::InitType::EMPTY_ARRAY: return Variant::ARRAY;
		case IRGlobalVar::InitType::EMPTY_DICT: return Variant::DICTIONARY;
		case IRGlobalVar::InitType::NULL_VAL:
		case IRGlobalVar::InitType::NONE:
		case IRGlobalVar::InitType::RUNTIME:
			// RUNTIME: caller fills type; untyped stays any-Variant.
			return IRInstruction::TypeHint_NONE;
	}
	return IRInstruction::TypeHint_NONE;
}

bool CodeGenerator::is_global_variable(const std::string& name) const {
	return m_global_variables.find(name) != m_global_variables.end();
}

bool CodeGenerator::is_global_const(const std::string& name) const {
	return m_global_consts.find(name) != m_global_consts.end();
}

int CodeGenerator::gen_string_value(const std::string& text, FunctionContext& func) {
	const int reg = alloc_register(func);
	IRInstruction load(IROpcode::LOAD_STRING, IRValue::reg(reg),
		IRValue::imm(add_string_constant(text)));
	load.type_hint = Variant::STRING;
	func.ir.instructions.push_back(load);
	set_register_type(func, reg, Variant::STRING);
	return reg;
}

int CodeGenerator::gen_global_enum_value(const std::string& enum_name,
	const std::string& member_name, FunctionContext& func, const Expr* site)
{
	const GlobalEnumValue* value = find_global_enum_value(enum_name, member_name);
	if (value == nullptr) {
		error_at("Global enum '" + enum_name + "' has no member named '" +
			member_name + "'", site);
	}
	return gen_int_immediate(value->value, func);
}

int CodeGenerator::gen_engine_class_constant(const std::string& class_name,
	const std::string& constant_name, FunctionContext& func)
{
	const int class_db_reg = gen_global_class_get("ClassDB", func);
	const int class_reg = gen_string_value(class_name, func);
	const int name_reg = gen_string_value(constant_name, func);

	const int result_reg = alloc_register(func);
	IRInstruction vcall(IROpcode::VCALL);
	vcall.operands.push_back(IRValue::reg(result_reg));
	vcall.operands.push_back(IRValue::reg(class_db_reg));
	vcall.operands.push_back(ir_str("class_get_integer_constant"));
	vcall.operands.push_back(IRValue::imm(2));
	vcall.operands.push_back(IRValue::reg(class_reg));
	vcall.operands.push_back(IRValue::reg(name_reg));
	vcall.type_hint = Variant::INT;
	func.ir.instructions.push_back(vcall);

	free_register(func, name_reg);
	free_register(func, class_reg);
	free_register(func, class_db_reg);
	set_register_type(func, result_reg, Variant::INT);
	return result_reg;
}

int CodeGenerator::gen_engine_class_static_call(const std::string& class_name,
	const MemberCallExpr* expr, FunctionContext& func)
{
	reject_named_arguments(*expr, "'" + expr->member_name + "'", expr);

	const int class_db_reg = gen_global_class_get("ClassDB", func);
	const int class_reg = gen_string_value(class_name, func);
	const int name_reg = gen_string_value(expr->member_name, func);

	std::vector<int> arg_regs;
	for (const auto& argument : expr->arguments) {
		arg_regs.push_back(gen_expr(argument.get(), func));
	}

	const int result_reg = alloc_register(func);
	IRInstruction vcall(IROpcode::VCALL);
	vcall.operands.push_back(IRValue::reg(result_reg));
	vcall.operands.push_back(IRValue::reg(class_db_reg));
	vcall.operands.push_back(ir_str("class_call_static"));
	vcall.operands.push_back(IRValue::imm(static_cast<int64_t>(2 + arg_regs.size())));
	vcall.operands.push_back(IRValue::reg(class_reg));
	vcall.operands.push_back(IRValue::reg(name_reg));
	for (int arg_reg : arg_regs) {
		vcall.operands.push_back(IRValue::reg(arg_reg));
	}
	func.ir.instructions.push_back(vcall);

	for (int reg : arg_regs) {
		free_register(func, reg);
	}
	free_register(func, name_reg);
	free_register(func, class_reg);
	free_register(func, class_db_reg);
	return result_reg;
}

int CodeGenerator::gen_script_class_new(const std::string& class_name, const std::string& path,
	const MemberCallExpr* expr, FunctionContext& func)
{
	std::vector<int> arg_regs;
	for (const auto& argument : expr->arguments) {
		arg_regs.push_back(gen_expr(argument.get(), func));
	}

	const int script_reg = gen_load_resource(path, func);
	const int result_reg = alloc_register(func);

	IRInstruction vcall(IROpcode::VCALL);
	vcall.operands.push_back(IRValue::reg(result_reg));
	vcall.operands.push_back(IRValue::reg(script_reg));
	vcall.operands.push_back(ir_str("new"));
	vcall.operands.push_back(IRValue::imm(static_cast<int64_t>(arg_regs.size())));
	for (int arg_reg : arg_regs) {
		vcall.operands.push_back(IRValue::reg(arg_reg));
	}
	vcall.type_hint = Variant::OBJECT;
	func.ir.instructions.push_back(vcall);

	free_register(func, script_reg);
	for (int reg : arg_regs) {
		free_register(func, reg);
	}
	set_register_type(func, result_reg, Variant::OBJECT);
	return result_reg;
}

int CodeGenerator::gen_engine_class_new(const std::string& class_name, const MemberCallExpr* expr,
	FunctionContext& func)
{
	if (!expr->arguments.empty()) {
		error_at("'" + class_name + ".new()' takes no arguments", expr,
			"An engine class is constructed empty and configured afterwards, as in GDScript");
	}
	int result_reg = alloc_register(func);
	int str_idx = add_string_constant(class_name);

	IRInstruction instr(IROpcode::CALL_SYSCALL);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::imm(ECALL_NODE_CREATE));
	instr.operands.push_back(IRValue::imm(str_idx));
	instr.operands.push_back(IRValue::imm(static_cast<int64_t>(class_name.length())));

	func.ir.instructions.push_back(instr);
	set_register_type(func, result_reg, Variant::OBJECT);

	return result_reg;
}

int CodeGenerator::gen_global_class_get(const std::string& class_name, FunctionContext& func) {
	int result_reg = alloc_register(func);
	int str_idx = add_string_constant(class_name);

	IRInstruction instr(IROpcode::CALL_SYSCALL);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::imm(ECALL_GET_OBJ));
	instr.operands.push_back(IRValue::imm(str_idx));
	instr.operands.push_back(IRValue::imm(static_cast<int64_t>(class_name.length())));

	func.ir.instructions.push_back(instr);
	set_register_type(func, result_reg, IRInstruction::TypeHint_NONE);

	return result_reg;
}

} // namespace gdscript
