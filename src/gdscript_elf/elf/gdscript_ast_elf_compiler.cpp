/**************************************************************************/
/*  gdscript_ast_elf_compiler.cpp                                        */
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

#include "gdscript_ast_elf_compiler.h"

#include "gdscript_ast_c_codegen.h"
#include "gdscript_c_compiler.h"
#include "../compilation/gdscript_parser.h"
#include "../compilation/gdscript_analyzer.h"

#include <godot_cpp/core/error_macros.hpp>

using namespace godot;

String GDScriptASTELFCompiler::last_compilation_error;

PackedByteArray GDScriptASTELFCompiler::compile_function_to_elf(const GDScriptParser::FunctionNode *p_function, const GDScriptParser::ClassNode *p_class, GDScriptAnalyzer *p_analyzer) {
	last_compilation_error.clear();

	if (!p_function || !p_class || !p_analyzer) {
		last_compilation_error = "Invalid parameters";
		return PackedByteArray();
	}

	if (!can_compile_function(p_function)) {
		return PackedByteArray();
	}

	PackedByteArray elf_output;
	Error err = compile_internal(p_function, p_class, p_analyzer, elf_output);

	if (err != OK) {
		return PackedByteArray();
	}

	return elf_output;
}

bool GDScriptASTELFCompiler::can_compile_function(const GDScriptParser::FunctionNode *p_function) {
	if (!p_function) {
		return false;
	}

	if (!is_compiler_available()) {
		last_compilation_error = "RISC-V cross-compiler not available";
		return false;
	}

	// Check if function has a body
	if (!p_function->body) {
		last_compilation_error = "Function has no body";
		return false;
	}

	return true;
}

bool GDScriptASTELFCompiler::is_compiler_available() {
	return !GDScriptCCompiler::detect_cross_compiler().is_empty();
}

String GDScriptASTELFCompiler::get_last_error() {
	return last_compilation_error;
}

Error GDScriptASTELFCompiler::compile_internal(const GDScriptParser::FunctionNode *p_function, const GDScriptParser::ClassNode *p_class, GDScriptAnalyzer *p_analyzer, PackedByteArray &r_elf_output) {
	// Generate C code from AST
	GDScriptASTCCodeGenerator codegen;
	String c_code = codegen.generate_c_code(p_function, p_class, p_analyzer);

	if (c_code.is_empty()) {
		last_compilation_error = "Failed to generate C code: " + codegen.get_error();
		return ERR_COMPILATION_FAILED;
	}

	// Compile C code to ELF
	GDScriptCCompiler compiler;
	r_elf_output = compiler.compile_to_elf(c_code);

	if (r_elf_output.is_empty()) {
		last_compilation_error = "Failed to compile C code to ELF: " + compiler.get_last_error();
		return ERR_COMPILATION_FAILED;
	}

	return OK;
}
