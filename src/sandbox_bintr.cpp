#include "sandbox.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include "sandbox_project_settings.h"
#include <atomic>
#include <mutex>
#include <unordered_set>

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
#if defined(__linux__)
static constexpr const char *BINTR_SUFFIX = ".so";
#elif defined(YEP_IS_WINDOWS)
static constexpr const char *BINTR_SUFFIX = ".dll";
#elif defined(YEP_IS_OSX)
static constexpr const char *BINTR_SUFFIX = ".dylib";
#else
static constexpr const char *BINTR_SUFFIX = "";
#endif

std::mutex bake_mutex;
std::mutex bake_compiler_mutex;
std::unordered_set<uint32_t> bakes_in_progress;
std::atomic<bool> auto_bake_compiler_failed { false };

String hash_string(uint32_t hash) {
	// String::pad_zeros() treats a leading A-F like a sign and inserts the zero
	// after it (E1304A45 -> E01304A45). The loader contract is exactly eight
	// hexadecimal digits, as emitted by libriscv's %08X filename.
	return vformat("%08X", hash);
}

String globalize_directory(const String &directory) {
	if (directory.is_empty())
		return String();
	String absolute = directory;
	if (directory.begins_with("res://") || directory.begins_with("user://"))
		absolute = ProjectSettings::get_singleton()->globalize_path(directory);
	return absolute.simplify_path().trim_suffix("/");
}

struct BakeGuard {
	uint32_t hash;
	bool owns = false;
	explicit BakeGuard(uint32_t p_hash) : hash(p_hash) {
		std::lock_guard<std::mutex> lock(bake_mutex);
		owns = bakes_in_progress.insert(hash).second;
	}
	~BakeGuard() {
		if (owns) {
			std::lock_guard<std::mutex> lock(bake_mutex);
			bakes_in_progress.erase(hash);
		}
	}
};

String compile_shared_translation(const std::string &source, uint32_t hash,
		const String &output_dir, const String &cc, const String &extra_cflags, bool quiet) {
#if defined(__ANDROID__) || defined(__wasm__) || defined(__SWITCH__) || defined(__EMSCRIPTEN__)
	(void)source; (void)hash; (void)output_dir; (void)cc; (void)extra_cflags; (void)quiet;
	return String();
#elif defined(__APPLE__) && !defined(__MACH__)
	(void)source; (void)hash; (void)output_dir; (void)cc; (void)extra_cflags; (void)quiet;
	return String();
#else
	if (source.empty() || output_dir.is_empty() || cc.is_empty() || BINTR_SUFFIX[0] == '\0')
		return String();
	if (quiet && auto_bake_compiler_failed.load())
		return String();
	if (DirAccess::make_dir_recursive_absolute(output_dir) != OK &&
			!DirAccess::dir_exists_absolute(output_dir)) {
		if (!quiet) ERR_PRINT("Sandbox: Failed to create binary translation directory: " + output_dir);
		return String();
	}
	const String final_path = output_dir.path_join("bintr-" + hash_string(hash) + BINTR_SUFFIX);
	if (FileAccess::file_exists(final_path))
		return final_path;

	BakeGuard guard(hash);
	if (!guard.owns)
		return String();
	std::lock_guard<std::mutex> compiler_lock(bake_compiler_mutex);
	if (FileAccess::file_exists(final_path))
		return final_path;

	const String nonce = itos(OS::get_singleton()->get_process_id()) + "-" +
			itos(Time::get_singleton()->get_ticks_usec());
	const String temp_base = output_dir.path_join(".bintr-" + hash_string(hash) + "-" + nonce);
	const String c99_path = temp_base + String(".c");
	const String output_path = temp_base + String(BINTR_SUFFIX);
	Ref<FileAccess> source_file = FileAccess::open(c99_path, FileAccess::ModeFlags::WRITE);
	if (source_file.is_null() || !source_file->is_open()) {
		if (!quiet) ERR_PRINT("Sandbox: Failed to write generated translation: " + c99_path);
		return String();
	}
	source_file->store_buffer(reinterpret_cast<const uint8_t *>(source.data()), source.size());
	source_file->close();

	Array args;
	const String compiler_name = cc.get_file().get_basename().to_lower();
	const bool zig = compiler_name == "zig";
	if (zig)
		args.push_back("cc");
#if defined(YEP_IS_WINDOWS)
	if (zig) {
		args.append_array(Array::make("-shared", "-fPIC", "-fvisibility=hidden", "-O2", "-w", "-o", output_path));
	} else {
		args.append_array(Array::make("/LD", "/O2", "/w", "/Fe" + output_path));
	}
#else
	args.append_array(Array::make("-shared", "-fPIC", "-fvisibility=hidden", "-O2", "-w", "-o", output_path));
#endif
	if (!extra_cflags.is_empty())
		args.append_array(extra_cflags.split(" ", false));
	args.push_back(c99_path);

	Array output;
	const int ret = OS::get_singleton()->execute(cc, args, output, true);
	DirAccess::remove_absolute(c99_path);
	if (ret != 0 || !FileAccess::file_exists(output_path)) {
		DirAccess::remove_absolute(output_path);
		if (quiet) {
			auto_bake_compiler_failed.store(true);
			WARN_PRINT_ONCE("SafeGDScript auto-bake is disabled because the configured C compiler failed. Check sandbox/binary_translation/compiler.");
		} else {
			ERR_PRINT("Sandbox: Failed to compile binary translation with " + cc + ".");
			UtilityFunctions::print(output);
		}
		return String();
	}
	if (FileAccess::file_exists(final_path)) {
		DirAccess::remove_absolute(output_path);
		return final_path;
	}
	if (DirAccess::rename_absolute(output_path, final_path) != OK) {
		DirAccess::remove_absolute(output_path);
		if (!quiet) ERR_PRINT("Sandbox: Failed to publish binary translation: " + final_path);
		return String();
	}
	if (!quiet)
		UtilityFunctions::print("Baked binary translation ", hash_string(hash), " -> ", final_path,
				" (", FileAccess::get_file_as_bytes(final_path).size(), " bytes)");
	return final_path;
#endif
}
} // namespace

Sandbox::BakeOptions Sandbox::current_bake_options() const {
	return BakeOptions {
		.ignore_limit = get_instructions_max() <= 0,
		.nbit_as = m_bintr_automatic_nbit_as,
		.unchecked = unchecked_memory_wanted(),
	};
}

String Sandbox::emit_binary_translation(bool ignore_instruction_limit, bool automatic_nbit_as) const {
	BakeOptions options = current_bake_options();
	options.ignore_limit = ignore_instruction_limit;
	options.nbit_as = automatic_nbit_as;
	return emit_binary_translation(options, false);
}

String Sandbox::emit_binary_translation(const BakeOptions &bake_options, bool shared_library,
		uint32_t *r_hash) const {
	const std::string_view &binary = machine().memory.binary();
	if (binary.empty()) {
		ERR_PRINT("Sandbox: No binary loaded.");
		return String();
	}
#ifdef RISCV_BINARY_TRANSLATION
	std::string code_output;
	// 1. Re-create the same options
	auto options = std::make_shared<riscv::MachineOptions<RISCV_ARCH>>(machine().options());
	options->use_shared_execute_segments = false;
	options->translate_enabled = false;
	options->translate_enable_embedded = false;
	options->translate_invoke_compiler = false;
	options->translate_ignore_instruction_limit = bake_options.ignore_limit;
	options->translate_automatic_nbit_address_space = bake_options.nbit_as;
	options->translate_unsafe_remove_checks = bake_options.unchecked;
	options->translate_use_register_caching = false;
	// Avoid any shenanigans with background compilation
	options->translate_background_callback = nullptr;
#ifdef RISCV_ASMJIT
	// This machine only exists to emit C99, so there is nothing for asmjit to do.
	options->asmjit_enabled = false;
	options->asmjit_background_callback = nullptr;
#endif
	// TODO: Make this configurable
	options->translate_instr_max = 75'000u;

	// 2. Enable binary translation output to a string
	options->cross_compile.push_back(riscv::MachineTranslationEmbeddableCodeOptions{
		.result_c99 = shared_library ? nullptr : &code_output,
		.result_shared_c99 = shared_library ? &code_output : nullptr,
	});

	// 3. Emit the binary translation by constructing a new machine
	machine_t m{ binary, *options };
	const auto &main_segment = m.memory.exec_segment_for(m.memory.start_address());
	if (r_hash != nullptr)
		*r_hash = main_segment->translation_hash();

	// 4. Wait for any potential background compilation to finish
	if constexpr (riscv::libtcc_enabled) {
		main_segment->wait_for_compilation_complete();
	}

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

String Sandbox::binary_translation_cache_dir(bool create) {
#ifdef RISCV_BINARY_TRANSLATION
	static std::mutex cache_mutex;
	static String cached;
	std::lock_guard<std::mutex> lock(cache_mutex);
	if (!cached.is_empty() && DirAccess::dir_exists_absolute(cached))
		return cached;
	const String shipped = OS::get_singleton()->get_executable_path().get_base_dir().path_join("bintr");
	if (DirAccess::dir_exists_absolute(shipped)) {
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

String Sandbox::binary_translation_path(uint32_t hash, const String &out_dir) {
	const String directory = out_dir.is_empty() ? binary_translation_cache_dir(false) : globalize_directory(out_dir);
	if (directory.is_empty() || hash == 0 || BINTR_SUFFIX[0] == '\0')
		return String();
	return directory.path_join("bintr-" + hash_string(hash) + BINTR_SUFFIX);
}

bool Sandbox::bintr_lookup_enabled() {
#ifdef RISCV_BINARY_TRANSLATION
	return SandboxProjectSettings::binary_translation_enabled() &&
			!binary_translation_cache_dir(false).is_empty();
#else
	return false;
#endif
}

String Sandbox::bake_binary_translation(const String &out_dir) const {
#ifdef RISCV_BINARY_TRANSLATION
	if (is_in_vmcall()) {
		ERR_PRINT("Sandbox: Cannot bake a binary translation while in a VM call.");
		return String();
	}
	if (get_restrictions()) {
		ERR_PRINT("Sandbox: Cannot bake a binary translation while restrictions are enabled.");
		return String();
	}
	const String directory = out_dir.is_empty() ? binary_translation_cache_dir(true) : globalize_directory(out_dir);
	if (directory.is_empty()) {
		ERR_PRINT("Sandbox: No writable binary translation cache directory is configured.");
		return String();
	}
	uint32_t emitted_hash = 0;
	const String source = emit_binary_translation(current_bake_options(), true, &emitted_hash);
	const uint32_t live_hash = uint32_t(get_translation_hash());
	if (source.is_empty() || live_hash == 0 || emitted_hash != live_hash) {
		ERR_PRINT(vformat("Sandbox: Refusing binary translation whose hash differs from the live machine (%08X != %08X).",
				emitted_hash, live_hash));
		return String();
	}
	const CharString source_utf8 = source.utf8();
	return compile_shared_translation(std::string(source_utf8.get_data(), source_utf8.length()),
			live_hash, directory, SandboxProjectSettings::binary_translation_compiler(),
			SandboxProjectSettings::binary_translation_extra_cflags(), false);
#else
	(void)out_dir;
	WARN_PRINT_ONCE("Sandbox: Binary translation is not enabled.");
	return String();
#endif
}

bool Sandbox::is_translation_baked() const {
	const String path = binary_translation_path(uint32_t(get_translation_hash()));
	return !path.is_empty() && FileAccess::file_exists(path);
}

String Sandbox::bake_binary_translation_from_buffer(const PackedByteArray &binary,
		uint32_t memory_max, const BakeOptions &bake_options, const String &out_dir,
		const String &compiler, const String &extra_cflags, bool quiet) {
#ifdef RISCV_BINARY_TRANSLATION
	if (binary.is_empty())
		return String();
	std::string source;
	auto options = riscv::MachineOptions<RISCV_ARCH>{};
	options.memory_max = uint64_t(memory_max) << 20;
	options.stack_size = GUEST_STACK_SIZE;
	options.use_shared_execute_segments = false;
	options.translate_enabled = false;
	options.translate_enable_embedded = false;
	options.translate_invoke_compiler = false;
	options.translate_ignore_instruction_limit = bake_options.ignore_limit;
	options.translate_automatic_nbit_address_space = bake_options.nbit_as;
	options.translate_unsafe_remove_checks = bake_options.unchecked;
	options.translate_use_register_caching = false;
	options.translate_background_callback = nullptr;
#ifdef RISCV_ASMJIT
	options.asmjit_enabled = false;
	options.asmjit_background_callback = nullptr;
#endif
	options.translate_instr_max = 75'000u;
	options.cross_compile.emplace_back(riscv::MachineTranslationEmbeddableCodeOptions{
		.result_shared_c99 = &source,
	});
	const std::string_view bytes(reinterpret_cast<const char *>(binary.ptr()), binary.size());
	machine_t emitter(bytes, options);
	const auto &main_segment = emitter.memory.exec_segment_for(emitter.memory.start_address());
	if constexpr (riscv::libtcc_enabled)
		main_segment->wait_for_compilation_complete();
	const uint32_t hash = main_segment->translation_hash();
	return compile_shared_translation(source, hash, globalize_directory(out_dir), compiler,
			extra_cflags, quiet);
#else
	(void)binary; (void)memory_max; (void)bake_options; (void)out_dir;
	(void)compiler; (void)extra_cflags; (void)quiet;
	return String();
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

bool Sandbox::try_compile_binary_translation(String shared_library_path, const String &cc, const String &extra_cflags, bool ignore_instruction_limit, bool automatic_nbit_as) {
	if (this->is_binary_translated() && !this->is_jit()) {
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
	// Android, WebAssembly, Nintendo Switch, and iOS do not support direct
	// compilation of binary translations into shared libraries (on that platform).
#if defined(__ANDROID__) || defined(__wasm__) || defined(__SWITCH__) || defined(__EMSCRIPTEN__)
	ERR_PRINT("Sandbox: Directly compiling binary translation is not supported on this platform.");
	return false;
#elif defined(__APPLE__) && !defined(__MACH__) // iOS?
	// TODO: Check for iOS?
	ERR_PRINT("Sandbox: Directly compiling binary translation is not supported on this platform.");
	return false;
#endif

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
	static const String c99_path = "user://temp_sandbox_generated.c";
	Ref<FileAccess> fa = FileAccess::open(c99_path, FileAccess::ModeFlags::WRITE);
	if (!fa->is_open()) {
		ERR_PRINT("Sandbox: Failed to open file for writing: " + c99_path);
		return false;
	}
	fa->store_string(code);
	fa->close();
	// Compile the generated code
	Array args;
	if (cc.ends_with("zig")) {
		// Zig cc - C compiler (faster than C++)
		args.push_back("cc");
	}
#if defined(__linux__) || defined(YEP_IS_OSX)
	args.push_back("-shared");
	args.push_back("-fPIC");
	args.push_back("-fvisibility=hidden");
	args.push_back("-O2");
	args.push_back("-w");
	args.push_back("-DCALLBACK_INIT");
	args.push_back("-o");
#elif defined(YEP_IS_WINDOWS)
	if (cc.ends_with("zig")) {
		// Zig cc - C compiler
		args.push_back("-shared");
		args.push_back("-fPIC");
		args.push_back("-fvisibility=hidden");
		args.push_back("-O2");
		args.push_back("-w");
		args.push_back("-DCALLBACK_INIT");
		args.push_back("-o");
	} else {
		args.push_back("/LD");
		args.push_back("/O2");
		args.push_back("/w");
		args.push_back("/DCALLBACK_INIT");
		args.push_back("/Fe");
	}
#endif
	args.push_back(shared_library_path.replace("res://", ""));
	if (!extra_cflags.is_empty())
		args.append_array(extra_cflags.split(" "));
	args.push_back(ProjectSettings::get_singleton()->globalize_path(c99_path));
	UtilityFunctions::print(cc, args);
	Array output;
	int ret = OS::get_singleton()->execute(cc, args, output, true);
	// Remove the generated C99 file
	Ref<DirAccess> dir = DirAccess::open("user://");
	dir->remove(c99_path);
	if (ret != 0) {
		ERR_PRINT("Sandbox: Failed to compile generated code: " + shared_library_path);
		UtilityFunctions::print(output);
		return false;
	}
	return true;
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
