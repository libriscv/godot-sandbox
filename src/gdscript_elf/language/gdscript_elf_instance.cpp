/**************************************************************************/
/*  gdscript_elf_instance.cpp                                            */
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

#include "gdscript_elf_instance.h"
#include "gdscript_elf_language.h"
#include "../../sandbox.h"
#include "../../elf/script_instance_helper.h"
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/core/property_info.hpp>
#include <godot_cpp/templates/list.hpp>

using namespace godot;

GDScriptELFInstance::GDScriptELFInstance() {
	owner = nullptr;
	sandbox = nullptr;
}

GDScriptELFInstance::~GDScriptELFInstance() {
	if (sandbox) {
		memdelete(sandbox);
		sandbox = nullptr;
	}
}

bool GDScriptELFInstance::set(const StringName &p_name, const Variant &p_value) {
	if (!script.is_valid()) {
		return false;
	}

	// Check if it's a member
	if (script->member_indices.has(p_name)) {
		const GDScriptELF::MemberInfo &member = script->member_indices[p_name];
		if (member.index >= 0 && member.index < members.size()) {
			members[member.index] = p_value;
			return true;
		}
	}

	// Check if it's a property setter
	if (script->member_indices.has(p_name)) {
		const GDScriptELF::MemberInfo &member = script->member_indices[p_name];
		if (!member.setter.is_empty()) {
			GDExtensionCallError ce;
			const Variant *setter_args[1] = { &p_value };
			callp(member.setter, setter_args, 1, ce);
			return ce.error == GDEXTENSION_CALL_OK;
		}
	}

	return false;
}

bool GDScriptELFInstance::get(const StringName &p_name, Variant &r_ret) const {
	if (!script.is_valid()) {
		return false;
	}

	// Check if it's a member
	if (script->member_indices.has(p_name)) {
		const GDScriptELF::MemberInfo &member = script->member_indices[p_name];
		if (member.index >= 0 && member.index < members.size()) {
			r_ret = members[member.index];
			return true;
		}
	}

	// Check if it's a property getter
	if (script->member_indices.has(p_name)) {
		const GDScriptELF::MemberInfo &member = script->member_indices[p_name];
		if (!member.getter.is_empty()) {
			GDExtensionCallError ce;
			r_ret = callp(member.getter, nullptr, 0, ce);
			return ce.error == GDEXTENSION_CALL_OK;
		}
	}

	// Check constants
	if (script->constants.has(p_name)) {
		r_ret = script->constants[p_name];
		return true;
	}

	return false;
}

const GDExtensionPropertyInfo *GDScriptELFInstance::get_property_list(uint32_t *r_count) const {
	if (!script.is_valid()) {
		*r_count = 0;
		return nullptr;
	}

	const int size = script->member_indices.size();
	GDExtensionPropertyInfo *list = memnew_arr(GDExtensionPropertyInfo, size);
	int i = 0;
	for (const KeyValue<StringName, GDScriptELF::MemberInfo> &E : script->member_indices) {
		list[i] = create_property_info(E.value.property_info);
		i++;
	}

	*r_count = size;
	return list;
}

void GDScriptELFInstance::free_property_list(const GDExtensionPropertyInfo *p_list, uint32_t p_count) const {
	if (!p_list) {
		return;
	}
	for (uint32_t i = 0; i < p_count; i++) {
		free_property_info(p_list[i]);
	}
	memdelete_arr(p_list);
}

Variant::Type GDScriptELFInstance::get_property_type(const StringName &p_name, bool *r_is_valid) const {
	if (!script.is_valid()) {
		if (r_is_valid) {
			*r_is_valid = false;
		}
		return Variant::NIL;
	}

	if (script->member_indices.has(p_name)) {
		const GDScriptELF::MemberInfo &member = script->member_indices[p_name];
		if (r_is_valid) {
			*r_is_valid = true;
		}
		return member.property_info.type;
	}

	if (r_is_valid) {
		*r_is_valid = false;
	}
	return Variant::NIL;
}

bool GDScriptELFInstance::validate_property(GDExtensionPropertyInfo &p_property) const {
	// Property validation - return true if valid
	return true;
}

bool GDScriptELFInstance::property_can_revert(const StringName &p_name) const {
	if (!script.is_valid()) {
		return false;
	}

	return script->has_property_default_value(p_name);
}

bool GDScriptELFInstance::property_get_revert(const StringName &p_name, Variant &r_ret) const {
	if (!script.is_valid()) {
		return false;
	}

	if (script->has_property_default_value(p_name)) {
		r_ret = script->get_property_default_value(p_name);
		return true;
	}

	return false;
}

const GDExtensionMethodInfo *GDScriptELFInstance::get_method_list(uint32_t *r_count) const {
	if (!script.is_valid()) {
		*r_count = 0;
		return nullptr;
	}

	List<MethodInfo> method_list;
	script->get_script_method_list(&method_list);

	const int size = method_list.size();
	GDExtensionMethodInfo *list = memnew_arr(GDExtensionMethodInfo, size);
	int i = 0;
	for (const MethodInfo &method_info : method_list) {
		list[i] = create_method_info(method_info);
		i++;
	}

	*r_count = size;
	return list;
}

void GDScriptELFInstance::free_method_list(const GDExtensionMethodInfo *p_list, uint32_t p_count) const {
	if (!p_list) {
		return;
	}
	for (uint32_t i = 0; i < p_count; i++) {
		free_method_info(p_list[i]);
	}
	memdelete_arr(p_list);
}

bool GDScriptELFInstance::has_method(const StringName &p_method) const {
	if (!script.is_valid()) {
		return false;
	}

	return script->has_method(p_method);
}

GDExtensionInt GDScriptELFInstance::get_method_argument_count(const StringName &p_method, bool &r_valid) const {
	if (!script.is_valid()) {
		r_valid = false;
		return -1;
	}

	if (script->member_functions.has(p_method)) {
		GDScriptELFFunction *func = script->member_functions[p_method];
		r_valid = true;
		return func->get_argument_count();
	}

	r_valid = false;
	return -1;
}

Variant GDScriptELFInstance::callp(const StringName &p_method, const Variant **p_args, int p_argcount, GDExtensionCallError &r_error) {
	if (!script.is_valid()) {
		r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return Variant();
	}

	if (script->member_functions.has(p_method)) {
		GDScriptELFFunction *func = script->member_functions[p_method];
		return func->call(this, p_args, p_argcount, r_error);
	}

	r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
	return Variant();
}

void GDScriptELFInstance::notification(int p_notification, bool p_reversed) {
	if (!script.is_valid()) {
		return;
	}

	// Call _ready if available
	if (p_notification == NOTIFICATION_READY && script->implicit_ready) {
		GDExtensionCallError ce;
		Variant result = script->implicit_ready->call(this, nullptr, 0, ce);
		(void)result; // Suppress unused variable warning
	}
}

String GDScriptELFInstance::to_string(bool *r_valid) {
	if (r_valid) {
		*r_valid = true;
	}
	return String("[GDScriptELFInstance]");
}

void GDScriptELFInstance::refcount_incremented() {
	// Handle reference count increment if needed
}

bool GDScriptELFInstance::refcount_decremented() {
	// Handle reference count decrement
	// Return true if object should be freed
	return false;
}

bool GDScriptELFInstance::is_placeholder() const {
	return false; // GDScriptELF instances are never placeholders
}

void GDScriptELFInstance::property_set_fallback(const StringName &p_name, const Variant &p_value, bool *r_valid) {
	if (r_valid) {
		*r_valid = false;
	}
	// Try to set property using standard set method
	if (const_cast<GDScriptELFInstance *>(this)->set(p_name, p_value)) {
		if (r_valid) {
			*r_valid = true;
		}
	}
}

Variant GDScriptELFInstance::property_get_fallback(const StringName &p_name, bool *r_valid) {
	if (r_valid) {
		*r_valid = false;
	}
	Variant ret;
	if (const_cast<GDScriptELFInstance *>(this)->get(p_name, ret)) {
		if (r_valid) {
			*r_valid = true;
		}
		return ret;
	}
	return Variant();
}

void GDScriptELFInstance::get_property_state(GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) {
	if (!script.is_valid()) {
		return;
	}
	// Add all member properties to the state
	for (const KeyValue<StringName, GDScriptELF::MemberInfo> &E : script->member_indices) {
		if (E.value.index >= 0 && E.value.index < members.size()) {
			p_add_func(E.key, members[E.value.index], p_userdata);
		}
	}
}

Ref<Script> GDScriptELFInstance::get_script() const {
	return script;
}

ScriptLanguage *GDScriptELFInstance::_get_language() {
	return GDScriptELFLanguage::get_singleton();
}

void GDScriptELFInstance::set_path(const String &p_path) {
	// Set path if needed
}

void GDScriptELFInstance::reload_members() {
	if (!script.is_valid()) {
		return;
	}

	// Resize members array to match script member count
	int max_index = -1;
	for (const KeyValue<StringName, GDScriptELF::MemberInfo> &E : script->member_indices) {
		if (E.value.index > max_index) {
			max_index = E.value.index;
		}
	}

	if (max_index >= 0) {
		members.resize(max_index + 1);
		// Initialize with default values
		for (const KeyValue<StringName, GDScriptELF::MemberInfo> &E : script->member_indices) {
			if (script->has_property_default_value(E.key)) {
				members.write[E.value.index] = script->get_property_default_value(E.key);
			}
		}
	}
}

Variant GDScriptELFInstance::get_rpc_config() const {
	if (!script.is_valid()) {
		return Variant();
	}
	return script->get_rpc_config();
}

void GDScriptELFInstance::_call_implicit_ready_recursively(GDScriptELF *p_script) {
	if (!p_script) {
		return;
	}

	// Call base script's _ready first
	if (p_script->base.is_valid()) {
		_call_implicit_ready_recursively(p_script->base.ptr());
	}

	// Call this script's _ready
	if (p_script->implicit_ready) {
		GDExtensionCallError ce;
		p_script->implicit_ready->call(this, nullptr, 0, ce);
	}
}

void GDScriptELFInstance::_initialize_sandbox() {
	if (sandbox) {
		return; // Already initialized
	}

	// Create sandbox for ELF execution
	// This will be used by GDScriptELFFunction to execute ELF binaries
	sandbox = memnew(Sandbox);
}

