/**************************************************************************/
/*  gdscript_elf_function.cpp                                            */
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

#include "gdscript_elf_function.h"
#include "gdscript_elf_instance.h"
#include "gdscript_elf.h"
#include "../../sandbox.h"
#include "core/variant/variant.h"
#include "core/variant/callable.h"

GDScriptELFFunction::GDScriptELFFunction() {
	script = nullptr;
	argument_count = 0;
	default_argument_count = 0;
	is_static = false;
	is_vararg = false;
	has_yield = false;
	line = -1;
	elf_compiled = false;
}

GDScriptELFFunction::~GDScriptELFFunction() {
	// Cleanup
}

Variant GDScriptELFFunction::call(GDScriptELFInstance *p_instance, const Variant **p_args, int p_argcount, Callable::CallError &r_error, CallState *p_state) {
	// Validate argument count
	if (p_argcount < argument_count - default_argument_count) {
		r_error.error = Callable::CallError::CALL_ERROR_TOO_FEW_ARGUMENTS;
		r_error.argument = argument_count - default_argument_count;
		return Variant();
	}

	if (!is_vararg && p_argcount > argument_count) {
		r_error.error = Callable::CallError::CALL_ERROR_TOO_MANY_ARGUMENTS;
		r_error.argument = argument_count;
		return Variant();
	}

	// Fill in default arguments if needed
	Vector<const Variant *> final_args;
	final_args.resize(argument_count);
	int arg_index = 0;
	for (; arg_index < p_argcount && arg_index < argument_count; arg_index++) {
		final_args[arg_index] = p_args[arg_index];
	}
	for (; arg_index < argument_count; arg_index++) {
		int default_index = arg_index - (argument_count - default_argument_count);
		if (default_index >= 0 && default_index < default_arguments.size()) {
			final_args[arg_index] = &default_arguments[default_index];
		} else {
			r_error.error = Callable::CallError::CALL_ERROR_TOO_FEW_ARGUMENTS;
			r_error.argument = arg_index;
			return Variant();
		}
	}

	// Try ELF execution first, fallback to VM if not available
	if (has_elf_code()) {
		return execute_elf(p_instance, final_args.ptr(), final_args.size(), r_error);
	} else {
		return execute_vm_fallback(p_instance, final_args.ptr(), final_args.size(), r_error);
	}
}

Variant GDScriptELFFunction::execute_elf(GDScriptELFInstance *p_instance, const Variant **p_args, int p_argcount, Callable::CallError &r_error) {
	if (!p_instance || !p_instance->sandbox) {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
		return Variant();
	}

	Sandbox *sandbox = p_instance->sandbox;

	// Load ELF binary into sandbox if not already loaded
	// For now, we'll assume the ELF binary is already loaded
	// In a full implementation, we'd need to:
	// 1. Load the ELF binary into the sandbox
	// 2. Resolve the function address
	// 3. Call the function with marshaled arguments

	// Prepare arguments for VM call
	Array vm_args;
	for (int i = 0; i < p_argcount; i++) {
		vm_args.push_back(*p_args[i]);
	}

	// Call function in sandbox
	GDExtensionCallError vm_error;
	Variant result = sandbox->vmcall((const Variant **)vm_args.ptr(), vm_args.size(), vm_error);

	if (vm_error.error != GDEXTENSION_CALL_OK) {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
		return Variant();
	}

	r_error.error = Callable::CallError::CALL_OK;
	return result;
}

Variant GDScriptELFFunction::execute_vm_fallback(GDScriptELFInstance *p_instance, const Variant **p_args, int p_argcount, Callable::CallError &r_error) {
	// Fallback to VM execution if ELF is not available
	// This would use the bytecode stored in the 'code' member
	// For now, return an error indicating VM fallback is not implemented
	r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
	ERR_PRINT("GDScriptELFFunction: VM fallback not yet implemented");
	return Variant();
}

