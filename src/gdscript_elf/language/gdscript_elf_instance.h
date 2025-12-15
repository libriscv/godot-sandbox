/**************************************************************************/
/*  gdscript_elf_instance.h                                              */
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

#include "gdscript_elf.h"
#include "gdscript_elf_function.h"
#include "core/object/script_instance.h"
#include "core/templates/self_list.h"

class Sandbox;

class GDScriptELFInstance : public ScriptInstance {
	friend class GDScriptELF;
	friend class GDScriptELFFunction;

	ObjectID owner_id;
	Object *owner = nullptr;
	Ref<GDScriptELF> script;
	Vector<Variant> members;

	// Sandbox for ELF execution
	Sandbox *sandbox = nullptr;

	SelfList<GDScriptELFInstance> script_instance_list;

	void _call_implicit_ready_recursively(GDScriptELF *p_script);
	void _initialize_sandbox();

public:
	virtual Object *get_owner() override { return owner; }

	virtual bool set(const StringName &p_name, const Variant &p_value) override;
	virtual bool get(const StringName &p_name, Variant &r_ret) const override;
	virtual void get_property_list(List<PropertyInfo> *p_properties) const override;
	virtual Variant::Type get_property_type(const StringName &p_name, bool *r_is_valid = nullptr) const override;
	virtual void validate_property(PropertyInfo &p_property) const override;

	virtual bool property_can_revert(const StringName &p_name) const override;
	virtual bool property_get_revert(const StringName &p_name, Variant &r_ret) const override;

	virtual void get_method_list(List<MethodInfo> *p_list) const override;
	virtual bool has_method(const StringName &p_method) const override;

	virtual int get_method_argument_count(const StringName &p_method, bool *r_is_valid = nullptr) const override;

	virtual Variant callp(const StringName &p_method, const Variant **p_args, int p_argcount, Callable::CallError &r_error) override;

	virtual void notification(int p_notification, bool p_reversed = false) override;
	String to_string(bool *r_valid);

	virtual Ref<Script> get_script() const override;

	virtual ScriptLanguage *get_language() override;

	void set_path(const String &p_path);

	void reload_members();

	virtual const Variant get_rpc_config() const override;

	GDScriptELFInstance();
	~GDScriptELFInstance();
};

