/**************************************************************************/
/*  resource_loader_gdscript_elf.cpp                                     */
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

#include "resource_loader_gdscript_elf.h"
#include "gdscript_elf.h"
#include <godot_cpp/classes/file_access.hpp>

using namespace godot;

Variant ResourceFormatLoaderGDScriptELF::_load(const String &path, const String &original_path, bool use_sub_threads, int32_t cache_mode) const {
	Ref<GDScriptELF> script = memnew(GDScriptELF);
	script->set_path(path);

	// Load source code
	Error err = script->reload();
	if (err != OK) {
		return Variant(); // Return invalid Variant on error
	}

	return script;
}

PackedStringArray ResourceFormatLoaderGDScriptELF::_get_recognized_extensions() const {
	PackedStringArray array;
	array.push_back("gde");
	return array;
}

bool ResourceFormatLoaderGDScriptELF::_recognize_path(const String &path, const StringName &type) const {
	String el = path.get_extension().to_lower();
	if (el == "gde") {
		return true;
	}
	return false;
}

bool ResourceFormatLoaderGDScriptELF::_handles_type(const StringName &type) const {
	String type_str = type;
	return type_str == "GDScriptELF" || type_str == "Script";
}

String ResourceFormatLoaderGDScriptELF::_get_resource_type(const String &p_path) const {
	String el = p_path.get_extension().to_lower();
	if (el == "gde") {
		return "GDScriptELF";
	}
	return "";
}

