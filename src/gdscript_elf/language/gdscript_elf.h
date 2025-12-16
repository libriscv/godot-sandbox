/**************************************************************************/
/*  gdscript_elf.h                                                        */
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

#include "../compilation/gdscript_types.h"
#include "../compilation/gdscript_function.h"
#include "../compilation/gdscript_parser.h"
#include "../compilation/gdscript_analyzer.h"
#include "../compilation/gdscript_compiler.h"
#include "../elf/gdscript_bytecode_elf_compiler.h"
#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/self_list.hpp>
#include <gdextension_interface.h>
#include "../../godot/script_instance.h"

using namespace godot;

class GDScriptELFLanguage;
class GDScriptELFInstance;
class GDScriptELFFunction;

// GDScriptELF - Script implementation that compiles GDScript to ELF
// Similar to GDScript but compiles to ELF instead of VM bytecode
class GDScriptELF : public ScriptExtension {
	GDCLASS(GDScriptELF, ScriptExtension);

	friend class GDScriptELFInstance;
	friend class GDScriptELFFunction;
	friend class GDScriptELFLanguage;
	friend class GDScriptELFCompiler;

	bool tool = false;
	bool valid = false;
	bool reloading = false;
	bool is_abstract_flag = false;

	struct MemberInfo {
		int index = 0;
		StringName setter;
		StringName getter;
		GDScriptDataType data_type;
		PropertyInfo property_info;
	};

	Ref<GDScriptELF> base;
	GDScriptELF *_base = nullptr; //fast pointer access
	GDScriptELF *_script_owner = nullptr; //for nested classes

protected:

	// Members are just indices to the instantiated script.
	HashMap<StringName, MemberInfo> member_indices;
	HashSet<StringName> members;

	HashMap<StringName, Variant> constants;
	HashMap<StringName, GDScriptELFFunction *> member_functions;
	HashMap<StringName, Ref<GDScriptELF>> subclasses; //nested classes
	HashMap<StringName, MethodInfo> _signals;
	Dictionary rpc_config;

	// ELF-specific: Store compiled ELF binaries for functions
	HashMap<StringName, PackedByteArray> function_elf_binaries;

	GDScriptELFFunction *initializer = nullptr;
	GDScriptELFFunction *implicit_initializer = nullptr;
	GDScriptELFFunction *implicit_ready = nullptr;
	GDScriptELFFunction *static_initializer = nullptr;

	HashSet<Object *> instances;
	bool destructing = false;
	bool clearing = false;

	//exported members
	String source;
	Vector<uint8_t> binary_tokens;
	String path;
	bool path_valid = false;
	StringName local_name;
	StringName global_name;
	String fully_qualified_name;
	String simplified_icon_path;
	SelfList<GDScriptELF> script_list;

	// Compilation state
	// Note: GDScriptParser and GDScriptAnalyzer don't inherit from RefCounted
	// Use raw pointers and manual memory management
	GDScriptParser *parser = nullptr;
	GDScriptAnalyzer *analyzer = nullptr;
	Error compilation_error = OK;

	Error _compile_to_elf();
	void _clear();

protected:
	bool _get(const StringName &p_name, Variant &r_ret) const;
	bool _set(const StringName &p_name, const Variant &p_value);
	void _get_property_list(List<PropertyInfo> *p_properties) const;
	Variant callp(const StringName &p_method, const Variant **p_args, int p_argcount, GDExtensionCallError &r_error);
	static void _bind_methods();

public:
	GDScriptELF();
	~GDScriptELF();

	// ScriptExtension methods - these may not all exist in GDExtension
	// Implement only what's available
	bool is_valid() const { return valid; }
	bool inherits_script(const Ref<Script> &p_script) const;
	bool is_tool() const { return tool; }
	bool is_abstract() const { return is_abstract_flag; }
	
	// ScriptExtension virtual methods
	virtual bool _is_abstract() const override { return is_abstract_flag; }
	Ref<Script> get_base_script() const;
	StringName get_global_name() const;
	StringName get_instance_base_type() const;
	ScriptInstanceExtension *instance_create(Object *p_this);
	// PlaceHolderScriptInstance doesn't exist in GDExtension
	// virtual PlaceHolderScriptInstance *placeholder_instance_create(Object *p_this) override;
	bool has_script_signal(const StringName &p_signal) const;
	void get_script_signal_list(List<MethodInfo> *r_signals) const;
	bool can_instantiate() const;
	// These methods are not virtual in ScriptExtension - implement as regular methods
	bool has_source_code() const;
	String get_source_code() const;
	void set_source_code(const String &p_code);
	Error reload(bool p_keep_state = false);
	bool has_method(const StringName &p_method) const;
	bool has_static_method(const StringName &p_method) const;
	Dictionary get_method_info(const StringName &p_method) const;
	void get_script_method_list(List<MethodInfo> *p_list) const;
	void get_script_property_list(List<PropertyInfo> *p_list) const;
	bool has_property_default_value(const StringName &p_property) const;
	Variant get_property_default_value(const StringName &p_property) const;
	void update_exports();
	Variant get_rpc_config() const;

	// GDScriptELF-specific
	_FORCE_INLINE_ const HashMap<StringName, GDScriptELFFunction *> &get_member_functions() const { return member_functions; }
	_FORCE_INLINE_ const HashMap<StringName, Variant> &get_constants() const { return constants; }
	_FORCE_INLINE_ const HashSet<StringName> &get_members() const { return members; }
	_FORCE_INLINE_ const GDScriptELFFunction *get_implicit_initializer() const { return implicit_initializer; }
	_FORCE_INLINE_ const GDScriptELFFunction *get_implicit_ready() const { return implicit_ready; }
	_FORCE_INLINE_ const GDScriptELFFunction *get_static_initializer() const { return static_initializer; }
	Ref<GDScriptELF> get_base() const;
	GDScriptELF *find_class(const String &p_qualified_name);
	bool has_class(const GDScriptELF *p_script) const;
	StringName get_local_name() const { return local_name; }
	String get_fully_qualified_name() const { return fully_qualified_name; }
	const HashMap<StringName, Ref<GDScriptELF>> &get_subclasses() const { return subclasses; }

	// ELF compilation
	PackedByteArray get_function_elf(const StringName &p_function_name) const;
	bool has_function_elf(const StringName &p_function_name) const;
};
