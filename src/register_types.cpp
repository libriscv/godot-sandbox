/* godot-cpp integration testing project.
 *
 * This is free and unencumbered software released into the public domain.
 */

#include "register_types.h"

#include <gdextension_interface.h>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "cpp/script_cpp.h"
#include "cpp/script_language_cpp.h"
#include "cpp/resource_loader_cpp.h"
#include "cpp/resource_saver_cpp.h"
#include "elf/script_elf.h"
#include "elf/resource_loader_elf.h"
#include "sandbox.hpp"

using namespace godot;

static Ref<ResourceFormatLoaderELF> elf_loader;
static Ref<ResourceFormatLoaderCPP> cpp_loader;
static Ref<ResourceFormatSaverCPP> cpp_saver;

void initialize_riscv_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	ClassDB::register_class<Sandbox>();
	GDREGISTER_CLASS(ELFScript);
	GDREGISTER_CLASS(CPPScript);
	GDREGISTER_CLASS(CPPScriptLanguage);
	GDREGISTER_CLASS(ResourceFormatLoaderELF);
	GDREGISTER_CLASS(ResourceFormatLoaderCPP);
	GDREGISTER_CLASS(ResourceFormatSaverCPP);
	elf_loader.instantiate();
	cpp_loader.instantiate();
	cpp_saver.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(elf_loader);
	ResourceLoader::get_singleton()->add_resource_format_loader(cpp_loader);
	ResourceSaver::get_singleton()->add_resource_format_saver(cpp_saver);
}

void uninitialize_riscv_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}

extern "C" {
// Initialization.
GDExtensionBool GDE_EXPORT riscv_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_riscv_module);
	init_obj.register_terminator(uninitialize_riscv_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
