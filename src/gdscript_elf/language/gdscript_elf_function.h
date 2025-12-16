/**************************************************************************/
/*  gdscript_elf_function.h                                               */
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

#include "../compilation/gdscript_function.h"
#include "../compilation/gdscript.h"
#include <gdextension_interface.h>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/templates/self_list.hpp>
#include <godot_cpp/variant/variant.hpp>

using namespace godot;

class GDScriptELFInstance;
class GDScriptELF;
class Sandbox;

// GDScriptELFFunction - Function that executes ELF binaries instead of VM bytecode
// Similar to GDScriptFunction but uses sandbox execution
class GDScriptELFFunction : public RefCounted {
	GDCLASS(GDScriptELFFunction, RefCounted);

	friend class GDScriptELF;
	friend class GDScriptELFInstance;
	friend class GDScriptELFCompiler;

	StringName name;
	GDScriptELF *script = nullptr;
	int argument_count = 0;
	int default_argument_count = 0;
	Vector<Variant> default_arguments;
	Vector<GDScriptDataType> argument_types;
	GDScriptDataType return_type;
	bool is_static = false;
	bool is_vararg = false;
	bool has_yield = false;
	int line = -1;

	// ELF-specific: Compiled ELF binary for this function
	PackedByteArray elf_binary;
	bool elf_compiled = false;

	// Original VM bytecode (for fallback if ELF compilation fails)
	Vector<int> code;
	Vector<Variant> constants;
	// LocalStack doesn't exist in GDExtension - use Vector<Variant> for stack
	Vector<Variant> stack;

	SelfList<GDScriptELFFunction> function_list;

public:
	struct CallState {
		Variant *stack = nullptr;
		int *ip = nullptr;
		int *line = nullptr;
		ObjectID instance_id;
		Variant self;
		Variant *result = nullptr;
	};

	GDScriptELFFunction();
	~GDScriptELFFunction();

	// Main call method - executes ELF binary in sandbox
	Variant call(GDScriptELFInstance *p_instance, const Variant **p_args, int p_argcount, GDExtensionCallError &r_error, CallState *p_state = nullptr);

	// Check if ELF compilation is available
	bool has_elf_code() const { return elf_compiled && !elf_binary.is_empty(); }

	// Get/set ELF binary
	void set_elf_binary(const PackedByteArray &p_elf) { elf_binary = p_elf; elf_compiled = !p_elf.is_empty(); }
	PackedByteArray get_elf_binary() const { return elf_binary; }

	// Function metadata
	_FORCE_INLINE_ StringName get_name() const { return name; }
	_FORCE_INLINE_ int get_argument_count() const { return argument_count; }
	_FORCE_INLINE_ int get_default_argument_count() const { return default_argument_count; }
	_FORCE_INLINE_ bool get_is_static() const { return is_static; }
	_FORCE_INLINE_ bool get_is_vararg() const { return is_vararg; }
	_FORCE_INLINE_ int get_line() const { return line; }
	_FORCE_INLINE_ GDScriptDataType get_return_type() const { return return_type; }
	_FORCE_INLINE_ const Vector<GDScriptDataType> &get_argument_types() const { return argument_types; }

private:
	// Execute ELF binary in sandbox
	Variant execute_elf(GDScriptELFInstance *p_instance, const Variant **p_args, int p_argcount, GDExtensionCallError &r_error);

	// Fallback to VM execution if ELF is not available
	Variant execute_vm_fallback(GDScriptELFInstance *p_instance, const Variant **p_args, int p_argcount, GDExtensionCallError &r_error);
};
