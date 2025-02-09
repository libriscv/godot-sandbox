#include "register_types.h"
#include "core/object/object.h"
#include "core/extension/gdextension_interface.h"
#include "core/extension/gdextension_manager.h"
#include "core/extension/./gdextension_static_library_loader.h"
#include "core/object/ref_counted.h"

Ref<GDExtensionStaticLibraryLoader> loader;

extern "C" {
    GDExtensionBool riscv_library_init(
        GDExtensionInterfaceGetProcAddress p_get_proc_address,
        GDExtensionClassLibraryPtr p_library,
        GDExtensionInitialization *r_initialization
    );
}

void initialize_sandbox_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	loader.instantiate();
	loader->set_entry_funcptr((void*)&riscv_library_init);
	GDExtensionManager::get_singleton()->load_extension_with_loader("riscv_library", loader);
}

void uninitialize_sandbox_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	ERR_PRINT("UNINIT SANDBOX");
	GDExtensionManager::get_singleton()->unload_extension("riscv_library");
	loader.unref();
}
