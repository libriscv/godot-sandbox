#include "sandbox.hpp"

#include <godot_cpp/core/class_db.hpp>

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#ifdef RISCV_BINARY_TRANSLATION
#include <thread> // TODO: Use Godot's threading API.
#endif

using namespace godot;

// There are two APIs:
// 1. The API that always makes sense, eg. Timers, Nodes, etc.
//    This will be using system calls. Assign a fixed-number.
// 2. The Game-specific API, eg. whatever bullshit you need for your game to be fucking awesome.
//    (will be implemented later)

static constexpr size_t MAX_HEAP = 16ull << 20;
static const int HEAP_SYSCALLS_BASE = 570;
static const int MEMORY_SYSCALLS_BASE = 575;
static const int THREADS_SYSCALL_BASE = 590;

String Sandbox::_to_string() const {
	return "[ GDExtension::Sandbox <--> Instance ID:" + uitos(get_instance_id()) + " ]";
}

void Sandbox::_bind_methods() {
	// Methods.
	ClassDB::bind_method(D_METHOD("get_program"), &Sandbox::get_program);
	ClassDB::bind_method(D_METHOD("set_program", "program"), &Sandbox::set_program);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "program", PROPERTY_HINT_RESOURCE_TYPE, "ELFResource"), "set_program", "get_program");
	ClassDB::bind_method(D_METHOD("get_functions"), &Sandbox::get_functions);
	{
		MethodInfo mi;
		mi.arguments.push_back(PropertyInfo(Variant::STRING, "function"));
		mi.name = "vmcall";
		mi.return_val = PropertyInfo(Variant::OBJECT, "result");
		ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "vmcall", &Sandbox::vmcall, mi, DEFVAL(std::vector<Variant>{}));
	}
	ClassDB::bind_method(D_METHOD("vmcallable"), &Sandbox::vmcallable);
}

Sandbox::Sandbox() {
	// In order to reduce checks we guarantee that this
	// class is well-formed at all times.
	this->m_machine = new machine_t{};
	UtilityFunctions::print("Constructor, sizeof(Variant) == ", static_cast<int32_t>(sizeof(Variant)));
	UtilityFunctions::print("Constructor, alignof(Variant) == ", static_cast<int32_t>(alignof(Variant)));
}

Sandbox::~Sandbox() {
	UtilityFunctions::print("Destructor.");
	delete this->m_machine;
}

// Methods.

void Sandbox::set_program(Ref<ELFResource> program) {
	m_program_data = program;
	if (m_program_data.is_null()) {
		// TODO unload program
		return;
	}
	PackedByteArray data = m_program_data->get_content();
	if (Engine::get_singleton()->is_editor_hint())
		return;
	load(data, m_program_arguments);
}
Ref<ELFResource> Sandbox::get_program() {
	return m_program_data;
}
void Sandbox::load(Variant vbuf, const TypedArray<String> &arguments) {
	auto buffer = vbuf.operator godot::PackedByteArray();
	if (buffer.is_empty()) {
		UtilityFunctions::print("Empty binary, cannot load program.");
		return;
	}

	m_binary = std::vector<uint8_t>{ buffer.ptr(), buffer.ptr() + buffer.size() };

	try {
		delete this->m_machine;

		const riscv::MachineOptions<RISCV_ARCH> options{
			//.verbose_loader = true,
			.default_exit_function = "fast_exit",
#ifdef RISCV_BINARY_TRANSLATION
			.translate_background_callback =
					[](auto &compilation_step) {
						// TODO: Use Godot's threading API.
						std::thread([compilation_step = std::move(compilation_step)] {
							compilation_step();
						}).detach();
					}
#endif
		};

		this->m_machine = new machine_t{ this->m_binary, options };
		machine_t &m = machine();

		m.set_userdata(this);

		this->initialize_syscalls();

		const auto heap_area = machine().memory.mmap_allocate(MAX_HEAP);

		// Add native system call interfaces
		machine().setup_native_heap(HEAP_SYSCALLS_BASE, heap_area, MAX_HEAP);
		machine().setup_native_memory(MEMORY_SYSCALLS_BASE);
		machine().setup_native_threads(THREADS_SYSCALL_BASE);

		std::vector<std::string> args;
		for (size_t i = 0; i < arguments.size(); i++) {
			args.push_back(arguments[i].operator String().utf8().get_data());
		}
		if (args.empty()) {
			// Create at least one argument (as required by main)
			args.push_back("program");
		}
		m.setup_linux(args);

		m.simulate(MAX_INSTRUCTIONS);
	} catch (const std::exception &e) {
		this->handle_exception(machine().cpu.pc());
		// TODO: Program failed to load.
	}
}

PackedStringArray Sandbox::get_functions() {
	static std::string exclude_functions[] = { "btree_node_update_separator_after_split", "frame_downheap", "version_lock_lock_exclusive", "read_encoded_value_with_base", "fde_unencoded_extract", "fde_radixsort", "fde_unencoded_compare", "version_lock_unlock_exclusive", "btree_allocate_node", "btree_handle_root_split.part.0", "btree_insert.isra.0", "btree_release_tree_recursively", "btree_destroy", "get_cie_encoding", "btree_remove", "fde_mixed_encoding_extract", "classify_object_over_fdes", "get_pc_range", "fde_single_encoding_extract", "fde_single_encoding_compare", "fde_mixed_encoding_compare", "add_fdes.isra.0", "linear_search_fdes", "release_registered_frames", "deregister_tm_clones", "register_tm_clones", "frame_dummy", "d_make_comp", "d_number", "d_call_offset", "d_discriminator", "d_count_templates_scopes", "d_pack_length", "d_index_template_argument.part.0", "d_growable_string_callback_adapter", "next_is_type_qual.isra.0", "d_append_char", "d_lookup_template_argument", "d_find_pack", "d_append_string", "d_template_param", "d_append_num", "d_print_lambda_parm_name", "d_source_name", "d_substitution", "d_maybe_module_name", "d_type", "d_cv_qualifiers", "d_function_type", "d_name", "d_expression_1", "d_template_args_1", "d_parmlist", "d_bare_function_type", "d_template_parm", "d_template_head", "d_operator_name", "d_unqualified_name", "d_exprlist", "d_prefix", "d_expr_primary", "d_special_name", "d_encoding.part.0", "d_template_arg", "d_print_comp_inner", "d_print_comp", "d_print_mod", "d_print_array_type", "d_print_expr_op", "d_print_subexpr", "d_maybe_print_fold_expression", "d_maybe_print_designated_init", "d_print_function_type", "d_print_mod_list", "d_demangle_callback.constprop.0", "init_dwarf_reg_size_table", "uw_install_context_1", "read_encoded_value", "execute_stack_op", "uw_update_context_1", "execute_cfa_program_specialized", "execute_cfa_program_generic", "uw_frame_state_for", "uw_init_context_1", "call_fini", "check_one_fd", "new_do_write", "mmap_remap_check", "decide_maybe_mmap", "flush_cleanup", "save_for_backup", "enlarge_userbuf", "malloc_printerr", "int_mallinfo", "alloc_perturb", "munmap_chunk", "detach_arena.part.0", "unlink_chunk.isra.0", "malloc_consolidate", "ptmalloc_init.part.0", "alloc_new_heap", "new_heap", "arena_get2", "arena_get_retry", "sysmalloc_mmap_fallback.constprop.0", "sysmalloc_mmap.isra.0", "systrim.constprop.0", "sysmalloc", "tcache_init.part.0", "two_way_long_needle", "next_line", "read_sysfs_file", "get_nproc_stat", "do_tunable_update_val", "tunable_initialize", "parse_tunables", "elf_machine_matches_host", "free_derivation", "free_modules_db", "derivation_compare", "find_derivation", "insert_module", "add_module", "add_alias2.part.0", "read_conf_file.isra.0", "find_module_idx", "find_module.constprop.0", "known_compare", "do_release_shlib", "do_release_all", "new_composite_name", "free_category", "plural_eval", "transcmp", "alias_compare", "read_alias_file", "do_swap", "msort_with_tmp.part.0", "indirect_msort_with_tmp", "read_int", "outstring_converted_wide_string", "group_number", "printf_positional", "read_int", "save_for_wbackup", "adjust_wide_data", "clear_once_control", "opendir_tail", "trecurse", "trecurse_r", "tdestroy_recurse", "maybe_split_for_insert.isra.0", "fatal_error", "length_mismatch", "is_dst", "is_trusted_path_normalize", "add_path.constprop.0.isra.0", "add_name_to_object.isra.0", "open_verify.constprop.0", "open_path.isra.0", "elf_machine_matches_host", "expand_dynamic_string_token", "fillin_rpath.isra.0", "decompose_rpath", "check_match", "do_lookup_x", "elf_machine_matches_host", "elf_machine_matches_host", "search_cache", "do_dlopen", "dlerror_run", "do_dlsym_private", "do_dlsym", "do_dlvsym", "do_dlclose", "free_slotinfo", "gconv_parse_code", "hack_digit", "two_way_long_needle", "remove_slotinfo", "call_dl_init", "add_to_global_update", "dl_open_worker", "dl_open_worker_begin", "add_to_global_resize_failure.isra.0", "add_to_global_resize", "elf_machine_matches_host", "dfs_traversal.part.0", "dlinfo_doit", "dlmopen_doit", "dlopen_doit", "dlsym_doit", "dlvsym_doit", "openaux", "call_init", "check_match", "call_dl_lookup", "do_sym", "base_of_encoded_value", "read_encoded_value_with_base", "stpcpy", "tsearch", "clock_gettime", "secure_getenv", "strcpy", "unsetenv", "gsignal", "bcmp", "setjmp", "getrlimit", "ioctl", "dlerror", "fstatat", "strtok_r", "dlinfo", "sysconf", "vsprintf", "getdtablesize", "fdopendir", "strtoull_l", "pthread_rwlock_rdlock", "memrchr", "strndup", "argz_add_sep", "stat64", "memmove", "pthread_rwlock_init", "getauxval", "snprintf", "munmap", "sched_getparam", "malloc_stats", "strtoimax", "getenv", "wcslen", "memmem", "wmempcpy", "sys_veval", "getpid", "getpagesize", "arc4random", "qsort", "dlmopen", "getrlimit64", "memcpy", "strtoll_l", "rewinddir", "sys_vcall", "malloc", "isatty", "openat64", "sched_setscheduler", "sysinfo", "vsnprintf", "strtoll", "register_printf_modifier", "strtoumax", "strtoul", "sched_getscheduler", "pthread_rwlock_unlock", "readdir", "pvalloc", "dladdr", "lseek", "wmemcpy", "clearenv", "mmap", "strtol_l", "dlclose", "abort", "sys_print", "fstat64", "tdelete", "strtoq", "strtol", "strnlen", "strrchr", "calloc", "strcasecmp_l", "malloc_usable_size", "tdestroy", "rindex", "write", "dladdr1", "wmemchr", "fstat", "dcgettext", "pthread_once", "madvise", "mremap", "getdelim", "memchr", "tfind", "lstat", "strstr", "pthread_kill", "read", "getentropy", "strncmp", "dlopen", "wcsrtombs", "getdents64", "pthread_rwlock_wrlock", "get_avphys_pages", "setenv", "sched_get_priority_max", "funlockfile", "pthread_sigmask", "realloc", "readdir64", "wcsnlen", "register_printf_specifier", "memcmp", "sched_get_priority_min", "malloc_trim", "lstat64", "sigaction", "twalk_r", "dlsym", "sbrk", "strdup", "strtoull", "index", "fopen", "memset", "get_phys_pages", "mallinfo2", "mallopt", "fclose", "open64", "malloc_info", "tcgetattr", "opendir", "strcmp", "pthread_mutex_unlock", "register_printf_function", "strtoul_l", "getcwd", "fstatat64", "memalign", "asprintf", "strerror_r", "strcspn", "setlocale", "mmap64", "strsep", "valloc", "dlvsym", "fputc", "register_printf_type", "mallinfo", "pthread_self", "twalk", "argz_create_sep", "stat", "fast_exit", "wmemmove", "fwrite", "qsort_r", "strtouq", "pthread_mutex_lock", "gettext", "get_nprocs", "exit", "brk", "openat", "fgets_unlocked", "strspn", "strlen", "lseek64", "open", "strchr", "arc4random_buf", "fputs", "mprotect", "closedir", "vasprintf", "strchrnul", "get_nprocs_conf", "aligned_alloc", "posix_memalign", "wcrtomb", "close", "raise", "free", "sigprocmask", "fopen64" };
	PackedStringArray array;
	for (auto &f : machine().memory.all_unmangled_function_symbols()) {
		bool exclude_function = false;
		for (auto &excluded_function : exclude_functions) {
			if (f == excluded_function) {
				exclude_function = true;
				break;
			}
		}
		if (!exclude_function) {
			array.append(String(std::string(f).c_str()));
		}
	}
	return array;
}

Variant Sandbox::vmcall_address(gaddr_t address, const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error) {
	error.error = GDEXTENSION_CALL_OK;
	return this->vmcall_internal(address, args, arg_count);
}
Variant Sandbox::vmcall(const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error) {
	if (arg_count < 1) {
		error.error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
		return Variant();
	}
	auto &function = *args[0];
	args += 1;
	arg_count -= 1;
	error.error = GDEXTENSION_CALL_OK;
	return this->vmcall_internal(cached_address_of(String(function)), args, arg_count);
}
void Sandbox::setup_arguments(const Variant **args, int argc) {
	// Stack pointer
	auto &sp = m_machine->cpu.reg(2);
	sp -= sizeof(GuestVariant) * argc;
	const auto arrayDataPtr = sp;
	const auto arrayElements = argc;

	GuestVariant *v = m_machine->memory.memarray<GuestVariant>(arrayDataPtr, arrayElements);

	for (size_t i = 0; i < argc; i++) {
		//printf("args[%zu] = type %d\n", i, int(args[i]->get_type()));
		v[i].set(*this, (*args)[i]);
	}
	sp &= ~gaddr_t(0xF); // align stack pointer

	// Set up a std::span of GuestVariant
	m_machine->cpu.reg(10) = arrayDataPtr;
	m_machine->cpu.reg(11) = arrayElements;
}
Variant Sandbox::vmcall_internal(gaddr_t address, const Variant **args, int argc) {
	try {
		m_level++;
		// execute guest function
		if (m_level == 1) {
			// reset the stack pointer to an initial location (deliberately)
			m_machine->cpu.reset_stack_pointer();
			// setup calling convention
			m_machine->setup_call();
			// set up each argument
			this->setup_arguments(args, argc);
			// execute!
			m_machine->simulate_with(MAX_INSTRUCTIONS, 0u, address);
		} else if (m_level > 1 && m_level < MAX_LEVEL) {
			riscv::Registers<RISCV_ARCH> regs;
			regs = m_machine->cpu.registers();
			// we need to make some stack room
			m_machine->cpu.reg(riscv::REG_SP) -= 16u;
			// setup calling convention
			m_machine->setup_call();
			// set up each argument
			this->setup_arguments(args, argc);
			// execute!
			return m_machine->cpu.preempt_internal(regs, true, address, MAX_INSTRUCTIONS);
		} else {
			throw std::runtime_error("Recursion level exceeded");
		}

		m_level--;
		m_scoped_variants.clear();
		return m_machine->return_value<int64_t>();
	} catch (const std::exception &e) {
		m_level--;
		m_scoped_variants.clear();
		this->handle_exception(address);
	}
	return -1;
}
Variant Sandbox::vmcallable(String function) {
	const auto address = cached_address_of(function);

	auto *call = memnew(RiscvCallable);
	call->init(this, address);
	return Callable(call);
}
void RiscvCallable::call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, GDExtensionCallError &r_call_error) const {
	auto result = self->vmcall_internal(address, p_arguments, p_argcount);

	r_return_value = result;
	r_call_error.error = GDEXTENSION_CALL_OK;
}

void Sandbox::handle_exception(gaddr_t address) {
	auto callsite = machine().memory.lookup(address);
	UtilityFunctions::print(
			"[", get_name(), "] Exception when calling:\n  ", callsite.name.c_str(), " (0x",
			String("%x").format(callsite.address), ")\n", "Backtrace:\n");
	//this->print_backtrace(address);

	try {
		throw; // re-throw
	} catch (const riscv::MachineTimeoutException &e) {
		this->handle_timeout(address);
		return; // NOTE: might wanna stay
	} catch (const riscv::MachineException &e) {
		const String instr(machine().cpu.current_instruction_to_string().c_str());
		const String regs(machine().cpu.registers().to_string().c_str());

		UtilityFunctions::print(
				"\nException: ", e.what(), "  (data: ", String("%x").format(e.data()), ")\n",
				">>> ", instr, "\n",
				">>> Machine registers:\n[PC\t", String("%x").format(machine().cpu.pc()),
				"] ", regs, "\n");
	} catch (const std::exception &e) {
		UtilityFunctions::print("\nMessage: ", e.what(), "\n\n");
		ERR_PRINT(("Exception: " + std::string(e.what())).c_str());
	}
	UtilityFunctions::print(
			"Program page: ", machine().memory.get_page_info(machine().cpu.pc()).c_str(),
			"\n");
	UtilityFunctions::print(
			"Stack page: ", machine().memory.get_page_info(machine().cpu.reg(2)).c_str(),
			"\n");
}

void Sandbox::handle_timeout(gaddr_t address) {
	this->m_budget_overruns++;
	auto callsite = machine().memory.lookup(address);
	UtilityFunctions::print(
			"Sandbox: Timeout for '", callsite.name.c_str(),
			"' (Timeouts: ", m_budget_overruns, "\n");
}

void Sandbox::print(std::string_view text) {
	String str(static_cast<std::string>(text).c_str());
	if (this->m_last_newline) {
		UtilityFunctions::print("[", get_name(), "] says: ", str);
	} else {
		UtilityFunctions::print(str);
	}
	this->m_last_newline = (text.back() == '\n');
}

gaddr_t Sandbox::cached_address_of(String function) const {
	const auto ascii = function.ascii();
	const std::string str{ ascii.get_data(), (size_t)ascii.length() };

	// lookup function address
	gaddr_t address = 0x0;
	auto it = m_lookup.find(str);
	if (it == m_lookup.end()) {
		address = address_of(str);
		m_lookup.insert_or_assign(str, address);
	} else {
		address = it->second;
	}

	return address;
}

gaddr_t Sandbox::address_of(std::string_view name) const {
	return machine().address_of(name);
}
