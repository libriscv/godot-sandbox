#include "register_types.h"
#include "core/object/object.h"
#include "core/extension/gdextension_interface.h"
#include "core/extension/gdextension_manager.h"
#include "./gdextension_static_library_loader.h"
#include "core/object/ref_counted.h"
#include "src/register_types.h"

void initialize_sandbox_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}

	Ref<GDExtensionStaticLibraryLoader> loader;
	loader.instantiate();
	loader->set_entry_funcptr((void*)&riscv_library_init);
	GDExtensionManager::get_singleton()->load_extension_with_loader("sandbox", loader);
}

void uninitialize_sandbox_module(ModuleInitializationLevel p_level) {
}
