#include "sandbox.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include "sandbox_project_settings.h"
#include <mutex>

#if defined(__linux__)
# include <dlfcn.h>
#elif defined(__MINGW32__) || defined(__MINGW64__) || defined(_MSC_VER)
# define YEP_IS_WINDOWS 1
# include <libriscv/win32/dlfcn.h>
# ifdef _MSC_VER
#  define access _access
#  define unlink _unlink
extern "C" int access(const char* path, int mode);
extern "C" int unlink(const char* path);
#  define R_OK   4       /* Test for read permission.  */
# else // _MSC_VER
#  include <unistd.h>
# endif
#elif defined(__APPLE__) && defined(__MACH__) // macOS OSX
# include <TargetConditionals.h>
# if TARGET_OS_MAC
#  include <dlfcn.h>
#  define YEP_IS_OSX 1
# endif
#endif
extern "C" void libriscv_register_translation8(...);

namespace {
String globalize_directory(const String &directory) {
	if (directory.is_empty())
		return String();
	String absolute = directory;
	if (directory.begins_with("res://") || directory.begins_with("user://"))
		absolute = ProjectSettings::get_singleton()->globalize_path(directory);
	return absolute.simplify_path().trim_suffix("/");
}
} // namespace

int64_t Sandbox::get_translation_hash() const {
#ifdef RISCV_BINARY_TRANSLATION
	if (m_machine == nullptr || machine().memory.binary().empty())
		return 0;
	const auto &main_segment = machine().memory.exec_segment_for(machine().memory.start_address());
	return main_segment->translation_hash();
#else
	return 0;
#endif
}

String Sandbox::shipped_translation_dir() {
#ifdef RISCV_BINARY_TRANSLATION
	const String shipped = OS::get_singleton()->get_executable_path().get_base_dir().path_join("bintr");
	if (DirAccess::dir_exists_absolute(shipped))
		return shipped;
#endif
	return String();
}

String Sandbox::binary_translation_cache_dir(bool create) {
#ifdef RISCV_BINARY_TRANSLATION
	static std::mutex cache_mutex;
	static String cached;
	std::lock_guard<std::mutex> lock(cache_mutex);
	if (!cached.is_empty() && DirAccess::dir_exists_absolute(cached))
		return cached;
	const String shipped = shipped_translation_dir();
	if (!shipped.is_empty()) {
		cached = shipped;
		return cached;
	}
	const String configured = globalize_directory(SandboxProjectSettings::binary_translation_cache_dir());
	if (configured.is_empty())
		return String();
	if (create && !DirAccess::dir_exists_absolute(configured))
		DirAccess::make_dir_recursive_absolute(configured);
	if (DirAccess::dir_exists_absolute(configured)) {
		cached = configured;
		return cached;
	}
#else
	(void)create;
#endif
	return String();
}

bool Sandbox::bintr_cache_opted_in() {
#ifdef RISCV_BINARY_TRANSLATION
	// Every machine that may take part in the AOT cache needs its own execute
	// segment: It's a deficiency in the execute segment key. To be fixed.
	return SandboxProjectSettings::binary_translation_enabled() ||
			!shipped_translation_dir().is_empty();
#else
	return false;
#endif
}

bool Sandbox::bintr_lookup_enabled() {
#ifdef RISCV_BINARY_TRANSLATION
	return bintr_cache_opted_in() && !binary_translation_cache_dir(false).is_empty();
#else
	return false;
#endif
}

bool Sandbox::load_binary_translation(const String &shared_library_path, bool allow_insecure) {
	if (m_global_instances_seen > 0 && !allow_insecure) {
		ERR_PRINT("Sandbox: Loading shared libraries after Sandbox instances have been created is a security risk."
			"Please load shared libraries before creating any Sandbox instances.");
		return false;
	}
#ifdef RISCV_BINARY_TRANSLATION
	// Load the shared library on platforms that support it
#  if defined(__linux__) || defined(YEP_IS_WINDOWS) || defined(YEP_IS_OSX)
	Ref<FileAccess> fa = FileAccess::open(shared_library_path, FileAccess::ModeFlags::READ);
	if (fa == nullptr || !fa->is_open()) {
		//ERR_PRINT("Sandbox: Failed to open shared library: " + shared_library_path);
		return false;
	}
	String path = fa->get_path_absolute();
	fa->close();
	void *handle = dlopen(path.utf8().ptr(), RTLD_LAZY);
	if (handle == nullptr) {
		ERR_PRINT("Sandbox: Failed to load shared library: " + shared_library_path);
		return false;
	}
	// If the shared library has a callback-based registration function, call it
	void *register_translation = dlsym(handle, "libriscv_init_with_callback8");
	if (register_translation == nullptr) {
		ERR_PRINT("Sandbox: The library is a hash-cache translation, not a self-registering translation.");
		dlclose(handle);
		return false;
	}
	using CallbackFunction = void (*)(void(*)(...));
	((CallbackFunction)register_translation)(libriscv_register_translation8);
#  else
	WARN_PRINT_ONCE("Sandbox: Loading shared libraries has not been implemented on this platform.");
#  endif
	// We don't need to do anything with the handle, as the shared library should self-register its functions
	return true;
#else
	WARN_PRINT_ONCE("Sandbox: Binary translation is not enabled.");
#endif
	return false;
}

bool Sandbox::is_binary_translated() const {
	// Get main execute segment
	auto &main_seg = this->m_machine->memory.exec_segment_for(this->m_machine->memory.start_address());
	return main_seg->is_binary_translated() || main_seg->is_asmjit_translated();
}

bool Sandbox::is_jit() const {
#if defined(RISCV_BINARY_TRANSLATION) || defined(RISCV_ASMJIT)
	auto &main_seg = this->m_machine->memory.exec_segment_for(this->m_machine->memory.start_address());
	// asmjit only ever produces JIT-compiled code, and a segment that is still
	// translating in the background is on its way to becoming JIT-compiled.
	return main_seg->is_asmjit_translated() || main_seg->is_libtcc()
		|| main_seg->is_background_compiling();
#else
	return false;
#endif
}
