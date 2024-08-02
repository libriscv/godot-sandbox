/* godot-cpp integration testing project.
 *
 * This is free and unencumbered software released into the public domain.
 */

#include "register_types.h"

#include <gdextension_interface.h>

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "cpp/resource_loader_cpp.h"
#include "cpp/resource_saver_cpp.h"
#include "cpp/script_cpp.h"
#include "cpp/script_language_cpp.h"
#include "elf/resource_loader_elf.h"
#include "elf/resource_saver_elf.h"
#include "elf/script_elf.h"
#include "elf/script_language_elf.h"
#include "sandbox.hpp"

using namespace godot;

static Ref<ResourceFormatLoaderELF> elf_loader;
static Ref<ResourceFormatSaverELF> elf_saver;

static Ref<ResourceFormatLoaderCPP> cpp_loader;
static Ref<ResourceFormatSaverCPP> cpp_saver;
static CPPScriptLanguage *cpp_language;
static ELFScriptLanguage *elf_language;

ScriptLanguage *get_cpp_language() {
	return cpp_language;
}

ScriptLanguage *get_elf_language() {
	return elf_language;
}

void initialize_riscv_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	ClassDB::register_class<Sandbox>();
	ClassDB::register_class<ELFScript>();
	ClassDB::register_class<ResourceFormatLoaderELF>();
	ClassDB::register_class<ResourceFormatSaverELF>();
	ClassDB::register_class<ELFScriptLanguage>();
	ClassDB::register_class<CPPScript>();
	ClassDB::register_class<ResourceFormatLoaderCPP>();
	ClassDB::register_class<ResourceFormatSaverCPP>();
	ClassDB::register_class<CPPScriptLanguage>();
	elf_loader.instantiate();
	elf_saver.instantiate();
	elf_language = memnew(ELFScriptLanguage);
	cpp_loader.instantiate();
	cpp_saver.instantiate();
	cpp_language = memnew(CPPScriptLanguage);
	Engine::get_singleton()->register_script_language(cpp_language);
	Engine::get_singleton()->register_script_language(elf_language);
	ResourceLoader::get_singleton()->add_resource_format_loader(elf_loader);
	ResourceLoader::get_singleton()->add_resource_format_loader(cpp_loader);
	ResourceSaver::get_singleton()->add_resource_format_saver(elf_saver);
	ResourceSaver::get_singleton()->add_resource_format_saver(cpp_saver);
}

void uninitialize_riscv_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	ResourceLoader::get_singleton()->remove_resource_format_loader(elf_loader);
	ResourceSaver::get_singleton()->remove_resource_format_saver(elf_saver);
	ResourceLoader::get_singleton()->remove_resource_format_loader(cpp_loader);
	ResourceSaver::get_singleton()->remove_resource_format_saver(cpp_saver);
	elf_loader.unref();
	elf_saver.unref();
	cpp_loader.unref();
	cpp_saver.unref();
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
