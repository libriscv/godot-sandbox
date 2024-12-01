#include "sandbox.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

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
	if (register_translation != nullptr) {
		using CallbackFunction = void (*)(void(*)(...));
		((CallbackFunction)register_translation)(libriscv_register_translation8);
	}
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

bool Sandbox::try_compile_binary_translation(String shared_library_path, const String &cc, const String &extra_cflags, bool ignore_instruction_limit, bool automatic_nbit_as) {
	if (this->is_binary_translated()) {
		return true;
	}
	if (this->is_in_vmcall()) {
		ERR_PRINT("Sandbox: Cannot produce binary translation while in a VM call. This is a security risk.");
		return false;
	}
	if (this->get_restrictions()) {
		ERR_PRINT("Sandbox: Cannot produce binary translation while restrictions are enabled.");
		return false;
	}
	if (shared_library_path.is_empty()) {
		ERR_PRINT("Sandbox: No shared library path specified.");
		return false;
	}
	if (!shared_library_path.begins_with("res://")) {
		ERR_PRINT("Sandbox: Shared library path must begin with 'res://'.");
		return false;
	}
#ifdef __linux__
	shared_library_path += ".so";
#elif defined(YEP_IS_WINDOWS)
	shared_library_path += ".dll";
#elif defined(YEP_IS_OSX)
	shared_library_path += ".dylib";
#else
	WARN_PRINT_ONCE("Sandbox: Compiling binary translations has not been implemented on this platform.");
	return false;
#endif
	const String code = this->emit_binary_translation(ignore_instruction_limit, automatic_nbit_as);
	if (code.is_empty()) {
		ERR_PRINT("Sandbox: Failed to emit binary translation.");
		return false;
	}
	static const String c99_path = "res://temp_sandbox_generated.c";
	Ref<FileAccess> fa = FileAccess::open(c99_path, FileAccess::ModeFlags::WRITE);
	if (!fa->is_open()) {
		ERR_PRINT("Sandbox: Failed to open file for writing: " + c99_path);
		return false;
	}
	fa->store_string(code);
	fa->close();
	// Compile the generated code
	Array args;
#if defined(__linux__) || defined(YEP_IS_MACOSX)
	args.push_back("-shared");
	args.push_back("-fPIC");
	args.push_back("-fvisibility=hidden");
	args.push_back("-O2");
	args.push_back("-w");
	args.push_back("-DCALLBACK_INIT");
	args.push_back("-o");
#elif defined(YEP_IS_WINDOWS)
	args.push_back("/LD");
	args.push_back("/O2");
	args.push_back("/w");
	args.push_back("/DCALLBACK_INIT");
	args.push_back("/Fe");
#endif
	args.push_back(shared_library_path.replace("res://", ""));
	if (!extra_cflags.is_empty())
		args.append_array(extra_cflags.split(" "));
	args.push_back(c99_path.replace("res://", ""));
	UtilityFunctions::print(cc, args);
	Array output;
	int ret = OS::get_singleton()->execute(cc, args, output, true);
	// Remove the generated C99 file
	Ref<DirAccess> dir = DirAccess::open("res://");
	dir->remove(c99_path);
	if (ret != 0) {
		ERR_PRINT("Sandbox: Failed to compile generated code: " + c99_path);
		UtilityFunctions::print(output);
		return false;
	}
	return true;
}

bool Sandbox::is_binary_translated() const {
	return this->m_machine->is_binary_translation_enabled();
}
