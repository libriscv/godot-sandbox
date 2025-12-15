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
#include "core/object/object.h"
#include "core/variant/variant.h"

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
			Callable::CallError ce;
			Variant setter_args[1] = { p_value };
			callp(member.setter, (const Variant **)setter_args, 1, ce);
			return ce.error == Callable::CallError::CALL_OK;
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
			Callable::CallError ce;
			r_ret = callp(member.getter, nullptr, 0, ce);
			return ce.error == Callable::CallError::CALL_OK;
		}
	}

	// Check constants
	if (script->constants.has(p_name)) {
		r_ret = script->constants[p_name];
		return true;
	}

	return false;
}

void GDScriptELFInstance::get_property_list(List<PropertyInfo> *p_properties) const {
	if (!script.is_valid()) {
		return;
	}

	for (const KeyValue<StringName, GDScriptELF::MemberInfo> &E : script->member_indices) {
		p_properties->push_back(E.value.property_info);
	}
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

void GDScriptELFInstance::validate_property(PropertyInfo &p_property) const {
	// Property validation
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

void GDScriptELFInstance::get_method_list(List<MethodInfo> *p_list) const {
	if (!script.is_valid()) {
		return;
	}

	script->get_script_method_list(p_list);
}

bool GDScriptELFInstance::has_method(const StringName &p_method) const {
	if (!script.is_valid()) {
		return false;
	}

	return script->has_method(p_method);
}

int GDScriptELFInstance::get_method_argument_count(const StringName &p_method, bool *r_is_valid) const {
	if (!script.is_valid()) {
		if (r_is_valid) {
			*r_is_valid = false;
		}
		return -1;
	}

	if (script->member_functions.has(p_method)) {
		GDScriptELFFunction *func = script->member_functions[p_method];
		if (r_is_valid) {
			*r_is_valid = true;
		}
		return func->get_argument_count();
	}

	if (r_is_valid) {
		*r_is_valid = false;
	}
	return -1;
}

Variant GDScriptELFInstance::callp(const StringName &p_method, const Variant **p_args, int p_argcount, Callable::CallError &r_error) {
	if (!script.is_valid()) {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
		return Variant();
	}

	if (script->member_functions.has(p_method)) {
		GDScriptELFFunction *func = script->member_functions[p_method];
		return func->call(this, p_args, p_argcount, r_error);
	}

	r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
	return Variant();
}

void GDScriptELFInstance::notification(int p_notification, bool p_reversed) {
	if (!script.is_valid()) {
		return;
	}

	// Call _ready if available
	if (p_notification == NOTIFICATION_READY && script->implicit_ready) {
		Callable::CallError ce;
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

Ref<Script> GDScriptELFInstance::get_script() const {
	return script;
}

ScriptLanguage *GDScriptELFInstance::get_language() {
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
				members[E.value.index] = script->get_property_default_value(E.key);
			}
		}
	}
}

const Variant GDScriptELFInstance::get_rpc_config() const {
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
		Callable::CallError ce;
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

