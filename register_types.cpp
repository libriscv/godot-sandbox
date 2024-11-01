#include "register_types.h"

#include <godot_cpp/classes/script_language.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "src/cpp/resource_loader_cpp.h"
#include "src/cpp/resource_saver_cpp.h"
#include "src/cpp/script_cpp.h"
#include "src/cpp/script_language_cpp.h"
#include "src/elf/resource_loader_elf.h"
#include "src/elf/resource_saver_elf.h"
#include "src/elf/script_elf.h"
#include "src/elf/script_language_elf.h"
#include "src/rust/resource_loader_rust.h"
#include "src/rust/resource_saver_rust.h"
#include "src/rust/script_language_rust.h"
#include "src/rust/script_rust.h"
#include "src/sandbox.h"
#include "src/sandbox_project_settings.h"
#include "src/zig/resource_loader_zig.h"
#include "src/zig/resource_saver_zig.h"
#include "src/zig/script_language_zig.h"
#include "src/zig/script_zig.h"
GODOT_NAMESPACE

static Ref<ResourceFormatLoaderELF> elf_loader;
static Ref<ResourceFormatSaverELF> elf_saver;

static ELFScriptLanguage *elf_language;

ScriptLanguage *get_elf_language() {
	return elf_language;
}

extern "C" {
static void initialize_sandbox_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	ClassDB::register_class<Sandbox>();
	ClassDB::register_class<ELFScript>();
	ClassDB::register_class<ELFScriptLanguage>();
	ClassDB::register_class<ResourceFormatLoaderELF>();
	ClassDB::register_class<ResourceFormatSaverELF>();
	ClassDB::register_class<CPPScript>();
	ClassDB::register_class<CPPScriptLanguage>();
	ClassDB::register_class<ResourceFormatLoaderCPP>();
	ClassDB::register_class<ResourceFormatSaverCPP>();
	ClassDB::register_class<RustScript>();
	ClassDB::register_class<RustScriptLanguage>();
	ClassDB::register_class<ResourceFormatLoaderRust>();
	ClassDB::register_class<ResourceFormatSaverRust>();
	ClassDB::register_class<ZigScript>();
	ClassDB::register_class<ZigScriptLanguage>();
	ClassDB::register_class<ResourceFormatLoaderZig>();
	ClassDB::register_class<ResourceFormatSaverZig>();
	elf_loader.instantiate();
	elf_saver.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(elf_loader, true);
	ResourceSaver::get_singleton()->add_resource_format_saver(elf_saver);
	elf_language = memnew(ELFScriptLanguage);
	Engine::get_singleton()->register_script_language(elf_language);
	CPPScriptLanguage::init();
	ResourceFormatLoaderCPP::init();
	ResourceFormatSaverCPP::init();
	RustScriptLanguage::init();
	ResourceFormatLoaderRust::init();
	ResourceFormatSaverRust::init();
	ZigScriptLanguage::init();
	ResourceFormatLoaderZig::init();
	ResourceFormatSaverZig::init();
	SandboxProjectSettings::register_settings();
}

static void uninitialize_sandbox_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	Engine *engine = Engine::get_singleton();
	engine->unregister_script_language(CPPScriptLanguage::get_singleton());
	engine->unregister_script_language(RustScriptLanguage::get_singleton());
	engine->unregister_script_language(ZigScriptLanguage::get_singleton());
	if (elf_language) {
		engine->unregister_script_language(elf_language);
		memdelete(elf_language);
		elf_language = nullptr;
	}

	ResourceLoader::get_singleton()->remove_resource_format_loader(elf_loader);
	ResourceSaver::get_singleton()->remove_resource_format_saver(elf_saver);
	elf_loader.unref();
	elf_saver.unref();
	ResourceFormatLoaderCPP::deinit();
	ResourceFormatSaverCPP::deinit();
	ResourceFormatLoaderRust::deinit();
	ResourceFormatSaverRust::deinit();
	ResourceFormatLoaderZig::deinit();
	ResourceFormatSaverZig::deinit();
}
// Initialization.
GDExtensionBool GDE_EXPORT riscv_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_sandbox_module);
	init_obj.register_terminator(uninitialize_sandbox_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
