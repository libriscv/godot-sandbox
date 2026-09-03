#include "sandbox.h"

#include "fast_cast.hpp"
#include "guest_datatypes.h"
#include "gdscript/compiler/call_abi.h"
#include "gdscript/compiler/debug_layout.h"
#include "gdscript/compiler/instance_layout.h"
#include "gdscript/compiler/trait_cache_layout.h"
#include "sandbox_project_settings.h"
#include "scoped_tree_base.h"
#include "variant_coerce.h"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#if defined(RISCV_BINARY_TRANSLATION) || defined(RISCV_ASMJIT)
#include <future>
#include <mutex>
#include <thread>
#endif

using namespace godot;

// fast_cast_to() picks a different (and much cheaper) strategy for engine classes than for
// our own GDCLASS() ones, based on a compile-time trait. If a godot-cpp update ever breaks
// that detection, the extension classes would silently start being static_cast from any
// object that shares their engine base class, so fail the build here instead.
static_assert(is_extension_class_v<Sandbox>, "GDCLASS() detection broke: fast_cast_to() would be unsafe for extension classes");
static_assert(!is_extension_class_v<godot::Node>, "GDCLASS() detection broke: fast_cast_to() would be needlessly slow for engine classes");

static constexpr bool VERBOSE_PROPERTIES = false;
static const int HEAP_SYSCALLS_BASE = 480;
static const int MEMORY_SYSCALLS_BASE = 485;
static const std::vector<std::string> program_arguments = { "program" };
static riscv::Machine<RISCV_ARCH> dummy_machine;
enum SandboxPropertyNameIndex : int {
	PROP_REFERENCES_MAX,
	PROP_COROUTINES_MAX,
	PROP_MEMORY_MAX,
	PROP_EXECUTION_TIMEOUT,
	PROP_ALLOCATIONS_MAX,
	PROP_UNBOXED_ARGUMENTS,
	PROP_PRECISE_SIMULATION,
	PROP_BINTR_NBIT_AS,
	PROP_BINTR_REG_CACHE,
	PROP_PROFILING,
	PROP_RESTRICTIONS,
	PROP_PROGRAM,
	PROP_MONITOR_HEAP_USAGE,
	PROP_MONITOR_HEAP_CHUNK_COUNT,
	PROP_MONITOR_HEAP_ALLOCATION_COUNTER,
	PROP_MONITOR_HEAP_DEALLOCATION_COUNTER,
	PROP_MONITOR_EXCEPTIONS,
	PROP_MONITOR_EXECUTION_TIMEOUTS,
	PROP_MONITOR_CALLS_MADE,
	PROP_MONITOR_BINARY_TRANSLATED,
	PROP_GLOBAL_CALLS_MADE,
	PROP_GLOBAL_EXCEPTIONS,
	PROP_GLOBAL_TIMEOUTS,
	PROP_MONITOR_ACCUMULATED_STARTUP_TIME,
	PROP_MONITOR_GLOBAL_INSTANCE_COUNT,
};
static std::vector<StringName> property_names;

#if defined(RISCV_BINARY_TRANSLATION) || defined(RISCV_ASMJIT)
namespace {
// A background translation runs code that lives inside this extension. If Godot
// unloads the extension while one is still running, the thread's own code is
// unmapped underneath it and the process dies on the way out. So they are
// tracked here and joined before the extension goes away.
struct BackgroundTranslation {
	std::thread thread;
	std::shared_ptr<std::atomic<bool>> done;
};
std::mutex background_translations_mutex;
std::vector<BackgroundTranslation> background_translations;
uint32_t auto_bake_pending = 0;
uint32_t auto_bake_new = 0;
uint64_t auto_bake_batch_start = 0;

void finish_auto_bake(bool new_file) {
	std::lock_guard<std::mutex> lock(background_translations_mutex);
	if (auto_bake_pending == 0)
		return;
	if (new_file)
		auto_bake_new++;
	auto_bake_pending--;
	if (auto_bake_pending == 0) {
		if (auto_bake_new > 0) {
			const double elapsed_seconds =
					double(Time::get_singleton()->get_ticks_usec() - auto_bake_batch_start) / 1'000'000.0;
			UtilityFunctions::print(vformat("SafeGDScript auto-bake: %d translations baked in %.1fs",
					auto_bake_new, elapsed_seconds));
		}
		auto_bake_batch_start = 0;
	}
}

struct AutoBakeCompletion {
	bool &new_file;
	~AutoBakeCompletion() { finish_auto_bake(new_file); }
};
} // namespace

void Sandbox::start_background_translation(std::function<void()> &&step)
{
	auto done = std::make_shared<std::atomic<bool>>(false);
	std::thread thread([step = std::move(step), done]() mutable {
		try {
			// This is a no-op if the step is empty.
			if (step)
				step();
		} catch (const std::exception &e) {
			String what = e.what();
			ERR_PRINT(("Binary translation background compilation exception: " + what));
		}
		done->store(true);
	});

	std::lock_guard<std::mutex> lock(background_translations_mutex);
	// Reap whatever finished since the last translation was started, so that a
	// long-running project doesn't accumulate joinable threads.
	for (auto it = background_translations.begin(); it != background_translations.end();) {
		if (it->done->load()) {
			it->thread.join();
			it = background_translations.erase(it);
		} else {
			++it;
		}
	}
	background_translations.push_back({ std::move(thread), std::move(done) });
}

void Sandbox::queue_binary_translation_bake(PackedByteArray binary, uint32_t memory_max)
{
#ifdef RISCV_BINARY_TRANSLATION
	if (binary.is_empty())
		return;
	const BakeOptions options {
		.ignore_limit = false,
		.nbit_as = true,
		.unchecked = true,
	};
	const String output_dir = binary_translation_cache_dir(true);
	const String compiler = SandboxProjectSettings::binary_translation_compiler();
	const String extra_cflags = SandboxProjectSettings::binary_translation_extra_cflags();
	if (output_dir.is_empty() || compiler.is_empty())
		return;
	{
		std::lock_guard<std::mutex> lock(background_translations_mutex);
		if (auto_bake_pending == 0) {
			auto_bake_new = 0;
			auto_bake_batch_start = Time::get_singleton()->get_ticks_usec();
		}
		auto_bake_pending++;
	}
	start_background_translation([binary = std::move(binary), memory_max, options,
			output_dir, compiler, extra_cflags] {
		bool new_file = false;
		AutoBakeCompletion completion { new_file };
		bake_binary_translation_from_buffer(binary, memory_max, options, output_dir,
				compiler, extra_cflags, true, &new_file);
	});
#else
	(void)binary;
	(void)memory_max;
#endif
}

void Sandbox::Deinitialize()
{
	std::vector<BackgroundTranslation> pending;
	{
		std::lock_guard<std::mutex> lock(background_translations_mutex);
		pending.swap(background_translations);
	}
	for (auto &bt : pending) {
		if (bt.thread.joinable())
			bt.thread.join();
	}
}

Dictionary Sandbox::_get_auto_bake_stats() {
	Dictionary stats;
	std::lock_guard<std::mutex> lock(background_translations_mutex);
	stats["pending"] = auto_bake_pending;
	stats["new"] = auto_bake_new;
	return stats;
}
#else
void Sandbox::Deinitialize() {}

Dictionary Sandbox::_get_auto_bake_stats() {
	Dictionary stats;
	stats["pending"] = 0;
	stats["new"] = 0;
	return stats;
}
#endif

void Sandbox::Initialize()
{
	Sandbox::initialize_syscalls();

	property_names = {
		"references_max",
		"coroutines_max",
		"memory_max",
		"execution_timeout",
		"allocations_max",
		"unboxed_arguments",
		"precise_simulation",
		"binary_translation_nbit_as",
		"binary_translation_register_caching",
		"profiling",
		"restrictions",
		"program",
		"monitor_heap_usage",
		"monitor_heap_chunk_count",
		"monitor_heap_allocation_counter",
		"monitor_heap_deallocation_counter",
		"monitor_exceptions",
		"monitor_execution_timeouts",
		"monitor_calls_made",
		"monitor_binary_translated",
		"global_calls_made",
		"global_exceptions",
		"global_timeouts",
		"monitor_accumulated_startup_time",
		"monitor_global_instance_count",
	};
}

String Sandbox::_to_string() const {
	return "[ GDExtension::Sandbox <--> Instance ID:" + uitos(get_instance_id()) + " ]";
}

void Sandbox::_bind_methods() {
	// Constructors.
	ClassDB::bind_static_method("Sandbox", D_METHOD("get_program_metadata", "binary"), &Sandbox::get_program_metadata);
	ClassDB::bind_static_method("Sandbox", D_METHOD("FromBuffer", "buffer"), &Sandbox::FromBuffer);
	ClassDB::bind_static_method("Sandbox", D_METHOD("FromProgram", "program"), &Sandbox::FromProgram);
	// Methods.
	ClassDB::bind_method(D_METHOD("load_buffer", "buffer"), &Sandbox::load_buffer);
	ClassDB::bind_method(D_METHOD("reset", "unload"), &Sandbox::reset, DEFVAL(false));
	{
		MethodInfo mi;
		//mi.arguments.push_back(PropertyInfo(Variant::STRING, "function"));
		mi.name = "vmcall";
		mi.return_val = PropertyInfo(Variant::OBJECT, "result");
		ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "vmcall", &Sandbox::vmcall, mi, DEFVAL(LocalVector<Variant>{}));
		ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "vmcallv", &Sandbox::vmcallv, mi, DEFVAL(LocalVector<Variant>{}));
	}
	ClassDB::bind_method(D_METHOD("vmcallable", "function", "args"), &Sandbox::vmcallable, DEFVAL(Array{}));
	ClassDB::bind_method(D_METHOD("vmcallable_address", "address", "args"), &Sandbox::vmcallable_address, DEFVAL(Array{}));

	// Sandbox restrictions.
	ClassDB::bind_method(D_METHOD("set_restrictions", "restrictions"), &Sandbox::set_restrictions);
	ClassDB::bind_method(D_METHOD("get_restrictions"), &Sandbox::get_restrictions);
	ClassDB::bind_method(D_METHOD("add_allowed_object", "instance"), &Sandbox::add_allowed_object);
	ClassDB::bind_method(D_METHOD("remove_allowed_object", "instance"), &Sandbox::remove_allowed_object);
	ClassDB::bind_method(D_METHOD("clear_allowed_objects"), &Sandbox::clear_allowed_objects);
	ClassDB::bind_method(D_METHOD("set_class_allowed_callback", "instance"), &Sandbox::set_class_allowed_callback);
	ClassDB::bind_method(D_METHOD("set_object_allowed_callback", "instance"), &Sandbox::set_object_allowed_callback);
	ClassDB::bind_method(D_METHOD("set_method_allowed_callback", "instance"), &Sandbox::set_method_allowed_callback);
	ClassDB::bind_method(D_METHOD("set_property_allowed_callback", "instance"), &Sandbox::set_property_allowed_callback);
	ClassDB::bind_method(D_METHOD("set_resource_allowed_callback", "instance"), &Sandbox::set_resource_allowed_callback);
	ClassDB::bind_method(D_METHOD("is_allowed_class", "name"), &Sandbox::is_allowed_class);
	ClassDB::bind_method(D_METHOD("is_class_access_restricted"), &Sandbox::is_class_access_restricted);
	ClassDB::bind_method(D_METHOD("is_allowed_object", "instance"), &Sandbox::is_allowed_object);
	// Disambiguated from the const char* overloads, which exist purely for internal call sites.
	ClassDB::bind_method(D_METHOD("is_allowed_method", "instance", "method"),
			static_cast<bool (Sandbox::*)(godot::Object *, const Variant &) const>(&Sandbox::is_allowed_method));
	ClassDB::bind_method(D_METHOD("is_allowed_property", "instance", "property", "is_set"),
			static_cast<bool (Sandbox::*)(godot::Object *, const Variant &, bool) const>(&Sandbox::is_allowed_property), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("is_allowed_resource", "res"), &Sandbox::is_allowed_resource);
	ClassDB::bind_static_method("Sandbox", D_METHOD("restrictive_callback_function", "arg"), &Sandbox::restrictive_callback_function);

	// Internal testing, debugging and introspection.
	ClassDB::bind_method(D_METHOD("set_redirect_stdout", "callback"), &Sandbox::set_redirect_stdout);
	ClassDB::bind_method(D_METHOD("get_general_registers"), &Sandbox::get_general_registers);
	ClassDB::bind_method(D_METHOD("get_floating_point_registers"), &Sandbox::get_floating_point_registers);
	ClassDB::bind_method(D_METHOD("set_argument_registers", "args"), &Sandbox::set_argument_registers);
	ClassDB::bind_method(D_METHOD("get_current_instruction"), &Sandbox::get_current_instruction);
	ClassDB::bind_method(D_METHOD("make_resumable"), &Sandbox::make_resumable);
	ClassDB::bind_method(D_METHOD("resume", "max_instructions"), &Sandbox::resume);

	ClassDB::bind_method(D_METHOD("assault", "test", "iterations"), &Sandbox::assault);
	ClassDB::bind_method(D_METHOD("has_function", "function"), &Sandbox::has_function);
	ClassDB::bind_method(D_METHOD("get_functions"), &Sandbox::get_functions);
	ClassDB::bind_method(D_METHOD("get_public_api"), &Sandbox::get_public_api);
	ClassDB::bind_method(D_METHOD("address_of", "symbol"), &Sandbox::address_of);
	ClassDB::bind_method(D_METHOD("clear_trait_caches"), &Sandbox::clear_trait_caches);
	ClassDB::bind_method(D_METHOD("lookup_address", "address"), &Sandbox::lookup_address);
	ClassDB::bind_static_method("Sandbox", D_METHOD("generate_api", "language", "header_extra", "use_argument_names"), &Sandbox::generate_api, DEFVAL("cpp"), DEFVAL(""), DEFVAL(false));
	ClassDB::bind_static_method("Sandbox", D_METHOD("download_program", "program_name"), &Sandbox::download_program, DEFVAL("hello_world"));

	// Profiling.
	ClassDB::bind_static_method("Sandbox", D_METHOD("get_hotspots", "total", "callable"), &Sandbox::get_hotspots, DEFVAL(6), DEFVAL(Callable()));
	ClassDB::bind_static_method("Sandbox", D_METHOD("clear_hotspots"), &Sandbox::clear_hotspots);

	// Binary translation.
	ClassDB::bind_method(D_METHOD("emit_binary_translation", "ignore_instruction_limit", "automatic_nbit_address_space"),
			static_cast<String (Sandbox::*)(bool, bool) const>(&Sandbox::emit_binary_translation),
			DEFVAL(false), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_translation_hash"), &Sandbox::get_translation_hash);
	ClassDB::bind_method(D_METHOD("bake_binary_translation", "out_dir"), &Sandbox::bake_binary_translation, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("is_translation_baked"), &Sandbox::is_translation_baked);
	if (OS::get_singleton()->has_feature("editor")) {
		ClassDB::bind_static_method("Sandbox", D_METHOD("_queue_binary_translation_bake", "binary", "memory_max"),
				&Sandbox::queue_binary_translation_bake);
		ClassDB::bind_static_method("Sandbox", D_METHOD("_get_auto_bake_stats"), &Sandbox::_get_auto_bake_stats);
	}
	ClassDB::bind_static_method("Sandbox", D_METHOD("load_binary_translation", "shared_library_path", "allow_insecure"), &Sandbox::load_binary_translation, DEFVAL("res://bintr.so"), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("try_compile_binary_translation", "shared_library_path", "compiler", "extra_cflags", "ignore_instruction_limit", "automatic_nbit_as"), &Sandbox::try_compile_binary_translation, DEFVAL("res://bintr"), DEFVAL("cc"), DEFVAL(""), DEFVAL(false), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("is_binary_translated"), &Sandbox::is_binary_translated);
	ClassDB::bind_method(D_METHOD("is_jit"), &Sandbox::is_jit);
	ClassDB::bind_static_method("Sandbox", D_METHOD("set_jit_enabled", "enable"), &Sandbox::set_jit_enabled);
	ClassDB::bind_static_method("Sandbox", D_METHOD("is_jit_enabled"), &Sandbox::is_jit_enabled);
	ClassDB::bind_static_method("Sandbox", D_METHOD("has_feature_jit"), &Sandbox::has_feature_jit);
	ClassDB::bind_static_method("Sandbox", D_METHOD("has_feature_binary_translation"), &Sandbox::has_feature_binary_translation);

	// Properties.
	ClassDB::bind_method(D_METHOD("set", "name", "value"), &Sandbox::set);
	ClassDB::bind_method(D_METHOD("get", "name"), &Sandbox::get);
	ClassDB::bind_method(D_METHOD("get_property_list"), &Sandbox::get_property_list);

	ClassDB::bind_method(D_METHOD("set_max_refs", "max"), &Sandbox::set_max_refs, DEFVAL(MAX_REFS));
	ClassDB::bind_method(D_METHOD("get_max_refs"), &Sandbox::get_max_refs);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "references_max", PROPERTY_HINT_NONE, "Maximum objects and variants referenced by a sandbox call"), "set_max_refs", "get_max_refs");

	// Coroutines
	ClassDB::bind_method(D_METHOD("set_max_coroutines", "max"), &Sandbox::set_max_coroutines, DEFVAL(MAX_COROUTINES));
	ClassDB::bind_method(D_METHOD("get_max_coroutines"), &Sandbox::get_max_coroutines);
	ClassDB::bind_method(D_METHOD("get_coroutine_count"), &Sandbox::get_coroutine_count);
	ClassDB::bind_method(D_METHOD("reap_coroutines"), &Sandbox::reap_coroutines);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "coroutines_max", PROPERTY_HINT_NONE, "Maximum number of suspended coroutines"), "set_max_coroutines", "get_max_coroutines");

	ClassDB::bind_method(D_METHOD("set_memory_max", "max"), &Sandbox::set_memory_max, DEFVAL(MAX_VMEM));
	ClassDB::bind_method(D_METHOD("get_memory_max"), &Sandbox::get_memory_max);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "memory_max", PROPERTY_HINT_NONE, "Maximum memory (in MiB) used by the sandboxed program"), "set_memory_max", "get_memory_max");

	ClassDB::bind_method(D_METHOD("set_instructions_max", "max"), &Sandbox::set_instructions_max, DEFVAL(MAX_INSTRUCTIONS));
	ClassDB::bind_method(D_METHOD("get_instructions_max"), &Sandbox::get_instructions_max);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "execution_timeout", PROPERTY_HINT_NONE, "Maximum millions of instructions executed before cancelling execution"), "set_instructions_max", "get_instructions_max");

	ClassDB::bind_method(D_METHOD("set_allocations_max", "max"), &Sandbox::set_allocations_max, DEFVAL(MAX_HEAP_ALLOCS));
	ClassDB::bind_method(D_METHOD("get_allocations_max"), &Sandbox::get_allocations_max);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "allocations_max", PROPERTY_HINT_NONE, "Maximum number of allocations allowed"), "set_allocations_max", "get_allocations_max");

	ClassDB::bind_method(D_METHOD("set_unboxed_arguments", "unboxed_arguments"), &Sandbox::set_unboxed_arguments);
	ClassDB::bind_method(D_METHOD("get_unboxed_arguments"), &Sandbox::get_unboxed_arguments);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "unboxed_arguments", PROPERTY_HINT_NONE, "Use unboxed arguments for VM function calls"), "set_unboxed_arguments", "get_unboxed_arguments");

	ClassDB::bind_method(D_METHOD("set_precise_simulation", "precise_simulation"), &Sandbox::set_precise_simulation);
	ClassDB::bind_method(D_METHOD("get_precise_simulation"), &Sandbox::get_precise_simulation);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "precise_simulation", PROPERTY_HINT_NONE, "Use precise simulation for VM execution"), "set_precise_simulation", "get_precise_simulation");

	ClassDB::bind_method(D_METHOD("set_binary_translation_nbit_as", "use_nbit_as"), &Sandbox::set_binary_translation_automatic_nbit_as);
	ClassDB::bind_method(D_METHOD("get_binary_translation_nbit_as"), &Sandbox::get_binary_translation_automatic_nbit_as);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "binary_translation_nbit_as", PROPERTY_HINT_NONE, "Use n-bit address space for binary translation"), "set_binary_translation_nbit_as", "get_binary_translation_nbit_as");

	ClassDB::bind_method(D_METHOD("set_binary_translation_register_caching", "register_caching"), &Sandbox::set_binary_translation_register_caching);
	ClassDB::bind_method(D_METHOD("get_binary_translation_register_caching"), &Sandbox::get_binary_translation_register_caching);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "binary_translation_register_caching", PROPERTY_HINT_NONE, "Use register caching for binary translation"), "set_binary_translation_register_caching", "get_binary_translation_register_caching");

	ClassDB::bind_method(D_METHOD("set_binary_translation_bg_compilation", "bg_compilation"), &Sandbox::set_binary_translation_bg_compilation);
	ClassDB::bind_method(D_METHOD("get_binary_translation_bg_compilation"), &Sandbox::get_binary_translation_bg_compilation);

	ClassDB::bind_method(D_METHOD("get_unchecked_memory"), &Sandbox::get_unchecked_memory);

	ClassDB::bind_method(D_METHOD("set_profiling", "enable"), &Sandbox::set_profiling, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_profiling"), &Sandbox::get_profiling);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "profiling", PROPERTY_HINT_NONE, "Enable profiling of VM calls"), "set_profiling", "get_profiling");

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "restrictions", PROPERTY_HINT_NONE, "Enable sandbox restrictions"), "set_restrictions", "get_restrictions");

	ClassDB::bind_method(D_METHOD("set_program", "program"), &Sandbox::set_program);
	ClassDB::bind_method(D_METHOD("get_program"), &Sandbox::get_program);
	ClassDB::bind_method(D_METHOD("has_program_loaded"), &Sandbox::has_program_loaded);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "program", PROPERTY_HINT_RESOURCE_TYPE, "ELFScript"), "set_program", "get_program");

	// Group for monitored Sandbox health.
	ADD_GROUP("Sandbox Monitoring", "monitor_");

	ClassDB::bind_method(D_METHOD("get_heap_usage"), &Sandbox::get_heap_usage);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_heap_usage", PROPERTY_HINT_NONE, "Current memory arena usage", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_heap_usage");

	ClassDB::bind_method(D_METHOD("get_heap_chunk_count"), &Sandbox::get_heap_chunk_count);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_heap_chunk_count", PROPERTY_HINT_NONE, "Number of memory chunks allocated", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_heap_chunk_count");

	ClassDB::bind_method(D_METHOD("get_heap_allocation_counter"), &Sandbox::get_heap_allocation_counter);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_heap_allocation_counter", PROPERTY_HINT_NONE, "Number of heap allocations", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_heap_allocation_counter");

	ClassDB::bind_method(D_METHOD("get_heap_deallocation_counter"), &Sandbox::get_heap_deallocation_counter);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_heap_deallocation_counter", PROPERTY_HINT_NONE, "Number of heap deallocations", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_heap_deallocation_counter");

	ClassDB::bind_method(D_METHOD("get_exceptions"), &Sandbox::get_exceptions);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_exceptions", PROPERTY_HINT_NONE, "Number of exceptions thrown", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_exceptions");

	ClassDB::bind_method(D_METHOD("get_timeouts"), &Sandbox::get_timeouts);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_execution_timeouts", PROPERTY_HINT_NONE, "Number of execution timeouts", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_timeouts");

	ClassDB::bind_method(D_METHOD("get_calls_made"), &Sandbox::get_calls_made);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_calls_made", PROPERTY_HINT_NONE, "Number of calls made", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_calls_made");

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "monitor_binary_translated", PROPERTY_HINT_NONE, "Number of calls made", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "is_binary_translated");

	ClassDB::bind_static_method("Sandbox", D_METHOD("get_global_calls_made"), &Sandbox::get_global_calls_made);
	ClassDB::bind_static_method("Sandbox", D_METHOD("get_global_exceptions"), &Sandbox::get_global_exceptions);
	ClassDB::bind_static_method("Sandbox", D_METHOD("get_global_timeouts"), &Sandbox::get_global_timeouts);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_global_calls_made", PROPERTY_HINT_NONE, "Number of calls made", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_global_calls_made");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_global_exceptions", PROPERTY_HINT_NONE, "Number of exceptions thrown", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_global_exceptions");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_global_execution_timeouts", PROPERTY_HINT_NONE, "Number of execution timeouts", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_global_timeouts");

	ClassDB::bind_static_method("Sandbox", D_METHOD("get_global_instance_count"), &Sandbox::get_global_instance_count);
	ClassDB::bind_static_method("Sandbox", D_METHOD("get_accumulated_startup_time"), &Sandbox::get_accumulated_startup_time);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_global_instance_count", PROPERTY_HINT_NONE, "Number of active sandbox instances", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_global_instance_count");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "monitor_accumulated_startup_time", PROPERTY_HINT_NONE, "Accumulated startup time of all sandbox instantiations", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_accumulated_startup_time");

	// Group for sandboxed properties.
	ADD_GROUP("Sandboxed Properties", "custom_");
}

std::vector<PropertyInfo> Sandbox::create_sandbox_property_list() {
	std::vector<PropertyInfo> list;
	// Create a list of properties for the Sandbox class only.
	// This is used to expose the basic properties to the editor.

	// Group for sandbox restrictions.
	list.push_back(PropertyInfo(Variant::INT, "references_max", PROPERTY_HINT_NONE));
	list.push_back(PropertyInfo(Variant::INT, "coroutines_max", PROPERTY_HINT_NONE));
	list.push_back(PropertyInfo(Variant::INT, "memory_max", PROPERTY_HINT_NONE));
	list.push_back(PropertyInfo(Variant::INT, "execution_timeout", PROPERTY_HINT_NONE));
	list.push_back(PropertyInfo(Variant::INT, "allocations_max", PROPERTY_HINT_NONE));
	list.push_back(PropertyInfo(Variant::BOOL, "unboxed_arguments", PROPERTY_HINT_NONE));
	list.push_back(PropertyInfo(Variant::BOOL, "precise_simulation", PROPERTY_HINT_NONE));
	list.push_back(PropertyInfo(Variant::BOOL, "binary_translation_nbit_as", PROPERTY_HINT_NONE));
	list.push_back(PropertyInfo(Variant::BOOL, "binary_translation_register_caching", PROPERTY_HINT_NONE));
	list.push_back(PropertyInfo(Variant::BOOL, "profiling", PROPERTY_HINT_NONE));
	list.push_back(PropertyInfo(Variant::BOOL, "restrictions", PROPERTY_HINT_NONE));

	// Group for sandbox properties.
	list.push_back(PropertyInfo(Variant::OBJECT, "program", PROPERTY_HINT_RESOURCE_TYPE, "ELFScript"));

	// Group for monitored Sandbox health.
	// Add the group name to the property name to group them in the editor.
	list.push_back(PropertyInfo(Variant::NIL, "Monitoring", PROPERTY_HINT_NONE, "monitor_", PROPERTY_USAGE_GROUP));
	list.push_back(PropertyInfo(Variant::INT, "monitor_heap_usage", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY));
	list.push_back(PropertyInfo(Variant::INT, "monitor_heap_chunk_count", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY));
	list.push_back(PropertyInfo(Variant::INT, "monitor_heap_allocation_counter", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY));
	list.push_back(PropertyInfo(Variant::INT, "monitor_heap_deallocation_counter", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY));
	list.push_back(PropertyInfo(Variant::INT, "monitor_exceptions", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY));
	list.push_back(PropertyInfo(Variant::INT, "monitor_execution_timeouts", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY));
	list.push_back(PropertyInfo(Variant::INT, "monitor_calls_made", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY));
	list.push_back(PropertyInfo(Variant::BOOL, "monitor_binary_translated", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY));

	return list;
}

void Sandbox::constructor_initialize() {
	// Reap coroutines before clearing the states their frames reference.
	this->reap_coroutines();
	this->m_program_generation += 1;
	this->m_current_state = &this->m_states[0];
	this->m_use_unboxed_arguments = SandboxProjectSettings::use_native_types();
	// For each call state, reset the state
	for (size_t i = 0; i < this->m_states.size(); i++) {
		this->m_states[i].reinitialize(i, this->m_max_refs);
	}
	this->m_perm_slots.clear();
	this->m_perm_free_slots.clear();
}
void Sandbox::reset_machine() {
	// Records live in the machine being torn down.
	this->m_instance_base = 0;
	this->m_default_instance_base = 0;
	this->m_instance_record_size = 0;
	this->m_instance_init_address = 0;
	this->m_live_instance_records.clear();
	// The heap they would be returned to goes with the machine.
	this->m_deferred_instance_records.clear();
	try {
		if (this->m_machine != &dummy_machine) {
			delete this->m_machine;
			this->m_machine = &dummy_machine;
		}
	} catch (const std::exception &e) {
		ERR_PRINT(("Sandbox exception: " + std::string(e.what())).c_str());
	}
}
void Sandbox::full_reset() {
	this->reset_machine();
	const bool unboxed_arguments = this->get_unboxed_arguments();
	this->constructor_initialize();
	this->set_unboxed_arguments(unboxed_arguments);

	this->m_properties.clear();
	this->m_public_api_functions.clear();
	this->m_lookup.clear();
	this->m_sname_lookup.clear();
	this->m_name_addresses.clear();
	this->m_guest_names.clear();
	this->m_object_bindings.clear();
	this->m_retained_objects.clear();
	// Allowed-objects list survives: it describes the host policy, not the program.
}
void Sandbox::set_tree_base(godot::Node *tree_base) {
	this->m_tree_base = tree_base != nullptr ? tree_base->get_instance_id() : godot::ObjectID();
}
godot::Node *Sandbox::get_tree_base() const {
	if (this->m_tree_base.is_null()) {
		return nullptr;
	}
	return Object::cast_to<Node>(ObjectDB::get_instance(this->m_tree_base));
}
godot::Object *Sandbox::get_script_instance_owner() const {
	return this->m_script_instance_owner.is_null() ? nullptr
		: ObjectDB::get_instance(this->m_script_instance_owner);
}

void Sandbox::read_instance_layout() {
	this->m_instance_base = 0;
	this->m_default_instance_base = 0;
	this->m_instance_record_size = 0;
	this->m_instance_init_address = 0;

	const gaddr_t blob = this->m_gdsc_instance_blob;
	if (blob == 0) {
		return;
	}
	uint32_t magic = 0;
	uint32_t version = 0;
	uint32_t record_size = 0;
	uint32_t member_count = 0;
	uint64_t default_base = 0;
	try {
		machine().copy_from_guest(&magic, blob + gdscript::InstanceLayout::MAGIC_OFF, sizeof(magic));
		machine().copy_from_guest(&version, blob + gdscript::InstanceLayout::VERSION_OFF, sizeof(version));
		machine().copy_from_guest(&default_base, blob + gdscript::InstanceLayout::DEFAULT_BASE_OFF, sizeof(default_base));
		machine().copy_from_guest(&record_size, blob + gdscript::InstanceLayout::RECORD_SIZE_OFF, sizeof(record_size));
		machine().copy_from_guest(&member_count, blob + gdscript::InstanceLayout::MEMBER_COUNT_OFF, sizeof(member_count));
	} catch (const std::exception &e) {
		ERR_PRINT("Sandbox: unreadable instance layout: " + String(e.what()));
		return;
	}
	if (magic != gdscript::InstanceLayout::MAGIC || version != gdscript::InstanceLayout::LAYOUT_VERSION) {
		ERR_PRINT("Sandbox: the program's instance layout is not one this build knows.");
		return;
	}
	// Member count must match record size (also catches wrong real_t width).
	// The record must be a valid Variant array in guest memory.
	if (record_size == 0 && member_count == 0) {
		return; // No members.
	}
	if (uint64_t(record_size) != uint64_t(member_count) * sizeof(GuestVariant)) {
		ERR_PRINT("Sandbox: the program's instance record size disagrees with its member count.");
		return;
	}
	if (!this->variant_area_is_sane(gaddr_t(default_base), gaddr_t(record_size), "instance record")) {
		return;
	}
	this->m_default_instance_base = gaddr_t(default_base);
	this->m_instance_record_size = gaddr_t(record_size);
	this->m_instance_base = gaddr_t(default_base);
	this->m_instance_init_address = this->m_gdsc_instance_init;
}

bool Sandbox::variant_area_is_sane(gaddr_t base, gaddr_t bytes, const char *what) {
	// Guest ELF is untrusted. Refuse areas that are not whole-slot aligned,
	// exceed the permanent pool, or lie outside guest writable memory.
	const String area = String(what);
	if (base == 0 || bytes == 0) {
		ERR_PRINT("Sandbox: the program's " + area + " is empty or unplaced.");
		return false;
	}
	if (bytes % sizeof(GuestVariant) != 0) {
		ERR_PRINT("Sandbox: the program's " + area + " is not a whole number of Variants."
				" Was it built for a different real_t width?");
		return false;
	}
	const uint64_t slots = uint64_t(bytes) / sizeof(GuestVariant);
	if (slots > PERM_MAX_SLOTS) {
		ERR_PRINT("Sandbox: the program's " + area + " claims more slots than the sandbox can hold.");
		return false;
	}
	try {
		// memarray validates the whole span in one call.
		machine().memory.memarray<GuestVariant>(base, size_t(slots));
	} catch (const std::exception &e) {
		ERR_PRINT("Sandbox: the program's " + area + " is not in guest memory: " + String(e.what()));
		return false;
	}
	return true;
}

void Sandbox::scan_startup_symbols() {
	this->m_properties_address = 0;
	this->m_gdsc_globals_base = 0;
	this->m_gdsc_globals_size = 0;
	this->m_gdsc_instance_blob = 0;
	this->m_gdsc_instance_init = 0;

	// .symtab is linear and C++/Rust guests export thousands of symbols.
	// One pass finds everything. GDScript names checked only when .gdsmeta
	// is present (few section headers).
	const bool is_gdscript = !Sandbox::elf_section_bytes(
			machine().memory.binary(), gdscript::GDSMETA_SECTION).empty();

	try {
		machine().memory.for_each_symbol([&](const riscv::Elf<RISCV_ARCH>::Sym &sym, const char *name) {
			if (name == nullptr) {
				return;
			}
			const std::string_view symbol(name);
			if (symbol == "properties") {
				if (this->m_properties_address == 0) {
					this->m_properties_address = gaddr_t(sym.st_value);
				}
				return;
			}
			if (!is_gdscript) {
				return;
			}
			if (symbol == gdscript::DEBUG_GLOBALS_SYMBOL) {
				if (this->m_gdsc_globals_base == 0) {
					// Globals have no header; length comes from the symbol only.
					this->m_gdsc_globals_base = gaddr_t(sym.st_value);
					this->m_gdsc_globals_size = gaddr_t(sym.st_size);
				}
			} else if (symbol == gdscript::INSTANCE_SYMBOL) {
				if (this->m_gdsc_instance_blob == 0) {
					this->m_gdsc_instance_blob = gaddr_t(sym.st_value);
				}
			} else if (symbol == gdscript::INSTANCE_INIT_SYMBOL) {
				if (this->m_gdsc_instance_init == 0) {
					this->m_gdsc_instance_init = gaddr_t(sym.st_value);
				}
			}
		});
	} catch (const std::exception &e) {
		ERR_PRINT("Sandbox: unreadable symbol table: " + String(e.what()));
	}

	// Only validation point for the globals area (length from symbol alone).
	// Dropping a bogus one also skips the elevated startup that reads it.
	if (this->m_gdsc_globals_base != 0 &&
		!this->variant_area_is_sane(this->m_gdsc_globals_base, this->m_gdsc_globals_size, "globals area")) {
		this->m_gdsc_globals_base = 0;
		this->m_gdsc_globals_size = 0;
	}
}

void Sandbox::promote_startup_handles() {
	// Both areas already validated (whole Variants, in guest memory, within pool).
	const gaddr_t globals_base = this->m_gdsc_globals_base;
	const size_t globals_bytes = size_t(this->m_gdsc_globals_size);
	const size_t record_bytes = size_t(this->m_instance_record_size);
	const size_t slots = (globals_bytes + record_bytes) / sizeof(GuestVariant);
	// Reserve before promoting; a full pool leaves handles in discarded state.
	this->reserve_permanent_state(clamped_perm_slots(size_t(this->m_max_refs) + slots));

	const auto promote_area = [this](gaddr_t base, size_t bytes) {
		if (base == 0) {
			return;
		}
		const size_t count = bytes / sizeof(GuestVariant);
		for (size_t i = 0; i < count; i++) {
			GuestVariant *slot = machine().memory.memarray<GuestVariant>(
					base + gaddr_t(i * sizeof(GuestVariant)), 1);
			// Negative = permanent slot or VASSIGN's empty sentinel.
			if (!slot->is_scoped_variant() || int32_t(slot->v.i) < 0) {
				continue;
			}
			try {
				slot->v.i = int32_t(this->create_permanent_variant(unsigned(slot->v.i)));
			} catch (const std::exception &e) {
				// Bogus index must not block the rest of the area.
				ERR_PRINT("Sandbox: could not promote a startup value: " + String(e.what()));
			}
		}
	};
	try {
		promote_area(globals_base, globals_bytes);
		promote_area(this->m_default_instance_base, record_bytes);
	} catch (const std::exception &e) {
		ERR_PRINT("Sandbox: could not read a startup area: " + String(e.what()));
	}
}

void Sandbox::run_instance_initializer(gaddr_t address, gaddr_t base) {
	const bool reentrant = this->is_in_vmcall();
	CurrentState *const previous_state = this->m_current_state;
	CurrentState *const end_state = this->m_states.data() + this->m_states.size();
	if (previous_state + 1 >= end_state) {
		ERR_PRINT("Too many VM calls in progress while initializing an instance");
		return;
	}
	// Initializer temporaries belong to this invocation, not to permanent state.
	// STORE_GLOBAL promotes only the final member value.  Running directly in
	// state zero made every key/string used to build a member permanent, so a
	// second instance of a moderately large literal exhausted the reference cap.
	this->m_current_state = previous_state + 1;
	this->m_current_state->reset();
	this->reserve_call_state(*this->m_current_state);

	const gaddr_t previous_base = this->m_instance_base;
	this->m_instance_base = base;

	riscv::CPU<RISCV_ARCH> &cpu = machine().cpu;
	auto &sp = cpu.reg(riscv::REG_SP);
	const uint64_t max_instr = get_instructions_max() << 20;

	try {
		if (!reentrant) {
			cpu.reg(riscv::REG_RA) = machine().memory.exit_address();
			sp = machine().memory.stack_initial();
			cpu.reg(riscv::REG_ARG0) = base;
			cpu.reg(riscv::REG_TP) = base;
			cpu.jump(address);
			machine().simulate(max_instr ? max_instr : ~0ULL);
		} else {
			riscv::Registers<RISCV_ARCH> regs = cpu.registers();
			cpu.reg(riscv::REG_RA) = machine().memory.exit_address();
			sp -= 16u;
			cpu.reg(riscv::REG_ARG0) = base;
			cpu.reg(riscv::REG_TP) = base;
			cpu.preempt_internal(regs, true, true, address, max_instr ? max_instr : ~0ULL);
		}

		// Folded complex defaults (notably [] and {}) are written directly by
		// emit_instance_init(), without passing through VSTORE_GLOBAL. Promote
		// those handles before the temporary initializer state is discarded.
		const size_t slots = size_t(this->m_instance_record_size) / sizeof(GuestVariant);
		for (size_t i = 0; i < slots; i++) {
			GuestVariant *member = machine().memory.memarray<GuestVariant>(
					base + gaddr_t(i * sizeof(GuestVariant)), 1);
			if (!member->is_scoped_variant() || int32_t(member->v.i) < 0) {
				continue;
			}
			member->v.i = int32_t(this->create_permanent_variant(unsigned(member->v.i)));
		}
	} catch (const std::exception &e) {
		this->handle_exception(address);
	}

	this->m_instance_base = previous_base;
	this->m_current_state = previous_state;
}

gaddr_t Sandbox::create_instance_record() {
	if (!this->has_instance_records()) {
		return this->m_default_instance_base;
	}
	if (!machine().has_arena()) {
		ERR_PRINT("Sandbox: no guest heap to allocate an instance record from.");
		return this->m_default_instance_base;
	}
	const gaddr_t base = gaddr_t(machine().arena().malloc(size_t(this->m_instance_record_size)));
	if (base == 0) {
		ERR_PRINT("Sandbox: out of guest memory for an instance record.");
		return this->m_default_instance_base;
	}
	// INT32_MIN: VASSIGN sentinel, distinct from valid scoped-variant indices.
	const size_t slots = size_t(this->m_instance_record_size) / sizeof(GuestVariant);
	std::vector<uint8_t> blank(size_t(this->m_instance_record_size), 0);
	for (size_t i = 0; i < slots; i++) {
		const int64_t empty_index = int64_t(INT32_MIN);
		std::memcpy(blank.data() + i * sizeof(GuestVariant) + offsetof(GuestVariant, v),
				&empty_index, sizeof(empty_index));
	}
	machine().copy_to_guest(base, blank.data(), blank.size());

	this->m_live_instance_records.insert(base);

	if (this->m_instance_init_address != 0) {
		const size_t live = this->m_live_instance_records.size();
		this->reserve_permanent_state(clamped_perm_slots(size_t(this->m_max_refs) + live * slots));
		this->run_instance_initializer(this->m_instance_init_address, base);
	}
	return base;
}

void Sandbox::destroy_instance_record(gaddr_t base) {
	if (!this->has_instance_records() || base == 0 || base == this->m_default_instance_base) {
		return;
	}
	this->m_live_instance_records.erase(base);
	this->reap_coroutines_for_instance(base);

	if (this->is_in_vmcall()) {
		this->m_deferred_instance_records.push_back(base);
		return;
	}
	this->release_instance_record(base);
}

void Sandbox::release_instance_record(gaddr_t base) {
	const size_t slots = size_t(this->m_instance_record_size) / sizeof(GuestVariant);
	try {
		for (size_t i = 0; i < slots; i++) {
			GuestVariant *gvar = machine().memory.memarray<GuestVariant>(
					base + gaddr_t(i * sizeof(GuestVariant)), 1);
			if (gvar->is_scoped_variant() && Sandbox::is_permanent_variant(gvar->v.i)) {
				this->release_permanent_variant(gvar->v.i);
			}
		}
		if (machine().has_arena()) {
			machine().arena().free(base);
		}
	} catch (const std::exception &e) {
		ERR_PRINT("Sandbox: could not release an instance record: " + String(e.what()));
	}
	this->release_retained_objects(base, gaddr_t(this->m_instance_record_size));
	if (this->m_instance_base == base) {
		this->m_instance_base = this->m_default_instance_base;
	}
}

void Sandbox::drain_deferred_instance_records() {
	while (!this->m_deferred_instance_records.empty()) {
		std::vector<gaddr_t> pending = std::move(this->m_deferred_instance_records);
		this->m_deferred_instance_records.clear();
		for (gaddr_t base : pending) {
			this->release_instance_record(base);
		}
	}
}

gaddr_t Sandbox::rebase_instance_address(gaddr_t address) const noexcept {
	if (!this->has_instance_records() || this->m_instance_base == this->m_default_instance_base) {
		return address;
	}
	if (address < this->m_default_instance_base ||
		address >= this->m_default_instance_base + this->m_instance_record_size) {
		return address;
	}
	return this->m_instance_base + (address - this->m_default_instance_base);
}

Sandbox::Sandbox() {
	this->constructor_initialize();
	this->set_tree_base(this);
	this->m_global_instances_current += 1;
	this->m_global_instances_seen += 1;
	this->reset_machine();
}
Sandbox::Sandbox(const PackedByteArray &buffer) : Sandbox() {
	this->load_buffer(buffer);
}
Sandbox::Sandbox(Ref<ELFScript> program) : Sandbox() {
	this->set_program(program);
}

Sandbox::~Sandbox() {
	if (this->is_in_vmcall()) {
		ERR_PRINT("Sandbox instance destroyed while a VM call is in progress.");
	}
	this->m_global_instances_current -= 1;
	// Quietly: completed signal would run against a node under destruction.
	this->reap_coroutines_internal(false);
	this->set_program_data_internal(nullptr);
	try {
		if (this->m_machine != &dummy_machine)
			delete this->m_machine;
	} catch (const std::exception &e) {
		ERR_PRINT(("Sandbox exception: " + std::string(e.what())).c_str());
	}
}

void Sandbox::set_memory_max(uint32_t max) {
	m_memory_max = max;
	if (this->has_program_loaded() && !this->is_in_vmcall()) {
		// Reset the machine if the memory limit is changed
		const gaddr_t current_arena = machine().memory.memory_arena_size();
		const gaddr_t new_arena_size = uint64_t(max) << 20;
		if (new_arena_size > current_arena) {
			this->reset();
		}
	}
}

void Sandbox::set_program(Ref<ELFScript> program) {
	// Check if a call is being made from the VM already,
	// which could spell trouble when we now reset the machine.
	if (this->is_in_vmcall()) {
		ERR_PRINT("Cannot load a new program while a VM call is in progress.");
		return;
	}

	// Avoid reloading the same program
	if (program.is_valid() && this->m_program_data == program) {
		if (this->m_source_version == program->get_source_version()) {
			return;
		}
	} else {
		this->m_source_version = -1;
	}

	// Try to retain Sandboxed properties
	std::vector<Variant> property_values;
	property_values.reserve(this->m_properties.size());
	for (const SandboxProperty &prop : this->m_properties) {
		Variant value;
		if (this->get_property(prop.name(), value)) {
			property_values.push_back(value);
		} else {
			property_values.push_back(Variant());
		}
	}
	// Move the properties to a temporary vector (reset coming up)
	std::vector<SandboxProperty> properties = std::move(this->m_properties);

	this->set_program_data_internal(program);
	this->m_program_bytes = {};

	// Unload program and reset the machine
	this->full_reset();

	if (this->m_program_data.is_null())
		return;

	if (this->load(&m_program_data->get_content())) {
		this->m_source_version = m_program_data->get_source_version();
	}

	// Restore Sandboxed properties by comparing the new program's properties
	// with the old ones, then comparing the type. If the property is found,
	// try to set the property with the old value.
	for (const SandboxProperty &old_prop : properties) {
		const Variant *value = nullptr;
		for (const SandboxProperty &new_prop : this->m_properties) {
			if (new_prop.name() == old_prop.name() && new_prop.type() == old_prop.type()) {
				value = &property_values[&old_prop - &properties[0]];
				break;
			}
		}
		if (value) {
			this->set_property(old_prop.name(), *value);
		}
	}
}

void Sandbox::clear_trait_caches() {
	if (!has_program_loaded()) return;
	if (is_in_vmcall()) {
		ERR_PRINT("Cannot clear trait caches while a VM call is in progress.");
		return;
	}
	static constexpr size_t CACHE_BYTES = gdscript::TraitCacheLayout::AREA_SIZE;
	try {
		for (size_t index = 0; index < 4096; index++) {
			const gaddr_t address = this->address_of(
					String(gdscript::TRAIT_CACHE_SYMBOL_PREFIX) + itos(index));
			if (address == 0) break;
			uint8_t *cache = machine().memory.memarray<uint8_t>(address, CACHE_BYTES);
			std::fill_n(cache, CACHE_BYTES, uint8_t(0));
		}
	} catch (const std::exception &error) {
		ERR_PRINT("Sandbox: failed to clear trait caches: " + String(error.what()));
	}
}
void Sandbox::set_program_data_internal(Ref<ELFScript> program) {
	if (this->m_program_data.is_valid()) {
		//printf("Sandbox %p: Program *unset* from %s\n", this, this->m_program_data->get_path().utf8().ptr());
		this->m_program_data->unregister_instance(this);
	}
	this->m_program_data = program;
	if (this->m_program_data.is_valid()) {
		//printf("Sandbox %p: Program set to %s\n", this, this->m_program_data->get_path().utf8().ptr());
		this->m_program_data->register_instance(this);
	}
}
Ref<ELFScript> Sandbox::get_program() {
	return m_program_data;
}
void Sandbox::load_buffer(const PackedByteArray &buffer) {
	// Check if a call is being made from the VM already,
	// which could spell trouble when we now reset the machine.
	if (this->is_in_vmcall()) {
		ERR_PRINT("Cannot load a new program while a VM call is in progress.");
		return;
	}

	this->set_program_data_internal(nullptr);
	this->m_program_bytes = buffer;

	// Reset the machine
	this->full_reset();

	this->load(&this->m_program_bytes);
}
void Sandbox::reset(bool unload) {
	// Check if a call is being made from the VM already,
	// which could spell trouble when we now reset the machine.
	if (this->is_in_vmcall()) {
		ERR_PRINT("Cannot reset the sandbox while a VM call is in progress.");
		return;
	}

	// Allow the program to be reloaded
	this->m_source_version = -1;
	if (unload) {
		this->set_program_data_internal(nullptr);
		this->m_program_bytes = {};
		this->full_reset();
	} else {
		// Reset the machine
		if (this->m_program_data.is_valid()) {
			this->set_program(this->m_program_data);
		} else if (!this->m_program_bytes.is_empty()) {
			this->load_buffer(this->m_program_bytes);
		}
	}
}
bool Sandbox::has_program_loaded() const {
	return !machine().memory.binary().empty();
}
bool Sandbox::load(const PackedByteArray *buffer, const std::vector<std::string> *argv_ptr) {
	if (buffer == nullptr || buffer->is_empty()) {
		ERR_PRINT("Empty binary, cannot load program.");
		this->m_unchecked_memory_active = false;
		this->reset_machine();
		return false;
	}
	const std::string_view binary_view = std::string_view{ (const char *)buffer->ptr(), static_cast<size_t>(buffer->size()) };

	// Guest addresses are about to change, so names cached against them are no longer valid.
	this->m_guest_names.clear();

	// Get t0 for the startup time
	const uint64_t startup_t0 = Time::get_singleton()->get_ticks_usec();

#ifdef RISCV_BINARY_TRANSLATION
	const bool bintr_cache = bintr_cache_opted_in();
	const bool bintr_lookup = bintr_cache && bintr_lookup_enabled();
#endif // RISCV_BINARY_TRANSLATION

	/** We can't handle exceptions until the Machine is fully constructed. Two steps.  */
	try {
		// Reset the machine
		if (this->m_machine != &dummy_machine)
			delete this->m_machine;

		auto options = std::make_shared<riscv::MachineOptions<RISCV_ARCH>>(riscv::MachineOptions<RISCV_ARCH>{
				.memory_max = uint64_t(get_memory_max()) << 20, // in MiB
				.stack_size = GUEST_STACK_SIZE,
				//.verbose_loader = true,
#ifdef RISCV_BINARY_TRANSLATION
				// The execute segment stores its translation hash. Sharing it between
				// machines with different checked/n-bit/limit options would reuse the
				// first machine's hash and could activate the wrong native object.
				.use_shared_execute_segments = !bintr_cache,
				.translate_enabled = riscv::libtcc_enabled ? m_bintr_jit : bintr_lookup,
				.translate_enable_embedded = true,
				.translate_future_segments = false,
				.translate_invoke_compiler = riscv::libtcc_enabled && m_bintr_jit,
				.translation_cache = bintr_lookup,
				//.translate_trace = true,
				//.translate_timing = true,
#endif // RISCV_BINARY_TRANSLATION
				.translate_ignore_instruction_limit = get_instructions_max() <= 0,
#ifdef RISCV_BINARY_TRANSLATION
#  ifdef RISCV_LIBTCC
				.translate_use_register_caching = this->m_bintr_register_caching,
#  endif // RISCV_LIBTCC
#endif
				.translate_automatic_nbit_address_space = this->m_bintr_automatic_nbit_as,
#ifdef RISCV_BINARY_TRANSLATION
#  ifdef RISCV_LIBTCC
				.translate_live_patching = false, // Don't meddle with instruction stream
#  endif // RISCV_LIBTCC
				.translation_prefix = binary_translation_cache_dir(false).path_join("bintr-").utf8().get_data(),
#  if defined(__linux__)
				.translation_suffix = ".so",
#  elif defined(_WIN32)
				.translation_suffix = ".dll",
#  elif defined(__APPLE__) && defined(__MACH__)
				.translation_suffix = ".dylib",
#  endif
#endif
#ifdef RISCV_ASMJIT
				.asmjit_enabled = m_bintr_jit,
#endif
		});
		this->m_unchecked_memory_active = this->unchecked_memory_wanted();
		options->translate_unsafe_remove_checks = this->m_unchecked_memory_active;
		options->default_exit_function = "fast_exit";
#if defined(RISCV_BINARY_TRANSLATION) || defined(RISCV_ASMJIT)
		// Background compilation, if enabled, will run the compilation in a separate thread
		// and live-patch the results into the decoder cache after the compilation is done.
		if (this->m_bintr_bg_compilation) {
			// This is called from inside the translator in the main thread, and the
			// goal is to run the callback in a separate thread, to avoid blocking
			// the main thread while the compilation step is running.
			auto background_callback = [](std::function<void()>& callback) {
				Sandbox::start_background_translation(std::move(callback));
			};
#  if defined(RISCV_BINARY_TRANSLATION) && defined(RISCV_LIBTCC)
			options->translate_background_callback = background_callback;
#  endif
#  ifdef RISCV_ASMJIT
			// NOTE: libriscv disables asmjit entirely when binary translation is
			// compiling in the background, as only one backend can own the patched
			// decoder cache. Setting both is still correct, just redundant.
			options->asmjit_background_callback = background_callback;
#  endif
		}
#endif

		this->m_machine = new machine_t{ binary_view, *options };
		this->m_machine->set_options(std::move(options));
	} catch (const std::exception &e) {
		ERR_PRINT(("Sandbox construction exception: " + std::string(e.what())).c_str());
		this->m_machine = &dummy_machine;
		return false;
	}

	/** Now we can process symbols, backtraces etc. */
	// Run GDScript initializers one state up so temporaries are reclaimable.
	// State zero never releases scoped slots, so loops hit the ref cap.
	// Promote surviving handles afterward.
	CurrentState *const startup_state = this->m_current_state;
	bool elevated_startup = false;
	try {
		this->m_is_initialization = true;
		machine_t &m = machine();

		m.set_userdata(this);
		m.set_printer([](const machine_t &m, const char *str, size_t len) {
			Sandbox *sandbox = m.get_userdata<Sandbox>();
			sandbox->print(String::utf8(str, len));
		});

		this->initialize_syscalls_runtime();

		// Single .symtab pass; startup lookups below read its results.
		this->scan_startup_symbols();

		const gaddr_t heap_size = gaddr_t(machine().memory.memory_arena_size() * 0.8) & ~0xFFFLL;
		const gaddr_t heap_area = machine().memory.mmap_allocate(heap_size);

		// Add native system call interfaces
		machine().setup_native_heap(HEAP_SYSCALLS_BASE, heap_area, heap_size);
		machine().setup_native_memory(MEMORY_SYSCALLS_BASE);
		machine().arena().set_max_chunks(get_allocations_max());

		// Set up a Linux environment for the program
		const std::vector<std::string> *argv = argv_ptr ? argv_ptr : &program_arguments;
		m.setup_linux(*argv, { "LC_CTYPE=C", "LC_ALL=C", "TZ=UTC", "LD_LIBRARY_PATH=" });

		// Run the program through to its main() function
		if (!this->m_resumable_mode) {
			if (this->has_gdscript_startup() &&
				startup_state + 1 < this->m_states.data() + this->m_states.size()) {
				elevated_startup = true;
				this->m_current_state = startup_state + 1;
				this->m_current_state->reset();
				this->reserve_call_state(*this->m_current_state);
			}
			if (!this->get_precise_simulation()) {
				if (get_instructions_max() <= 0) {
					m.cpu.simulate_inaccurate(m.cpu.pc());
				} else {
					m.simulate(get_instructions_max() << 20);
				}
			} else {
				// Precise simulation can help discover bugs in the program,
				// as the exact PC address will be known when an exception occurs.
				uint64_t max_instr = get_instructions_max() << 20;
				m.set_max_instructions(max_instr ? max_instr : ~0ULL);
				m.cpu.simulate_precise();
				if (m.instruction_limit_reached()) {
					throw riscv::MachineTimeoutException(riscv::MAX_INSTRUCTIONS_REACHED,
						"Instruction count limit reached", max_instr);
				}
			}
		}
		this->m_is_initialization = false;
	} catch (const std::exception &e) {
		ERR_PRINT(("Sandbox exception: " + std::string(e.what())).c_str());
		this->m_is_initialization = false;
		this->handle_exception(machine().cpu.pc());
	}

	this->read_instance_layout();

	// Promote before restoring state; runs on the failing path too.
	if (elevated_startup) {
		this->promote_startup_handles();
		this->m_current_state = startup_state;
	}

	// Read the program's custom properties, if any
	this->read_program_properties(true);

	// Sync public API to ELFScript if present.
	if (this->m_program_data.is_valid()) {
		if (!this->m_public_api_functions.is_empty()) {
			this->m_program_data->set_public_api_functions(this->m_public_api_functions.duplicate());
		} else if (!this->m_program_data->functions.is_empty()) {
			// No guest registration (e.g. resumable mode); adopt from the resource.
			this->m_public_api_functions = this->m_program_data->functions.duplicate();
			for (int i = 0; i < this->m_program_data->functions.size(); i++) {
				const Dictionary func = this->m_program_data->functions[i];
				String name = func["name"];
				const gaddr_t address = func.get("address", 0x0);
				this->m_lookup.insert_or_assign(name.hash(), LookupEntry{ std::move(name), address });
			}
			this->m_program_data->update_public_api_functions();
		}
	}

	// Accumulate startup time
	const uint64_t startup_t1 = Time::get_singleton()->get_ticks_usec();
	double startup_time = (startup_t1 - startup_t0) / 1e6;
	m_accumulated_startup_time += startup_time;
	//fprintf(stderr, "Sandbox startup time: %.3f seconds\n", startup_time);
	return true;
}

Variant Sandbox::vmcall_address(gaddr_t address, const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error) {
	error.error = GDEXTENSION_CALL_OK;
	return this->vmcall_internal(address, args, arg_count);
}
Variant Sandbox::vmcall(const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error) {
	if (arg_count < 1) {
		error.error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
		error.argument = -1;
		return Variant();
	}

	const Variant &function = *args[0];
	args += 1;
	arg_count -= 1;
	const gaddr_t address = cached_address_of_variant(function);
	if (address == 0) {
		ERR_PRINT("Function not found: " + function.operator String() + " (Added to the public API?)");
		error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		error.argument = 0;
		return Variant();
	}

	error.error = GDEXTENSION_CALL_OK;
	return this->vmcall_internal(address, args, arg_count);
}
Variant Sandbox::vmcallv(const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error) {
	if (arg_count < 1) {
		error.error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
		error.argument = -1;
		return Variant();
	}

	const Variant &function = *args[0];
	args += 1;
	arg_count -= 1;
	const gaddr_t address = cached_address_of_variant(function);
	if (address == 0) {
		ERR_PRINT("Function not found: " + function.operator String() + " (Added to the public API?)");
		error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		error.argument = 0;
		return Variant();
	}

	// Force Variant arguments for the duration of the call, restoring the setting after,
	// including when the call throws.
	struct ScopedBoxedArguments {
		Sandbox &sandbox;
		const bool previous;
		ScopedBoxedArguments(Sandbox &s) :
				sandbox(s), previous(s.get_unboxed_arguments()) {
			sandbox.set_unboxed_arguments(false);
		}
		~ScopedBoxedArguments() { sandbox.set_unboxed_arguments(previous); }
	} boxed(*this);

	error.error = GDEXTENSION_CALL_OK;
	return this->vmcall_internal(address, args, arg_count);
}
Variant Sandbox::vmcall_fn(const StringName &function_name, const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error) {
	if (this->m_throttled > 0) {
		this->m_throttled--;
		return Variant();
	}
	// Sandbox.call() is a special case that allows calling functions by name
	static const StringName s_call("call");
	if (UNLIKELY(stringname_equals(function_name, s_call))) {
		// Redirect to vmcall() with the first argument as the function name
		return this->vmcall(args, arg_count, error);
	}
	const gaddr_t address = cached_address_of(function_name);
	if (address == 0) {
		ERR_PRINT("Function not found: " + function_name + " (Added to the public API?)");
		error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return Variant();
	}

	Variant result = this->vmcall_internal(address, args, arg_count);
	error.error = GDEXTENSION_CALL_OK;
	return result;
}
void Sandbox::setup_arguments_native(gaddr_t arrayDataPtr, GuestVariant *v, const Variant **args, int argc) {
	// In this mode we will try to use registers when possible
	// The stack is already set up from setup_arguments(), so we just need to set up the registers
	machine_t &machine = this->machine();
	int index = 11;
	int flindex = 10;

	for (size_t i = 0; i < argc; i++) {
		const Variant &arg = *args[i];
		const GDNativeVariant *inner = (const GDNativeVariant *)arg._native_ptr();

		// Incoming arguments are implicitly trusted, as they are provided by the host
		// They also have have the guaranteed lifetime of the function call
		switch (arg.get_type()) {
			case Variant::Type::BOOL:
				machine.cpu.reg(index++) = inner->value;
				break;
			case Variant::Type::INT:
				//printf("Type: %u Value: %ld\n", inner->type, inner->value);
				machine.cpu.reg(index++) = inner->value;
				break;
			case Variant::Type::FLOAT: // Variant floats are always 64-bit
				//printf("Type: %u Value: %f\n", inner->type, inner->flt);
				machine.cpu.registers().getfl(flindex++).set_double(inner->flt);
				break;
			case Variant::VECTOR2: { // 8- or 16-byte structs can be passed in registers
				machine.cpu.registers().getfl(flindex++).set_float(inner->vec2_flt[0]);
				machine.cpu.registers().getfl(flindex++).set_float(inner->vec2_flt[1]);
				break;
			}
			case Variant::VECTOR2I: { // 8- or 16-byte structs can be passed in registers
				machine.cpu.reg(index++) = inner->value; // 64-bit packed integers
				break;
			}
			case Variant::VECTOR3: {
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->vec3_flt[0];
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->vec3_flt[2];
				break;
			}
			case Variant::VECTOR3I: {
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->ivec3_int[0];
				machine.cpu.reg(index++) = inner->ivec3_int[2];
				break;
			}
			case Variant::VECTOR4: {
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->vec4_flt[0];
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->vec4_flt[2];
				break;
			}
			case Variant::VECTOR4I: {
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->ivec4_int[0];
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->ivec4_int[2];
				break;
			}
			case Variant::COLOR: { // 16-byte struct (must use integer registers)
				// RVG calling convention:
				// Unions and arrays containing floats are passed in integer registers
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->color_flt[0];
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->color_flt[2];
				break;
			}
			case Variant::PLANE: {
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->vec4_flt[0];
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->vec4_flt[2];
				break;
			}
			case Variant::OBJECT: { // Objects are represented as uintptr_t
				// The engine pointer is right there in the Variant, so this avoids the
				// mutex-locked instance-binding lookup to_object() would need. The caller's
				// Variant keeps the object (and a RefCounted's reference) alive for the call.
				machine.cpu.reg(index++) = this->add_scoped_engine_object(uintptr_t(inner->object_ptr)); // Fits in a single register
				break;
			}
			case Variant::ARRAY:
			case Variant::DICTIONARY:
			case Variant::STRING:
			case Variant::STRING_NAME:
			case Variant::NODE_PATH:
			case Variant::RID:
			case Variant::CALLABLE:
			case Variant::TRANSFORM2D:
			case Variant::BASIS:
			case Variant::TRANSFORM3D:
			case Variant::QUATERNION:
			case Variant::PACKED_BYTE_ARRAY:
			case Variant::PACKED_FLOAT32_ARRAY:
			case Variant::PACKED_FLOAT64_ARRAY:
			case Variant::PACKED_INT32_ARRAY:
			case Variant::PACKED_INT64_ARRAY:
			case Variant::PACKED_VECTOR2_ARRAY:
			case Variant::PACKED_VECTOR3_ARRAY:
			case Variant::PACKED_VECTOR4_ARRAY:
			case Variant::PACKED_COLOR_ARRAY:
			case Variant::PACKED_STRING_ARRAY: { // Uses Variant index to reference the object
				unsigned idx = this->add_scoped_variant(&arg);
				machine.cpu.reg(index++) = idx;
				break;
			}
			default: { // Complex types are passed byref, pushed onto the stack as GuestVariant
				GuestVariant &g_arg = v[i + 1];
				g_arg.set(*this, arg, true);
				machine.cpu.reg(index++) = arrayDataPtr + (i + 1) * sizeof(GuestVariant);
			}
		}
	}

	if (UNLIKELY(index > 18 || flindex > 18)) {
		throw std::runtime_error("Sandbox: Too many arguments for VM function call (register overflow)");
	}
}
GuestVariant *Sandbox::setup_arguments(gaddr_t &sp, const Variant **args, int argc) {
	if (this->get_unboxed_arguments()) {
		sp -= sizeof(GuestVariant) * (argc + 1);
		sp &= ~gaddr_t(0xF); // re-align stack pointer
		const gaddr_t arrayDataPtr = sp;
		const int arrayElements = argc + 1;

		GuestVariant *v = m_machine->memory.memarray<GuestVariant>(arrayDataPtr, arrayElements);

		// Set up first argument (return value, also a Variant)
		m_machine->cpu.reg(10) = arrayDataPtr;

		if (argc > 11)
			throw std::runtime_error("Sandbox: Too many arguments for VM function call");
		setup_arguments_native(arrayDataPtr, v, args, argc);
		// A0 is the return value (Variant) of the function
		return &v[0];
	}

	// The first seven pointers use a1-a7; the rest use the entry stack.
	if (argc > int(gdscript::CallABI::MAX_ARGUMENTS))
		throw std::runtime_error("Sandbox: Too many arguments for VM function call");

	// The offset to where the first Variant is stored
	// The first argument is the return value, so we start at 1
	// The rest are overflow arguments, which are pushed onto the stack
	const int overflow_args = int(gdscript::CallABI::overflow_arguments(size_t(argc)));

	sp -= sizeof(GuestVariant) * (argc + 1) + sizeof(gaddr_t) * overflow_args;
	sp &= ~gaddr_t(0xF); // re-align stack pointer
	const gaddr_t arrayDataPtr = sp + sizeof(gaddr_t) * overflow_args;
	const int arrayElements = argc + 1;

	GuestVariant *v = m_machine->memory.memarray<GuestVariant>(arrayDataPtr, arrayElements);
	gaddr_t *overflow = nullptr;
	if (overflow_args > 0)
		overflow = m_machine->memory.memarray<gaddr_t>(sp, overflow_args);

	// Set up first argument (return value, also a Variant)
	m_machine->cpu.reg(10) = arrayDataPtr + overflow_args * sizeof(GuestVariant);

	for (size_t i = 0; i < argc; i++) {
		const Variant &arg = *args[i];
		GuestVariant &g_arg = v[1 + i];
		// Fast-path for simple types
		GDNativeVariant *inner = (GDNativeVariant *)arg._native_ptr();
		// Incoming arguments are implicitly trusted, as they are provided by the host
		// They also have have the guaranteed lifetime of the function call
		switch (arg.get_type()) {
			case Variant::Type::NIL:
				g_arg.type = Variant::Type::NIL;
				break;
			case Variant::Type::BOOL:
				g_arg.type = Variant::Type::BOOL;
				g_arg.v.b = inner->value;
				break;
			case Variant::Type::INT:
				g_arg.type = Variant::Type::INT;
				g_arg.v.i = inner->value;
				break;
			case Variant::Type::FLOAT:
				g_arg.type = Variant::Type::FLOAT;
				g_arg.v.f = inner->flt;
				break;
			case Variant::OBJECT: {
				// Objects passed directly as arguments are implicitly trusted/allowed.
				// The engine pointer is already in the Variant, so the mutex-locked
				// instance-binding lookup is left until the guest actually uses the handle;
				// the caller's Variant keeps the object alive until then either way.
				g_arg.type = Variant::OBJECT;
				g_arg.v.i = this->add_scoped_engine_object(uintptr_t(inner->object_ptr));
				break;
			}
			default:
				g_arg.set(*this, *args[i], true);
		}
		if (i < gdscript::CallABI::REGISTER_ARGUMENTS) {
			m_machine->cpu.reg(11 + i) = arrayDataPtr + (1 + i) * sizeof(GuestVariant);
		} else {
			overflow[i - gdscript::CallABI::REGISTER_ARGUMENTS] =
				arrayDataPtr + (1 + i) * sizeof(GuestVariant);
		}
	}
	// A0 is the return value (Variant) of the function
	return &v[overflow_args];
}
Variant Sandbox::vmcall_internal(gaddr_t address, const Variant **args, int argc) {
	struct DeferredRecords {
		Sandbox &self;
		~DeferredRecords() {
			if (!self.is_in_vmcall() && !self.m_deferred_instance_records.empty()) {
				self.drain_deferred_instance_records();
			}
		}
	} deferred_records{ *this };

	// Cleared per call. Nested calls must not inherit a resumed frame.
	const uint64_t entering_coroutine = this->m_resume_entry_id;
	this->m_resume_entry_id = 0;
	struct ResumeScope {
		Sandbox &self;
		uint64_t previous;
		~ResumeScope() { self.m_resuming_coroutine_id = previous; }
	} resume_scope{ *this, this->m_resuming_coroutine_id };
	this->m_resuming_coroutine_id = entering_coroutine;

	this->m_current_state += 1;
	const auto *beginptr = this->m_states.data();
	const auto *endptr = this->m_states.data() + this->m_states.size();
	if (UNLIKELY(this->m_current_state >= endptr)) {
		ERR_PRINT("Too many VM calls in progress");
		this->m_exceptions++;
		this->m_global_exceptions++;
		this->m_current_state -= 1;
		return Variant();
	}

	CurrentState &state = *this->m_current_state;
	const bool is_reentrant_call = (this->m_current_state - beginptr) > 1;
	state.reset();

	// Call statistics
	this->m_calls_made++;
	Sandbox::m_global_calls_made++;

	try {
		GuestVariant *retvar = nullptr;
		riscv::CPU<RISCV_ARCH> &cpu = m_machine->cpu;
		auto &sp = cpu.reg(riscv::REG_SP);
		// execute guest function
		const bool writes_instance_base = this->m_instance_record_size != 0;
		if (!is_reentrant_call) {
			if (writes_instance_base) {
				cpu.reg(riscv::REG_TP) = this->m_instance_base;
			}
			cpu.reg(riscv::REG_RA) = m_machine->memory.exit_address();
			// reset the stack pointer to its initial location
			sp = m_machine->memory.stack_initial();
			// set up each argument, and return value
			retvar = this->setup_arguments(sp, args, argc);
			// execute!
			if (UNLIKELY(this->m_precise_simulation)) {
				m_machine->set_instruction_counter(0);
				uint64_t max_instr = get_instructions_max() << 20;
				m_machine->set_max_instructions(max_instr ? max_instr : ~0ULL);
				m_machine->cpu.jump(address);
				m_machine->cpu.simulate_precise();
				if (m_machine->instruction_limit_reached()) {
					throw riscv::MachineTimeoutException(riscv::MAX_INSTRUCTIONS_REACHED,
						"Instruction count limit reached", max_instr);
				}
			} else if (UNLIKELY(this->m_local_profiling_data != nullptr)) {
				LocalProfilingData &profdata = *this->m_local_profiling_data;
				m_machine->cpu.jump(address);
				do {
					const int32_t next = std::max(int32_t(1), int32_t(profdata.profiling_interval) - int32_t(profdata.profiler_icounter_accumulator));
					m_machine->simulate<false>(next, 0u);
					if (m_machine->instruction_limit_reached()) {
						profdata.profiler_icounter_accumulator = 0;
						profdata.visited.push_back(m_machine->cpu.pc());
					}
				} while (m_machine->instruction_limit_reached());
				// update the accumulator with the remaining instructions
				profdata.profiler_icounter_accumulator += m_machine->instruction_counter();
				if (profdata.profiler_icounter_accumulator >= profdata.profiling_interval) {
					profdata.profiler_icounter_accumulator = 0;
				}
				if (!profdata.visited.empty()) {
					ProfilingData &gprofdata = *this->m_profiling_data;
					// Determine ELF path
					std::string_view path = "";
					if (this->m_program_data.is_valid()) {
						path = this->m_program_data->get_std_path();
					}
					// Update the global profiler
					{
						std::scoped_lock lock(profiling_mutex);
						ProfilingState &gprofstate = gprofdata.state[path];
						// Add all the local known functions to the global state,
						// to aid lookup in the profiler later on
						if (gprofstate.lookup.size() < this->m_lookup.size()) {
							gprofstate.lookup.clear();
							for (const auto [hash, entry] : this->m_lookup) {
								gprofstate.lookup.push_back(entry);
							}
						}
						// Update the global visited map
						std::unordered_map<gaddr_t, int> &hotspots = gprofstate.hotspots;
						for (const gaddr_t address : profdata.visited) {
							hotspots[address]++;
						}
					}
					profdata.visited.clear();
				}
			} else if (get_instructions_max() <= 0) {
				m_machine->cpu.simulate_inaccurate(address);
			} else {
				m_machine->simulate_with(get_instructions_max() << 20, 0u, address);
			}
		} else {
			this->reserve_call_state(state);
			riscv::Registers<RISCV_ARCH> regs;
			regs = cpu.registers();
			// we are in a recursive call, so wait before setting exit address
			if (writes_instance_base) {
				cpu.reg(riscv::REG_TP) = this->m_instance_base;
			}
			cpu.reg(riscv::REG_RA) = m_machine->memory.exit_address();
			// Nested calls share one guest stack; preempt restores the outer SP.
			sp -= 16u;
			// set up each argument, and return value
			retvar = this->setup_arguments(sp, args, argc);
			// execute preemption! (precise simulation not supported)
			uint64_t max_instr = get_instructions_max() << 20;
			cpu.preempt_internal(regs, true, true, address, max_instr ? max_instr : ~0ULL);
		}

		// Suspended coroutine: return the Signal to await, not the function result.
		if (UNLIKELY(this->m_pending_suspend != 0)) {
			const uint64_t suspended = this->m_pending_suspend;
			this->m_current_state -= 1;
			// Leave the flag for coroutine_resume() to distinguish re-suspension from return.
			if (this->m_resuming_coroutine_id == suspended) {
				return Variant();
			}
			this->m_pending_suspend = 0;
			Coroutine *co = this->find_coroutine(suspended);
			if (co == nullptr || co->state_object.is_null()) {
				return Variant();
			}
			// Signal(state, "completed"): VM rejects GDExtension objects in place of
			// GDScriptFunctionState, but accepts a Signal on one natively.
			return Signal(co->state_object.ptr(), "completed");
		}

		// Treat return value as pointer to Variant
		Variant result = retvar->toVariant(*this);
		// Restore the previous state
		this->m_current_state -= 1;
		return result;

	} catch (const std::exception &e) {
		if (Engine::get_singleton()->is_editor_hint()) {
			// Throttle exceptions in the sandbox when calling from the editor
			this->m_throttled += EDITOR_THROTTLE;
		}
		this->handle_exception(address);
		// TODO: Free the function arguments and return value? Will help keep guest memory clean

		// Fault unwound past suspend: no frame to resume.
		if (this->m_pending_suspend != 0) {
			const uint64_t suspended = this->m_pending_suspend;
			this->m_pending_suspend = 0;
			this->retire_coroutine(suspended, true);
		}
		this->m_current_state -= 1;
		return Variant();
	}
}
Variant Sandbox::vmcallable(String function, Array args) {
	const gaddr_t address = cached_address_of(function.hash(), function);
	if (address == 0x0) {
		ERR_PRINT("Function not found in the guest: " + function);
		return Variant();
	}
	if (args.size() > int(gdscript::CallABI::MAX_ARGUMENTS)) {
		ERR_PRINT("Too many bound arguments for VM function call");
		return Variant();
	}

	RiscvCallable *call = memnew(RiscvCallable);
	call->init(this, address, std::move(args), !this->get_unboxed_arguments());
	return Callable(call);
}
Variant Sandbox::vmcallable_address(gaddr_t address, Array args) {
	if (args.size() > int(gdscript::CallABI::MAX_ARGUMENTS)) {
		ERR_PRINT("Too many bound arguments for VM function call");
		return Variant();
	}
	RiscvCallable *call = memnew(RiscvCallable);
	call->init(this, address, std::move(args), !this->get_unboxed_arguments());
	return Callable(call);
}
void RiscvCallable::init(Sandbox *self, gaddr_t address, Array args, bool variant_arguments) {
	this->sandbox_id = self != nullptr ? self->get_instance_id() : ObjectID();
	this->tree_base_id = self != nullptr ? self->get_tree_base_id() : ObjectID();
	this->instance_base = self != nullptr ? self->get_instance_base() : gaddr_t(0);
	this->address = address;
	this->m_variant_arguments = variant_arguments;

	for (int i = 0; i < args.size(); i++) {
		m_varargs[i] = args[i];
		m_varargs_ptrs[i] = &m_varargs[i];
	}
	this->m_varargs_base_count = args.size();
}

bool RiscvCallable::is_valid() const {
	Sandbox *self = this->sandbox();
	return self != nullptr && self->is_live_instance_base(this->instance_base);
}

Sandbox *RiscvCallable::sandbox() const {
	if (sandbox_id.is_null()) {
		return nullptr;
	}
	return Object::cast_to<Sandbox>(ObjectDB::get_instance(sandbox_id));
}

void RiscvCallable::call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, GDExtensionCallError &r_call_error) const {
	Sandbox *self = this->sandbox();
	if (self == nullptr) {
		ERR_PRINT("Callable: the Sandbox it belongs to no longer exists");
		r_call_error.error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL;
		return;
	}
	if (!self->is_live_instance_base(this->instance_base)) {
		ERR_PRINT("Callable: the script instance it belongs to no longer exists");
		r_call_error.error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL;
		return;
	}
	const bool varargs = m_varargs_base_count > 0;
	const int total_args = m_varargs_base_count + p_argcount;
	if (total_args > int(gdscript::CallABI::MAX_ARGUMENTS)) {
		ERR_PRINT("Too many arguments for VM function call");
		r_call_error.error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
		r_call_error.argument = p_argcount;
		return;
	}
	if (varargs) {
		for (int i = 0; i < p_argcount; i++) {
			m_varargs_ptrs[m_varargs_base_count + i] = p_arguments[i];
		}
	}

	const bool previous_unboxed = self->get_unboxed_arguments();
	self->set_unboxed_arguments(!m_variant_arguments);

	{
		ScopedTreeBase stb(self, this->tree_base_id.is_valid() ? this->tree_base_id : self->get_tree_base_id());
		ScopedInstanceBase sib(self, this->instance_base);
		if (varargs) {
			r_return_value = self->vmcall_internal(address, m_varargs_ptrs.data(), total_args);
		} else {
			r_return_value = self->vmcall_internal(address, p_arguments, p_argcount);
		}
	}

	self->set_unboxed_arguments(previous_unboxed);
	r_call_error.error = GDEXTENSION_CALL_OK;
}

gaddr_t Sandbox::cached_address_of(int64_t hash, const String &function) const {
	gaddr_t address = 0x0;
	auto it = m_lookup.find(hash);
	if (it != m_lookup.end()) {
		return it->second.address;
	} else if (m_machine != &dummy_machine) {
		const CharString ascii = function.ascii();
		const std::string_view str{ ascii.get_data(), (size_t)ascii.length() };
		address = machine().address_of(str);
		// Cache the address and symbol name
		LookupEntry entry{ function, address };
		m_lookup.insert_or_assign(hash, std::move(entry));
	}
	return address;
}

gaddr_t Sandbox::cached_address_of(const StringName &name) const {
	auto it = m_sname_lookup.find(name);
	if (it != m_sname_lookup.end()) {
		return it->second;
	}
	// Only the first call for a given name pays for the String conversion and its hash.
	const String str(name);
	const gaddr_t address = cached_address_of(str.hash(), str);
	m_sname_lookup.emplace(name, address);
	return address;
}

gaddr_t Sandbox::cached_address_of_variant(const Variant &name) const {
	const GDNativeVariant *inner = (const GDNativeVariant *)name._native_ptr();
	// A StringName has an identity cache of its own, and it is what script calls arrive as.
	if (inner->type == Variant::STRING_NAME) {
		return cached_address_of(*(const StringName *)&inner->value);
	}
	if (UNLIKELY(inner->type != Variant::STRING)) {
		const String str = name.operator String();
		return cached_address_of(str.hash(), str);
	}

	// Read the String in place: copying it out of the Variant is two calls into the engine
	// and two atomic refcount updates, all to look at a pointer.
	const String &str = *(const String *)&inner->value;
	const uintptr_t id = string_cache_key(str);
	NameAddressCache::Entry &entry = m_name_addresses.entries[(id * 0x9E3779B97F4A7C15ull >> 32) & (NameAddressCache::SIZE - 1)];
	if (string_cache_hit(entry.name, str)) {
		return entry.address;
	}

	const gaddr_t address = cached_address_of(str.hash(), str);
	entry.name = str;
	entry.address = address;
	return address;
}

gaddr_t Sandbox::address_of(const String &symbol) const {
	const int64_t hash = symbol.hash();
	return cached_address_of(hash, symbol);
}

String Sandbox::lookup_address(gaddr_t address) const {
	for (const auto &entry : m_lookup) {
		if (entry.second.address == address) {
			return entry.second.name;
		}
	}
	riscv::Memory<RISCV_ARCH>::Callsite callsite = machine().memory.lookup(address);
	return String::utf8(callsite.name.c_str(), callsite.name.size());
}

bool Sandbox::has_function(const StringName &p_function) const {
	return cached_address_of(p_function) != 0x0;
}

PackedStringArray Sandbox::get_functions() const {
	PackedStringArray names;
	names.resize(m_public_api_functions.size());
	for (int i = 0; i < m_public_api_functions.size(); i++) {
		names.set(i, m_public_api_functions[i].operator Dictionary()["name"]);
	}
	return names;
}

void Sandbox::add_public_api_function(Dictionary &&func) {
	const String name = func["name"];
	// Duplicate name replaces the earlier entry.
	for (int i = 0; i < m_public_api_functions.size(); i++) {
		if (m_public_api_functions[i].operator Dictionary()["name"].operator String() == name) {
			m_public_api_functions.remove_at(i);
			break;
		}
	}
	if (m_public_api_functions.size() >= MAX_PUBLIC_FUNCTIONS) {
		ERR_PRINT("Too many public functions in the Sandbox program");
		throw std::runtime_error("Too many public functions in the Sandbox program");
	}
	const gaddr_t address = func.get("address", 0x0);
	m_public_api_functions.push_back(std::move(func));
	// Populate address cache.
	this->add_cached_address(name, address);
}

void Sandbox::add_cached_address(const String &name, gaddr_t address) const {
	m_lookup.insert_or_assign(name.hash(), LookupEntry{ name, address });
	// A guest can register a function after something already looked for it and cached the
	// miss. There is no StringName here to overwrite the matching entry with, and this only
	// runs while the guest publishes its API, so drop the whole cache instead.
	m_sname_lookup.clear();
	m_name_addresses.clear();
}

Sandbox::CachedNameRef Sandbox::cached_guest_name(gaddr_t address, std::string_view name, bool terminated) const {
	// Guest names sit at byte-aligned addresses in .rodata, so neighbouring literals would
	// all land in adjacent slots. Mix the address before folding it into an index.
	const unsigned index = ((address * 2654435761u) >> 8) & (GuestNameCache::SIZE - 1);
	GuestNameCache::Entry &entry = m_guest_names.entries[index];

	if (entry.address == address && entry.terminated == terminated && entry.text.size() == name.size() && guest_memcmp(entry.text.data(), name.data(), name.size()) == 0) {
		return CachedNameRef(entry);
	}

	const auto build_name = [&](CachedName &cached, std::string_view text) {
		if (terminated) {
			std::string terminated_text(text);
			cached.sname = StringName(terminated_text.c_str(), false);
		} else {
			cached.sname = StringName(String::utf8(text.data(), text.size()));
		}
		cached.variant = cached.sname;
	};

	// A nested call that hashes to this slot must not invalidate the name an outer
	// Godot call is still reading. Such collisions are rare and recursion is bounded.
	if (UNLIKELY(entry.pins != 0)) {
		auto uncached = std::make_unique<CachedName>();
		build_name(*uncached, name);
		return CachedNameRef(std::move(uncached));
	}

	// Miss: build the name once and keep it. The two branches mirror how guests pass names:
	// a pointer to a NUL-terminated literal, or a view into a longer string.
	entry = GuestNameCache::Entry{};
	entry.address = address;
	entry.terminated = terminated;
	entry.text.assign(name.data(), name.size());
	build_name(entry.name, entry.text);
	return CachedNameRef(entry);
}

//-- Scoped objects and variants --//

static inline bool permanent_slot_untracked(size_t slot, size_t tracked) noexcept {
	return slot >= tracked;
}

int32_t Sandbox::track_permanent_slot(int32_t variant_index) {
	CurrentState &perm = this->m_states[0];
	// Pad for slots pushed outside track_permanent_slot; records must stay in step.
	while (m_perm_slots.size() + 1 < perm.scoped_variants.size()) {
		m_perm_slots.push_back(PermanentSlot{});
	}
	const uint32_t slot = uint32_t(perm.scoped_variants.size()) - 1u;
	if (m_perm_slots.size() <= slot) {
		m_perm_slots.resize(slot + 1);
	}
	m_perm_slots[slot].variant_index = variant_index;
	m_perm_slots[slot].free = false;
	return encode_permanent_index(slot, m_perm_slots[slot].generation);
}

bool Sandbox::permanent_index_valid(int32_t index) const noexcept {
	if (!is_permanent_variant(index)) {
		return false;
	}
	const uint32_t slot = decode_permanent_slot(index);
	if (slot >= m_states[0].scoped_variants.size()) {
		return false;
	}
	if (permanent_slot_untracked(slot, m_perm_slots.size())) {
		return decode_permanent_generation(index) == 0;
	}
	const PermanentSlot &rec = m_perm_slots[slot];
	return !rec.free && rec.generation == decode_permanent_generation(index);
}


unsigned Sandbox::add_scoped_variant(const Variant *value) const {
	CurrentState &st = this->state();
	if (st.scoped_variants.size() >= st.variants.capacity()) {
		ERR_PRINT("Maximum number of scoped variants reached.");
		throw std::runtime_error("Maximum number of scoped variants reached.");
	}
	st.scoped_variants.push_back(value);
	if (&st != &this->m_states[0])
		return int32_t(st.scoped_variants.size()) - 1;
	// Non-owned: slot is never recycled (variant_index -1).
	return const_cast<Sandbox *>(this)->track_permanent_slot(-1);
}
unsigned Sandbox::create_scoped_variant(Variant &&value) const {
	CurrentState &st = this->state();
	if (st.scoped_variants.size() >= st.variants.capacity()) {
		ERR_PRINT("Maximum number of scoped variants reached.");
		throw std::runtime_error("Maximum number of scoped variants reached.");
	}
	st.append(std::move(value));
	if (&st != &this->m_states[0])
		return int32_t(st.scoped_variants.size()) - 1;
	return const_cast<Sandbox *>(this)->track_permanent_slot(int32_t(st.variants.size()) - 1);
}
std::optional<const Variant *> Sandbox::get_scoped_variant_uncommon(int32_t index) const noexcept {
	if (index >= 0 && size_t(index) < state().scoped_variants.size()) {
		return state().scoped_variants[index];
	} else if (index < 0) {
		// INT32_MIN: -INT32_MIN is UB.
		if (UNLIKELY(index == INT32_MIN)) {
			ERR_PRINT("Invalid permanent variant index: " + itos(index));
			return std::nullopt;
		}
		if (this->permanent_index_valid(index)) {
			return this->m_states[0].scoped_variants[decode_permanent_slot(index)];
		}
		ERR_PRINT("Invalid permanent variant index: " + itos(index));
		return std::nullopt;
	}
	ERR_PRINT("Invalid scoped variant index: " + itos(index));
	return std::nullopt;
}
Variant &Sandbox::get_mutable_scoped_variant(int32_t index) {
	CurrentState *st = &this->state();
	int32_t slot = index;
	if (is_permanent_variant(index)) {
		st = &this->m_states[0];
		if (!permanent_index_valid(index)) {
			ERR_PRINT("Stale or invalid permanent variant index: " + itos(index));
			throw std::runtime_error("Stale or invalid permanent variant index: " + std::to_string(index));
		}
		slot = int32_t(decode_permanent_slot(index));
	}
	if (slot < 0 || size_t(slot) >= st->scoped_variants.size()) {
		ERR_PRINT("Invalid scoped variant index: " + itos(index));
		throw std::runtime_error("Invalid scoped variant index: " + std::to_string(index));
	}
	const Variant *var = st->scoped_variants[slot];
	if (st->is_mutable_variant(*var)) {
		return const_cast<Variant &>(*var);
	}
	// Non-owned Variant (eg. caller argument): copy into this state and re-point the slot.
	if (st->variants.size() >= st->variants.capacity()) {
		ERR_PRINT("Maximum number of scoped variants reached.");
		throw std::runtime_error("Maximum number of scoped variants reached.");
	}
	st->variants.push_back(Variant(*var));
	st->scoped_variants[slot] = &st->variants.back();
	return st->variants.back();
}
unsigned Sandbox::create_permanent_variant(unsigned idx) {
	if (int32_t(idx) < 0) {
		// It's already a permanent variant
		return idx;
	}
	std::optional<const Variant *> var_opt = get_scoped_variant(idx);
	if (!var_opt.has_value()) {
		ERR_PRINT("create_permanent_variant(): Invalid scoped variant index " + itos(idx));
		throw std::runtime_error("Could not make permanent: Invalid scoped variant index " + std::to_string(idx));
	}
	const Variant *var = var_opt.value();
	// Find the variant in the variants list
	auto it = std::find_if(state().variants.begin(), state().variants.end(), [var](const Variant &v) {
		return &v == var;
	});

	Variant value = (it == state().variants.end()) ? var->duplicate() : std::move(*it);
	const int32_t perm_idx = this->create_permanent_variant_from(std::move(value));
	if (perm_idx == 0) {
		return idx;
	}
	return unsigned(perm_idx);
}
int32_t Sandbox::create_permanent_variant_from(Variant &&value) {
	CurrentState &perm_state = this->m_states[0];

	// Recycle a released slot first; stale entries are dropped.
	while (!m_perm_free_slots.empty()) {
		const uint32_t slot = m_perm_free_slots.back();
		m_perm_free_slots.pop_back();
		if (slot >= m_perm_slots.size() || slot >= perm_state.scoped_variants.size()) {
			continue;
		}
		PermanentSlot &rec = m_perm_slots[slot];
		if (!rec.free) {
			continue;
		}
		if (rec.variant_index < 0 || size_t(rec.variant_index) >= perm_state.variants.size()) {
			continue;
		}
		perm_state.variants[rec.variant_index] = std::move(value);
		perm_state.scoped_variants[slot] = &perm_state.variants[rec.variant_index];
		rec.free = false;
		return encode_permanent_index(slot, rec.generation);
	}

	if (perm_state.variants.size() >= perm_state.variants.capacity() ||
		perm_state.scoped_variants.size() >= PERM_MAX_SLOTS) {
		ERR_PRINT("Maximum number of scoped variants in permanent state reached.");
		return 0;
	}
	perm_state.append(std::move(value));
	return this->track_permanent_slot(int32_t(perm_state.variants.size()) - 1);
}
void Sandbox::release_permanent_variant(int32_t idx) {
	if (!this->permanent_index_valid(idx)) {
		return;
	}
	const uint32_t slot = decode_permanent_slot(idx);
	if (slot >= m_perm_slots.size()) {
		return;
	}
	PermanentSlot &rec = m_perm_slots[slot];
	if (rec.variant_index < 0) {
		return;
	}
	CurrentState &perm_state = this->m_states[0];
	perm_state.variants[rec.variant_index] = Variant();
	perm_state.scoped_variants[slot] = &perm_state.variants[rec.variant_index];
	// 15-bit generation; wraps after 32768 recycles of the same slot.
	rec.generation = (rec.generation + 1u) & 0x7FFFu;
	rec.free = true;
	m_perm_free_slots.push_back(slot);
}
void Sandbox::assign_permanent_variant(int32_t idx, Variant &&val) {
	if (this->permanent_index_valid(idx)) {
		const uint32_t slot = decode_permanent_slot(idx);
		CurrentState &perm_state = this->m_states[0];
		const int32_t vi = slot < m_perm_slots.size() ? m_perm_slots[slot].variant_index : -1;
		if (vi >= 0 && size_t(vi) < perm_state.variants.size()) {
			perm_state.variants[vi] = std::move(val);
			perm_state.scoped_variants[slot] = &perm_state.variants[vi];
			return;
		}
		// Untracked or non-owned: take ownership.
		if (perm_state.variants.size() < perm_state.variants.capacity()) {
			perm_state.variants.push_back(std::move(val));
			perm_state.scoped_variants[slot] = &perm_state.variants.back();
			if (slot >= m_perm_slots.size()) {
				m_perm_slots.resize(slot + 1);
			}
			m_perm_slots[slot].variant_index = int32_t(perm_state.variants.size()) - 1;
			return;
		}
	}
	// It's either a scoped (temporary) variant, a stale index, or invalid
	ERR_PRINT("Invalid permanent variant index.");
	throw std::runtime_error("Invalid permanent variant index: " + std::to_string(idx));
}
unsigned Sandbox::try_reuse_assign_variant(int32_t assign_to_idx, Variant &&new_value) {
	if (this->is_permanent_variant(assign_to_idx)) {
		this->assign_permanent_variant(assign_to_idx, std::move(new_value));
		return assign_to_idx;
	}
	CurrentState &st = this->state();
	if (assign_to_idx >= 0 && size_t(assign_to_idx) < st.scoped_variants.size()) {
		const Variant *var = st.scoped_variants[assign_to_idx];
		if (st.is_mutable_variant(*var)) {
			const_cast<Variant &>(*var) = std::move(new_value);
			return assign_to_idx;
		}
	}
	// Slot unset, out of range or non-owned: allocate a new scoped Variant.
	return this->create_scoped_variant(std::move(new_value));
}
unsigned Sandbox::try_reuse_assign_variant(int32_t src_idx, const Variant &src_var, int32_t assign_to_idx, const Variant &new_value) {
	if (!this->is_permanent_variant(assign_to_idx) && assign_to_idx == src_idx && this->state().is_mutable_variant(src_var)) {
		// Same owned slot the operation read from: modify in place.
		const_cast<Variant &>(src_var) = new_value;
		return assign_to_idx;
	}
	return this->try_reuse_assign_variant(assign_to_idx, Variant(new_value));
}

uint64_t Sandbox::engine_object_id(const godot::Object *obj) noexcept {
	if (obj == nullptr || obj->_owner == nullptr)
		return 0;
	return internal::gdextension_interface_object_get_instance_id(obj->_owner);
}

uint64_t Sandbox::engine_ptr_object_id(uintptr_t engine_object) noexcept {
	if (engine_object == 0)
		return 0;
	return internal::gdextension_interface_object_get_instance_id(reinterpret_cast<GDExtensionObjectPtr>(engine_object));
}

void Sandbox::rem_scoped_object(const godot::Object *obj) {
	const uint64_t object_id = engine_object_id(obj);
	auto &objects = state().scoped_objects;
	objects.erase(std::remove_if(objects.begin(), objects.end(),
						  [object_id](const CurrentState::ScopedObject &so) { return so.object_id == object_id; }),
			objects.end());
}

godot::Object *Sandbox::resolve_scoped_object(CurrentState::ScopedObject &so) {
	if (so.binding == nullptr)
		so.binding = internal::get_object_instance_binding(reinterpret_cast<GodotObject *>(so.engine_object));
	return so.binding;
}

godot::Object *Sandbox::resolve_live_object(uint64_t object_id) const noexcept {
	if (object_id == 0)
		return nullptr;
	GDExtensionObjectPtr live = internal::gdextension_interface_object_get_instance_from_id(object_id);
	if (live == nullptr)
		return nullptr;
	ObjectBindingCache::Entry &entry = m_object_bindings.slot(object_id);
	if (entry.object_id == object_id)
		return entry.binding;
	godot::Object *binding = internal::get_object_instance_binding(static_cast<GodotObject *>(live));
	entry.object_id = object_id;
	entry.binding = binding;
	return binding;
}

bool Sandbox::add_scoped_entry(uint64_t object_id, uintptr_t engine_object, godot::Object *binding) {
	// The same object is commonly handed to the guest more than once in a call, eg. when
	// it walks the tree. Scoping it again would burn one of the max_refs slots each time.
	if (CurrentState::ScopedObject *so = this->find_scoped_object(object_id)) {
		if (so->binding == nullptr)
			so->binding = binding;
		return false;
	}
	if (state().scoped_objects.size() >= this->m_max_refs) {
		ERR_PRINT("Maximum number of scoped objects reached.");
		throw std::runtime_error("Maximum number of scoped objects reached.");
	}
	state().scoped_objects.push_back(CurrentState::ScopedObject{ object_id, engine_object, binding });
	return true;
}

bool Sandbox::hold_unrestricted_object(uint64_t object_id, godot::Object *obj) {
	RefCounted *ref = fast_cast_to<RefCounted>(obj);
	if (ref == nullptr)
		return false;
	if (!state().mark_referenced(object_id))
		return false;
	if (state().scoped_refs.size() >= MAX_UNRESTRICTED_REFS) {
		ERR_PRINT("Maximum number of referenced objects reached in a single call.");
		throw std::runtime_error("Maximum number of referenced objects reached in a single call.");
	}
	state().scoped_refs.push_back(Ref<RefCounted>(ref));
	return true;
}

uint64_t Sandbox::add_scoped_engine_object(uintptr_t engine_object) {
	const uint64_t object_id = engine_ptr_object_id(engine_object);
	if (object_id == 0)
		return 0;
	if (!this->is_object_access_unrestricted())
		this->add_scoped_entry(object_id, engine_object, nullptr);
	return object_handle_from_id(object_id);
}

void Sandbox::store_into_guest_slot(gaddr_t slot_address, Variant &&value) {
	GuestVariant *dst = machine().memory.memarray<GuestVariant>(slot_address, 1);

	const int32_t held = dst->is_scoped_variant() ? int32_t(dst->v.i) : 0;
	const bool holds_permanent = Sandbox::is_permanent_variant(held);
	const int32_t previous_type = dst->type;

	GuestVariant next;
	next.type = value.get_type();
	next.v.i = 0;
	if (next.is_scoped_variant()) {
		const int32_t stored = this->create_permanent_variant_from(std::move(value));
		if (UNLIKELY(stored == 0)) {
			next.type = Variant::NIL;
		} else {
			next.v.i = stored;
		}
	} else {
		next.create(*this, std::move(value));
	}

	if (holds_permanent) {
		this->release_permanent_variant(held);
	}
	*dst = next;

	if (dst->type == Variant::OBJECT || previous_type == Variant::OBJECT) {
		this->retain_global_object(slot_address);
	}
}

void Sandbox::retain_global_object(gaddr_t slot_address) {
	uint64_t object_id = 0;
	try {
		const GuestVariant *slot = machine().memory.memarray<GuestVariant>(slot_address, 1);
		if (slot->type == Variant::OBJECT) {
			object_id = object_id_from_handle(uint64_t(slot->v.i));
		}
	} catch (const std::exception &e) {
		ERR_PRINT("Sandbox: could not read a retained object slot: " + String(e.what()));
		return;
	}
	godot::Object *obj = object_id != 0 ? this->resolve_live_object(object_id) : nullptr;
	RefCounted *ref = obj != nullptr ? fast_cast_to<RefCounted>(obj) : nullptr;
	// Release side is unconditional: a ref taken while unrestricted must drop even if
	// restrictions were enabled since.
	if (ref == nullptr || !this->is_object_access_unrestricted()) {
		m_retained_objects.erase(slot_address);
		return;
	}
	m_retained_objects.insert_or_assign(slot_address, Ref<RefCounted>(ref));
}

void Sandbox::release_retained_objects(gaddr_t base, gaddr_t size) {
	if (m_retained_objects.empty()) {
		return;
	}
	for (auto it = m_retained_objects.begin(); it != m_retained_objects.end();) {
		if (it->first >= base && it->first < base + size) {
			it = m_retained_objects.erase(it);
		} else {
			++it;
		}
	}
}

uint64_t Sandbox::add_scoped_object(godot::Object *obj) {
	const uint64_t object_id = engine_object_id(obj);
	if (object_id == 0)
		return 0;
	// A RefCounted often reaches the guest through a temporary Variant, eg. the return
	// value of a method call, which is destroyed as soon as the pointer has been written
	// out. Hold a reference of our own so the guest's handle stays valid for the call.
	if (this->is_object_access_unrestricted()) {
		this->hold_unrestricted_object(object_id, obj);
	} else if (this->add_scoped_entry(object_id, engine_ptr(obj), obj)) {
		if (RefCounted *ref = fast_cast_to<RefCounted>(obj))
			state().scoped_refs.push_back(Ref<RefCounted>(ref));
	}
	return object_handle_from_id(object_id);
}

//-- Properties --//

void Sandbox::read_program_properties(bool editor) const {
	// Array named "properties", terminated by an invalid property.
	const gaddr_t prop_addr = this->m_properties_address;
	if (prop_addr == 0x0)
		return;
	try {
		struct GuestProperty {
			gaddr_t g_name;
			unsigned size;
			Variant::Type type;
			gaddr_t getter;
			gaddr_t setter;
			GuestVariant def_val;
		};
		auto *props = machine().memory.memarray<GuestProperty>(prop_addr, MAX_GUEST_PROPERTY_SLOTS);

		for (int i = 0; i < MAX_GUEST_PROPERTY_SLOTS; i++) {
			const GuestProperty *prop = &props[i];
			// Invalid property: stop reading
			if (prop->g_name == 0)
				break;
			// Check if the property is valid by checking its size
			if (prop->size != sizeof(GuestProperty)) {
				//ERR_PRINT("Sandbox: Invalid property size");
				break;
			}
			const std::string c_name = machine().memory.memstring(prop->g_name);
			Variant def_val = prop->def_val.toVariant(*this);

			this->add_property(String::utf8(c_name.c_str(), c_name.size()), prop->type, prop->setter, prop->getter, def_val);
		}
	} catch (const std::exception &e) {
		ERR_PRINT("Sandbox exception in " + get_name() + " while reading properties: " + String(e.what()));
	}
}

void Sandbox::add_property(const String &name, Variant::Type vtype, uint64_t setter, uint64_t getter, const Variant &def) const {
	if (setter == 0 || getter == 0) {
		ERR_PRINT("Sandbox: Setter and getter not found for property: " + name);
		return;
	} else if (m_properties.size() >= MAX_PROPERTIES) {
		ERR_PRINT("Sandbox: Maximum number of properties reached");
		return;
	}
	for (const String &builtin_name : property_names) {
		if (name.begins_with(builtin_name)) {
			ERR_PRINT("Sandbox: Property name conflicts with built-in property: " + name);
			return;
		}
	}
	for (const SandboxProperty &prop : m_properties) {
		if (prop.name() == name) {
			// TODO: Allow overriding properties?
			//ERR_PRINT("Sandbox: Property already exists: " + name);
			return;
		}
	}
	m_properties.emplace_back(name, vtype, setter, getter, def);

	// Make the property getter/setter functions visible to address_of and profiling
	this->add_cached_address("set_" + name, getter);
	this->add_cached_address("get_" + name, setter);
}
void Sandbox::add_property(const String &name, Variant::Type vtype, gaddr_t address, const Variant &def) const
{
	if (address == 0) {
		ERR_PRINT("Sandbox: Address not found for property: " + name);
		return;
	} else if (m_properties.size() >= MAX_PROPERTIES) {
		ERR_PRINT("Sandbox: Maximum number of properties reached");
		return;
	}
	for (const String &builtin_name : property_names) {
		if (name.begins_with(builtin_name)) {
			ERR_PRINT("Sandbox: Property name conflicts with built-in property: " + name);
			return;
		}
	}
	for (const SandboxProperty &prop : m_properties) {
		if (prop.name() == name) {
			ERR_PRINT("Sandbox: Property already exists: " + name);
			return;
		}
	}
	m_properties.emplace_back(name, vtype, address, def);
}
void Sandbox::set_property_hint(const String &name, uint32_t hint, const String &hint_string, uint32_t usage) const {
	for (SandboxProperty &prop : m_properties) {
		if (prop.name() == name) {
			prop.set_hint(hint, hint_string, usage);
			return;
		}
	}
	ERR_PRINT("Sandbox: No property to hint: " + name);
}

bool Sandbox::set_property(const StringName &name, const Variant &value) {
	for (SandboxProperty &prop : m_properties) {
		if (stringname_equals(prop.name(), name)) {
			//ERR_PRINT("Sandbox: SetProperty *found*: " + name);
			return prop.set(*this, value);
		}
	}
	// Not the most efficient way to do this, but it's (currently) a small list
	if (stringname_equals(name, property_names[PROP_REFERENCES_MAX])) {
		set_max_refs(value);
		return true;
	} else if (stringname_equals(name, property_names[PROP_COROUTINES_MAX])) {
		set_max_coroutines(value);
		return true;
	} else if (stringname_equals(name, property_names[PROP_MEMORY_MAX])) {
		set_memory_max(value);
		return true;
	} else if (stringname_equals(name, property_names[PROP_EXECUTION_TIMEOUT])) {
		set_instructions_max(value);
		return true;
	} else if (stringname_equals(name, property_names[PROP_ALLOCATIONS_MAX])) {
		set_allocations_max(value);
		return true;
	} else if (stringname_equals(name, property_names[PROP_UNBOXED_ARGUMENTS])) {
		set_unboxed_arguments(value);
		return true;
	} else if (stringname_equals(name, property_names[PROP_PRECISE_SIMULATION])) {
		set_precise_simulation(value);
		return true;
	} else if (stringname_equals(name, property_names[PROP_BINTR_NBIT_AS])) {
		set_binary_translation_automatic_nbit_as(value);
		return true;
	} else if (stringname_equals(name, property_names[PROP_BINTR_REG_CACHE])) {
		set_binary_translation_register_caching(value);
		return true;
	} else if (stringname_equals(name, property_names[PROP_PROFILING])) {
		set_profiling(value);
		return true;
	} else if (stringname_equals(name, property_names[PROP_RESTRICTIONS])) {
		set_restrictions(value);
		return true;
	} else if (stringname_equals(name, property_names[PROP_PROGRAM])) {
		set_program(value);
		return true;
	}
	if constexpr (VERBOSE_PROPERTIES) {
		printf("Sandbox: SetProperty *not found*: %s\n", String(name).utf8().get_data());
	}
	return false;
}

bool Sandbox::get_property(const StringName &name, Variant &r_ret) {
	for (const SandboxProperty &prop : m_properties) {
		if (stringname_equals(prop.name(), name)) {
			r_ret = prop.get(*this);
			//ERR_PRINT("Sandbox: GetProperty *found*: " + name);
			return true;
		}
	}
	// Not the most efficient way to do this, but it's (currently) a small list
	if (stringname_equals(name, property_names[PROP_REFERENCES_MAX])) {
		r_ret = get_max_refs();
		return true;
	} else if (stringname_equals(name, property_names[PROP_COROUTINES_MAX])) {
		r_ret = get_max_coroutines();
		return true;
	} else if (stringname_equals(name, property_names[PROP_MEMORY_MAX])) {
		r_ret = get_memory_max();
		return true;
	} else if (stringname_equals(name, property_names[PROP_EXECUTION_TIMEOUT])) {
		r_ret = get_instructions_max();
		return true;
	} else if (stringname_equals(name, property_names[PROP_ALLOCATIONS_MAX])) {
		r_ret = get_allocations_max();
		return true;
	} else if (stringname_equals(name, property_names[PROP_UNBOXED_ARGUMENTS])) {
		r_ret = get_unboxed_arguments();
		return true;
	} else if (stringname_equals(name, property_names[PROP_PRECISE_SIMULATION])) {
		r_ret = get_precise_simulation();
		return true;
	} else if (stringname_equals(name, property_names[PROP_BINTR_NBIT_AS])) {
		r_ret = this->m_bintr_automatic_nbit_as;
		return true;
	} else if (stringname_equals(name, property_names[PROP_BINTR_REG_CACHE])) {
		r_ret = this->m_bintr_register_caching;
		return true;
	} else if (stringname_equals(name, property_names[PROP_PROFILING])) {
		r_ret = get_profiling();
		return true;
	} else if (stringname_equals(name, property_names[PROP_RESTRICTIONS])) {
		r_ret = get_restrictions();
		return true;
	} else if (stringname_equals(name, property_names[PROP_PROGRAM])) {
		r_ret = get_program();
		return true;
	} else if (stringname_equals(name, property_names[PROP_MONITOR_HEAP_USAGE])) {
		r_ret = get_heap_usage();
		return true;
	} else if (stringname_equals(name, property_names[PROP_MONITOR_HEAP_CHUNK_COUNT])) {
		r_ret = get_heap_chunk_count();
		return true;
	} else if (stringname_equals(name, property_names[PROP_MONITOR_HEAP_ALLOCATION_COUNTER])) {
		r_ret = get_heap_allocation_counter();
		return true;
	} else if (stringname_equals(name, property_names[PROP_MONITOR_HEAP_DEALLOCATION_COUNTER])) {
		r_ret = get_heap_deallocation_counter();
		return true;
	} else if (stringname_equals(name, property_names[PROP_MONITOR_EXCEPTIONS])) {
		r_ret = get_exceptions();
		return true;
	} else if (stringname_equals(name, property_names[PROP_MONITOR_EXECUTION_TIMEOUTS])) {
		r_ret = get_timeouts();
		return true;
	} else if (stringname_equals(name, property_names[PROP_MONITOR_CALLS_MADE])) {
		r_ret = get_calls_made();
		return true;
	} else if (stringname_equals(name, property_names[PROP_MONITOR_BINARY_TRANSLATED])) {
		r_ret = is_binary_translated();
		return true;
	} else if (stringname_equals(name, property_names[PROP_GLOBAL_CALLS_MADE])) {
		r_ret = get_global_calls_made();
		return true;
	} else if (stringname_equals(name, property_names[PROP_GLOBAL_EXCEPTIONS])) {
		r_ret = get_global_exceptions();
		return true;
	} else if (stringname_equals(name, property_names[PROP_GLOBAL_TIMEOUTS])) {
		r_ret = get_global_timeouts();
		return true;
	} else if (stringname_equals(name, property_names[PROP_MONITOR_ACCUMULATED_STARTUP_TIME])) {
		r_ret = get_accumulated_startup_time();
		return true;
	} else if (stringname_equals(name, property_names[PROP_MONITOR_GLOBAL_INSTANCE_COUNT])) {
		r_ret = get_global_instance_count();
		return true;
	}
	if constexpr (VERBOSE_PROPERTIES) {
		printf("Sandbox: GetProperty *not found*: %s\n", String(name).utf8().get_data());
	}
	return false;
}

const SandboxProperty *Sandbox::find_property_or_null(const StringName &name) const {
	for (const SandboxProperty &prop : m_properties) {
		if (stringname_equals(prop.name(), name)) {
			return &prop;
		}
	}
	return nullptr;
}

Variant Sandbox::get(const StringName &name) {
	Variant result;
	if (get_property(name, result)) {
		return result;
	}
	// Get as if it's on the underlying Node object
	return Node::get(name);
}

void Sandbox::set(const StringName &name, const Variant &value) {
	if (!set_property(name, value)) {
		// Set as if it's on the underlying Node object
		Node::set(name, value);
	}
}

Array Sandbox::get_property_list() const {
	Array arr;
	// Sandboxed properties
	for (const SandboxProperty &prop : m_properties) {
		Dictionary d;
		d["name"] = prop.name();
		d["type"] = prop.type();
		d["hint"] = prop.hint();
		d["hint_string"] = prop.hint_string();
		uint32_t usage = prop.usage() != 0
			? prop.usage()
			: uint32_t(PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_SCRIPT_VARIABLE);
		if (prop.type() == Variant::NIL) {
			usage |= uint32_t(PROPERTY_USAGE_NIL_IS_VARIANT);
		} else {
			usage &= ~uint32_t(PROPERTY_USAGE_NIL_IS_VARIANT);
		}
		d["usage"] = usage;
		arr.push_back(d);
	}
	// Our properties
	for (const PropertyInfo &prop : this->create_sandbox_property_list()) {
		Dictionary d;
		d["name"] = prop.name;
		d["type"] = prop.type;
		d["usage"] = PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_SCRIPT_VARIABLE;
		arr.push_back(d);
	}
	// Node properties
	arr.append_array(Node::get_property_list());
	return arr;
}

bool SandboxProperty::set(Sandbox &sandbox, const Variant &value) {
	Variant narrowed = value;
	if (!coerce_variant_to(narrowed, m_type)) {
		return false;
	}

	if (m_setter_address == 0) {
		if (m_address != 0) {
			sandbox.store_into_guest_slot(sandbox.rebase_instance_address(m_address),
					std::move(narrowed));
			return true;
		}
		ERR_PRINT("Sandbox: Setter was invalid for property: " + m_name);
		return false;
	}
	const Variant *args[] = { &narrowed };
	// Store unboxed_arguments state and restore it after the call
	// It's much more convenient to use Variant arguments for properties
	auto old_unboxed_arguments = sandbox.get_unboxed_arguments();
	sandbox.set_unboxed_arguments(false);
	sandbox.vmcall_internal(m_setter_address, args, 1);
	sandbox.set_unboxed_arguments(old_unboxed_arguments);
	return true;
}

Variant SandboxProperty::get(const Sandbox &sandbox) const {
	if (m_getter_address == 0) {
		if (m_address != 0) {
			const uint64_t address = sandbox.rebase_instance_address(m_address);
			GuestVariant *g_prop = sandbox.machine().memory.memarray<GuestVariant>(address, 1);
			return g_prop->toVariant(sandbox);
		}
		ERR_PRINT("Sandbox: Getter was invalid for property: " + m_name);
		return Variant();
	}
	return const_cast<Sandbox &>(sandbox).vmcall_internal(m_getter_address, nullptr, 0);
}

void Sandbox::CurrentState::reinitialize(unsigned level, unsigned max_refs) {
	if (level <= 1)
		this->variants.reserve(max_refs);
	this->variants.clear();
	this->scoped_objects.clear();
	this->scoped_variants.clear();
	this->scoped_refs.clear();
	this->clear_referenced();
}
bool Sandbox::CurrentState::is_mutable_variant(const Variant &var) const {
	// Check if the address of the variant is within the range of the current state std::vector.
	// data() rather than &variants[0]: indexing an empty vector is undefined even when the
	// result is only ever used as an address, and this is reached with no Variants scoped.
	const Variant *ptr = &var;
	const Variant *begin = variants.data();
	return ptr >= begin && ptr < begin + variants.size();
}

void Sandbox::set_max_refs(uint32_t max) {
	if (this->is_in_vmcall()) {
		ERR_PRINT("Sandbox: Cannot change max references during a Sandbox call.");
		return;
	}
	this->m_max_refs = max;
	for (size_t i = 1; i < this->m_states.size(); i++) {
		this->m_states[i].reinitialize(i, max);
	}
	// Permanent state (m_states[0]) holds guest globals; grow without clearing.
	this->reserve_permanent_state(max);
}

void Sandbox::reserve_permanent_state(uint32_t max_refs) {
	// 16-bit slot indices; clamp to PERM_MAX_SLOTS.
	if (max_refs > PERM_MAX_SLOTS) {
		max_refs = PERM_MAX_SLOTS;
	}
	CurrentState &perm = this->m_states[0];
	if (max_refs <= perm.variants.capacity()) {
		return;
	}
	perm.variants.reserve(max_refs);
	// Reallocation invalidated scoped_variant pointers; rebuild from m_perm_slots.
	for (size_t slot = 0; slot < perm.scoped_variants.size() && slot < m_perm_slots.size(); slot++) {
		const int32_t vi = m_perm_slots[slot].variant_index;
		if (vi >= 0 && size_t(vi) < perm.variants.size()) {
			perm.scoped_variants[slot] = &perm.variants[vi];
		}
	}
}

void Sandbox::set_allocations_max(int64_t max) {
	this->m_allocations_max = max;
	if (machine().has_arena()) {
		machine().arena().set_max_chunks(max);
	}
}

int64_t Sandbox::get_heap_usage() const {
	if (machine().has_arena()) {
		return machine().arena().bytes_used();
	}
	return 0;
}

int64_t Sandbox::get_heap_chunk_count() const {
	if (machine().has_arena()) {
		return machine().arena().chunks_used();
	}
	return 0;
}

int64_t Sandbox::get_heap_allocation_counter() const {
	if (machine().has_arena()) {
		return machine().arena().allocation_counter();
	}
	return 0;
}

int64_t Sandbox::get_heap_deallocation_counter() const {
	if (machine().has_arena()) {
		return machine().arena().deallocation_counter();
	}
	return 0;
}

// One print() call holds the text of every one of its arguments at once, which
// is the one place in the guest print path where host memory scales with the
// argument count rather than with a single argument. Past this the line is cut
// short: the guest keeps running and gets told, instead of the host growing a
// buffer to whatever size the guest picked.
static constexpr int PRINT_LINE_MAX_CHARS = 1 << 18;

void Sandbox::print(const Variant *const *args, unsigned count, Print_Channel channel) {
	// The latch covers the conversion, not just the output. Stringifying an
	// argument runs Variant::operator String(), which for an Object reaches
	// Object::to_string() and from there a script's _to_string() - and when that
	// script is itself a Sandbox, the guest is running again inside this call and
	// can reach print() a second time. Godot's print() concatenates before it
	// emits anything, so the conversion sits between the latch and the output
	// and has to be inside it.
	static bool already_been_here = false;
	if (already_been_here) {
		ERR_PRINT("Recursive call to Sandbox::print() detected, ignoring.");
		return;
	}
	already_been_here = true;
	// Re-entering the guest can throw straight back out through here. Releasing
	// the latch on the way out keeps one faulting _to_string() from silencing
	// every print() for the rest of the process.
	struct LatchGuard {
		bool &flag;
		~LatchGuard() { flag = false; }
	} latch_guard{ already_been_here };

	// Separator: spaces for prints(), tabs for printt(), empty otherwise.
	const char *separator = channel == Print_Channel::SPACED ? " "
			: (channel == Print_Channel::TABBED ? "\t" : "");

	String line;
	for (unsigned i = 0; i < count; i++) {
		if (i > 0) {
			line += separator;
		}
		line += args[i]->stringify();
		if (line.length() > PRINT_LINE_MAX_CHARS) {
			ERR_PRINT("print(): Line too long, truncated");
			line = line.substr(0, PRINT_LINE_MAX_CHARS);
			break;
		}
	}

	// Redirect captures all channels.
	if (this->m_redirect_stdout.is_valid()) {
		this->m_redirect_stdout.call(line);
		return;
	}

	switch (channel) {
		case Print_Channel::PRINT:
		case Print_Channel::SPACED:
		case Print_Channel::TABBED:
			UtilityFunctions::print(line);
			break;
		case Print_Channel::RAW:
			UtilityFunctions::printraw(line);
			break;
		case Print_Channel::RICH:
			UtilityFunctions::print_rich(line);
			break;
		case Print_Channel::ERROR:
			UtilityFunctions::printerr(line);
			break;
		case Print_Channel::VERBOSE:
			UtilityFunctions::print_verbose(line);
			break;
		case Print_Channel::PUSH_ERROR:
			UtilityFunctions::push_error(line);
			break;
		case Print_Channel::PUSH_WARNING:
			UtilityFunctions::push_warning(line);
			break;
		case Print_Channel::CHANNEL_COUNT:
			break;
	}
}

void Sandbox::print(const Variant &v) {
	const Variant *args[1] = { &v };
	this->print(args, 1);
}

bool Sandbox::is_sandbox_function(const StringName &p_function) const {
	// Only functions listed in sandbox.h and public:
	static const HashSet<StringName> sandbox_functions = {
		"vmcall",
		"vmcall_address",
		"vmcallable",
		"vmcallable_address",
		"get_program",
		"set_program",
		"has_function",
		"address_of",
		"lookup_address",
		"get_max_refs",
		"set_max_refs",
		"get_memory_max",
		"set_memory_max",
		"get_instructions_max",
		"set_instructions_max",
		"get_allocations_max",
		"set_allocations_max",
		"get_max_coroutines",
		"set_max_coroutines",
		"get_coroutine_count",
		"reap_coroutines",
		"get_unboxed_arguments",
		"set_unboxed_arguments",
		"get_precise_simulation",
		"set_precise_simulation",
		"get_profiling",
		"set_profiling",
		"get_restrictions",
		"set_restrictions",
		"get_unchecked_memory",
		"get_exceptions",
		"get_timeouts",
		"get_calls_made",
		"is_binary_translated",
		"get_global_calls_made",
		"get_global_exceptions",
		"get_global_timeouts",
		"get_accumulated_startup_time",
		"get_global_instance_count",

		"add_allowed_object",
		"remove_allowed_object",
		"clear_allowed_objects",

		"set_object_allowed_callback",
		"is_allowed_object",
		"set_class_allowed_callback",
		"is_allowed_class",
		"is_class_access_restricted",
		"set_resource_allowed_callback",
		"is_allowed_resource",
		"set_method_allowed_callback",
		"is_allowed_method",
		"set_property_allowed_callback",
		"is_allowed_property",

		"get_hotspots",
		"clear_hotspots",
		"get_redirect_stdout",
		"set_redirect_stdout",

		"set_jit_enabled",
		"is_jit_enabled",
		"has_feature_jit",
	};

	return sandbox_functions.has(p_function);
}
