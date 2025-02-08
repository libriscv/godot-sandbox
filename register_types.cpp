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
	ERR_PRINT("INIT SANDBOX");
	GDExtensionManager::get_singleton()->load_extension_with_loader("sandbox", loader);
}

void uninitialize_sandbox_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDExtensionManager::get_singleton()->unload_extension("sandbox");
	loader.unref();
}
