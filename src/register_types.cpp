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
#include "rust/resource_loader_rust.h"
#include "rust/resource_saver_rust.h"
#include "rust/script_language_rust.h"
#include "rust/script_rust.h"
#include "sandbox.h"
#include "sandbox_project_settings.h"

using namespace godot;

static Ref<ResourceFormatLoaderELF> elf_loader;
static Ref<ResourceFormatSaverELF> elf_saver;

static ELFScriptLanguage *elf_language;

ScriptLanguage *get_elf_language() {
	return elf_language;
}

static void initialize_riscv_module(ModuleInitializationLevel p_level) {
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
	ClassDB::register_class<RustScript>();
	ClassDB::register_class<ResourceFormatLoaderRust>();
	ClassDB::register_class<ResourceFormatSaverRust>();
	ClassDB::register_class<RustScriptLanguage>();
	elf_loader.instantiate();
	elf_saver.instantiate();
	elf_language = memnew(ELFScriptLanguage);
	Engine::get_singleton()->register_script_language(elf_language);
	ResourceLoader::get_singleton()->add_resource_format_loader(elf_loader);
	ResourceSaver::get_singleton()->add_resource_format_saver(elf_saver);
	CPPScriptLanguage::init();
	ResourceFormatLoaderCPP::init();
	ResourceFormatSaverCPP::init();
	RustScriptLanguage::init();
	ResourceFormatLoaderRust::init();
	ResourceFormatSaverRust::init();
	SandboxProjectSettings::register_settings();
}

static void uninitialize_riscv_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	//Engine::get_singleton()->unregister_script_language(cpp_language);
	//Engine::get_singleton()->unregister_script_language(elf_language);
	ResourceLoader::get_singleton()->remove_resource_format_loader(elf_loader);
	ResourceSaver::get_singleton()->remove_resource_format_saver(elf_saver);
	//delete elf_language;
	elf_loader.unref();
	elf_saver.unref();
	ResourceFormatLoaderCPP::deinit();
	ResourceFormatSaverCPP::deinit();
	ResourceFormatLoaderRust::deinit();
	ResourceFormatSaverRust::deinit();
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
