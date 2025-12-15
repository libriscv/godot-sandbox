/**************************************************************************/
/*  register_gdscript_elf.cpp                                            */
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

#include "register_gdscript_elf.h"
#include "gdscript_elf_language.h"
#include "gdscript_elf.h"
#include "resource_loader_gdscript_elf.h"
#include "resource_saver_gdscript_elf.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>

using namespace godot;

static GDScriptELFLanguage *script_language_gdelf = nullptr;
static Ref<ResourceFormatLoaderGDScriptELF> resource_loader_gdelf;
static Ref<ResourceFormatSaverGDScriptELF> resource_saver_gdelf;

void initialize_gdscript_elf_language(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	// Register classes
	GDREGISTER_CLASS(GDScriptELF);
	GDREGISTER_CLASS(GDScriptELFLanguage);
	GDREGISTER_CLASS(ResourceFormatLoaderGDScriptELF);
	GDREGISTER_CLASS(ResourceFormatSaverGDScriptELF);

	// Create and register language
	script_language_gdelf = memnew(GDScriptELFLanguage);
	ScriptServer::register_language(script_language_gdelf);

	// Create and register resource loaders/savers
	resource_loader_gdelf.instantiate();
	ResourceLoader::add_resource_format_loader(resource_loader_gdelf);

	resource_saver_gdelf.instantiate();
	ResourceSaver::add_resource_format_saver(resource_saver_gdelf);

	// Initialize language
	script_language_gdelf->init();
}

void uninitialize_gdscript_elf_language(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	// Unregister language
	if (script_language_gdelf) {
		ScriptServer::unregister_language(script_language_gdelf);
		script_language_gdelf->finish();
		memdelete(script_language_gdelf);
		script_language_gdelf = nullptr;
	}

	// Remove resource loaders/savers
	if (resource_loader_gdelf.is_valid()) {
		ResourceLoader::remove_resource_format_loader(resource_loader_gdelf);
		resource_loader_gdelf.unref();
	}

	if (resource_saver_gdelf.is_valid()) {
		ResourceSaver::remove_resource_format_saver(resource_saver_gdelf);
		resource_saver_gdelf.unref();
	}
}

extern "C" {
GDExtensionBool GDE_EXPORT gdscript_elf_extension_init(
	GDExtensionInterfaceGetProcAddress p_get_proc_address,
	GDExtensionClassLibraryPtr p_library,
	GDExtensionInitialization *r_initialization
) {
	// Initialize GDExtension interface
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	// Register initialization and termination functions
	init_obj.register_initializer(initialize_gdscript_elf_language);
	init_obj.register_terminator(uninitialize_gdscript_elf_language);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
