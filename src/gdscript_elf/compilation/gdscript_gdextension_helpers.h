/**************************************************************************/
/*  gdscript_gdextension_helpers.h                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/variant/variant.hpp>

using namespace godot;

// Helper function to replace Variant::get_validated_object_with_check()
// In GDExtension, we use ObjectDB::get_instance() to validate objects
inline Object *get_validated_object_safe(const Variant &p_variant, bool &r_was_freed) {
	if (p_variant.get_type() != Variant::OBJECT) {
		r_was_freed = false;
		return nullptr;
	}

	// Try to get the object using operator Object*()
	Object *obj = p_variant.operator Object *();
	if (!obj) {
		r_was_freed = false;
		return nullptr;
	}

	// Validate using instance ID
	uint64_t instance_id = obj->get_instance_id();
	Object *validated = ObjectDB::get_instance(instance_id);
	
	// If validated is null but obj was not null, the object was likely freed
	r_was_freed = (validated == nullptr && obj != nullptr);
	return validated;
}

// Helper function to replace Object::get_script_instance()
// In GDExtension, we can't directly get ScriptInstanceExtension
// This is a limitation - return nullptr for now
// Note: ScriptInstanceExtension is a forward-declared type, not a concrete class
inline void *get_script_instance_safe(Object *p_obj) {
	if (!p_obj) {
		return nullptr;
	}
	Ref<Script> script = p_obj->get_script();
	if (!script.is_valid()) {
		return nullptr;
	}
	// In GDExtension, we can't directly get ScriptInstanceExtension
	// This is a limitation - we may need to store script instances differently
	// For now, return nullptr and handle cases where script instance is needed
	return nullptr; // TODO: Implement proper script instance retrieval
}

// Function pointer types to replace Variant::ValidatedOperatorEvaluator and related types
typedef void (*OperatorEvaluatorFunc)(Variant *r_ret, const Variant *p_a, const Variant *p_b, Variant::Operator p_op);
typedef void (*SetterFunc)(Variant *p_dst, const Variant *p_value);
typedef void (*GetterFunc)(const Variant *p_src, Variant *p_dst);
typedef void (*KeyedSetterFunc)(Variant *p_dst, const Variant *p_key, const Variant *p_value);
typedef void (*KeyedGetterFunc)(const Variant *p_src, const Variant *p_key, Variant *p_dst);
typedef void (*IndexedSetterFunc)(Variant *p_dst, int32_t p_index, const Variant *p_value);
typedef void (*IndexedGetterFunc)(const Variant *p_src, int32_t p_index, Variant *p_dst);
typedef void (*BuiltInMethodFunc)(Variant *r_ret, const Variant **p_args, int32_t p_arg_count, GDExtensionCallError &r_error);
typedef void (*ConstructorFunc)(Variant *r_ret, const Variant **p_args, int32_t p_arg_count, GDExtensionCallError &r_error);
typedef void (*UtilityFunctionFunc)(Variant *r_ret, const Variant **p_args, int32_t p_arg_count, GDExtensionCallError &r_error);

// Wrapper function for operator evaluation using Variant::evaluate()
inline void operator_evaluator_wrapper(Variant *r_ret, const Variant *p_a, const Variant *p_b, Variant::Operator p_op) {
	bool valid = false;
	Variant::evaluate(p_op, *p_a, *p_b, *r_ret, valid);
	if (!valid) {
		// If evaluation fails, set to nil
		*r_ret = Variant();
	}
}
