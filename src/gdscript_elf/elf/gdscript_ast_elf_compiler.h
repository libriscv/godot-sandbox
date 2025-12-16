/**************************************************************************/
/*  gdscript_ast_elf_compiler.h                                           */
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

#include "gdscript_ast_c_codegen.h"
#include "gdscript_c_compiler.h"

#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/variant.hpp>
#include "../compilation/gdscript_parser.h"

using namespace godot;

// Forward declaration
class GDScriptAnalyzer;

// Orchestrates AST-to-ELF compilation via direct C code generation
// This bypasses bytecode entirely, avoiding VM-specific types
class GDScriptASTELFCompiler {
public:
	// Compile a function from AST to RISC-V ELF
	// Returns the ELF binary as a PackedByteArray
	// Returns empty PackedByteArray on error
	static PackedByteArray compile_function_to_elf(const GDScriptParser::FunctionNode *p_function, const GDScriptParser::ClassNode *p_class, GDScriptAnalyzer *p_analyzer);

	// Check if a function can be compiled to ELF
	// Returns true if cross-compiler is available and function is valid
	static bool can_compile_function(const GDScriptParser::FunctionNode *p_function);

	// Check if cross-compiler is available
	static bool is_compiler_available();

	// Get last compilation error (if any)
	static String get_last_error();

private:
	static String last_compilation_error;

	// Main compilation logic
	static Error compile_internal(const GDScriptParser::FunctionNode *p_function, const GDScriptParser::ClassNode *p_class, GDScriptAnalyzer *p_analyzer, PackedByteArray &r_elf_output);
};
