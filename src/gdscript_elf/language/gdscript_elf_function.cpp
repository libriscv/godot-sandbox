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

using gaddr_t = riscv::address_type<riscv::RISCV64>;

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

	if (elf_binary.is_empty()) {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
		ERR_PRINT("GDScriptELFFunction: No ELF binary available for function '" + String(name) + "'");
		return Variant();
	}

	Sandbox *sandbox = p_instance->sandbox;

	// Load ELF binary into sandbox if not already loaded
	if (!sandbox->has_program_loaded()) {
		sandbox->load_buffer(elf_binary);
		if (!sandbox->has_program_loaded()) {
			// Fallback to VM if loading fails
			ERR_PRINT("GDScriptELFFunction: Failed to load ELF binary, falling back to VM");
			return execute_vm_fallback(p_instance, p_args, p_argcount, r_error);
		}
	}

	// Generate function symbol name matching C code generation
	// The C code generator creates functions with prefix "gdscript_"
	String func_name = name.operator String();
	func_name = func_name.replace(".", "_").replace(" ", "_");
	String symbol_name = "gdscript_" + func_name;

	// Resolve function address
	gaddr_t func_address = sandbox->address_of(symbol_name);
	if (func_address == 0) {
		// Function symbol not found, try fallback to VM
		WARN_PRINT("GDScriptELFFunction: Function symbol '" + symbol_name + "' not found in ELF, falling back to VM");
		return execute_vm_fallback(p_instance, p_args, p_argcount, r_error);
	}

	// Prepare arguments for sandbox call
	// The function signature expects: [result_ptr, arg0, arg1, ..., argN, instance_ptr, constants_addr, operator_funcs_addr]
	// For now, we'll use a simplified approach with just the arguments
	// TODO: Implement full argument marshaling with result pointer and context
	
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

	// Call function in sandbox by address
	GDExtensionCallError vm_error;
	Variant result = sandbox->vmcall_address(func_address, final_args.ptr(), final_args.size(), vm_error);

	if (vm_error.error != GDEXTENSION_CALL_OK) {
		// Fallback to VM on error
		WARN_PRINT("GDScriptELFFunction: ELF execution failed for '" + String(name) + "', falling back to VM");
		return execute_vm_fallback(p_instance, p_args, p_argcount, r_error);
	}

	r_error.error = Callable::CallError::CALL_OK;
	return result;
}

Variant GDScriptELFFunction::execute_vm_fallback(GDScriptELFInstance *p_instance, const Variant **p_args, int p_argcount, Callable::CallError &r_error) {
	// Fallback to VM execution if ELF is not available
	// This uses the bytecode stored in the 'code' member
	
	if (code.is_empty()) {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
		ERR_PRINT("GDScriptELFFunction: No bytecode available for VM fallback");
		return Variant();
	}

	// For now, we'll use a simplified approach:
	// Create a temporary GDScriptFunction-like execution context
	// This is a minimal implementation that handles basic cases
	
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

	// Note: Full VM execution would require implementing the entire opcode interpreter
	// For now, we'll return an error indicating that VM fallback needs full implementation
	// In a production system, this would execute the bytecode using GDScriptVM
	WARN_PRINT("GDScriptELFFunction: VM fallback execution not fully implemented. Function '" + String(name) + "' requires full VM support.");
	
	r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
	return Variant();
}

