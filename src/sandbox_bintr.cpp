#include "sandbox.h"

#include <godot_cpp/classes/file_access.hpp>

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

String Sandbox::emit_binary_translation(bool ignore_instruction_limit, bool automatic_nbit_as) const {
	const std::string_view &binary = machine().memory.binary();
	if (binary.empty()) {
		ERR_PRINT("Sandbox: No binary loaded.");
		return String();
	}
#ifdef RISCV_BINARY_TRANSLATION
	// 1. Re-create the same options
	auto options = std::make_shared<riscv::MachineOptions<RISCV_ARCH>>(machine().options());
	options->use_shared_execute_segments = false;
	options->translate_enabled = false;
	options->translate_enable_embedded = true;
	options->translate_invoke_compiler = false;
	options->translate_ignore_instruction_limit = ignore_instruction_limit;
	options->translate_automatic_nbit_address_space = automatic_nbit_as;

	// 2. Enable binary translation output to a string
	std::string code_output;
	options->cross_compile.push_back(riscv::MachineTranslationEmbeddableCodeOptions{
			.result_c99 = &code_output,
	});

	// 3. Emit the binary translation by constructing a new machine
	machine_t m{ binary, *options };

	// 4. Verify that the translation was successful
	if (code_output.empty()) {
		ERR_PRINT("Sandbox: Binary translation failed.");
		return String();
	}
	// 5. Return the translated code
	return String::utf8(code_output.c_str(), code_output.size());
#else
	ERR_PRINT("Sandbox: Binary translation is not enabled.");
	return String();
#endif
}

bool Sandbox::load_binary_translation(const String &shared_library_path) {
	if (m_global_instances_seen > 0) {
		ERR_PRINT("Sandbox: Loading shared libraries after Sandbox instances have been created is a security risk."
			"Please load shared libraries before creating any Sandbox instances.");
		return false;
	}
#ifdef RISCV_BINARY_TRANSLATION
	// Load the shared library on platforms that support it
#  if defined(__linux__) || defined(YEP_IS_WINDOWS) || defined(YEP_IS_OSX)
	Ref<FileAccess> fa = FileAccess::open(shared_library_path, FileAccess::ModeFlags::READ);
	if (!fa->is_open()) {
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
	if (register_translation != nullptr) {
		using CallbackFunction = void (*)(void(*)(...));
		((CallbackFunction)register_translation)(libriscv_register_translation8);
	}
#  else
	ERR_PRINT("Sandbox: Loading shared libraries has not been implemented on this platform.");
#  endif
	// We don't need to do anything with the handle, as the shared library should self-register its functions
	return true;
#else
	ERR_PRINT("Sandbox: Binary translation is not enabled.");
#endif
	return false;
}

bool Sandbox::is_binary_translated() const {
	return this->m_machine->is_binary_translation_enabled();
}