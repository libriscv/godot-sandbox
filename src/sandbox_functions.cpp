#include "sandbox.hpp"

#include <unordered_set>

using namespace godot;
static const std::unordered_set<std::string_view> exclude_functions{ "main", "btree_node_update_separator_after_split", "frame_downheap", "version_lock_lock_exclusive", "read_encoded_value_with_base", "fde_unencoded_extract", "fde_radixsort", "fde_unencoded_compare", "version_lock_unlock_exclusive", "btree_allocate_node", "btree_handle_root_split.part.0", "btree_insert.isra.0", "btree_release_tree_recursively", "btree_destroy", "get_cie_encoding", "btree_remove", "fde_mixed_encoding_extract", "classify_object_over_fdes", "get_pc_range", "fde_single_encoding_extract", "fde_single_encoding_compare", "fde_mixed_encoding_compare", "add_fdes.isra.0", "linear_search_fdes", "release_registered_frames", "deregister_tm_clones", "register_tm_clones", "frame_dummy", "d_make_comp", "d_number", "d_call_offset", "d_discriminator", "d_count_templates_scopes", "d_pack_length", "d_index_template_argument.part.0", "d_growable_string_callback_adapter", "next_is_type_qual.isra.0", "d_append_char", "d_lookup_template_argument", "d_find_pack", "d_append_string", "d_template_param", "d_append_num", "d_print_lambda_parm_name", "d_source_name", "d_substitution", "d_maybe_module_name", "d_type", "d_cv_qualifiers", "d_function_type", "d_name", "d_expression_1", "d_template_args_1", "d_parmlist", "d_bare_function_type", "d_template_parm", "d_template_head", "d_operator_name", "d_unqualified_name", "d_exprlist", "d_prefix", "d_expr_primary", "d_special_name", "d_encoding.part.0", "d_template_arg", "d_print_comp_inner", "d_print_comp", "d_print_mod", "d_print_array_type", "d_print_expr_op", "d_print_subexpr", "d_maybe_print_fold_expression", "d_maybe_print_designated_init", "d_print_function_type", "d_print_mod_list", "d_demangle_callback.constprop.0", "init_dwarf_reg_size_table", "uw_install_context_1", "read_encoded_value", "execute_stack_op", "uw_update_context_1", "execute_cfa_program_specialized", "execute_cfa_program_generic", "uw_frame_state_for", "uw_init_context_1", "call_fini", "check_one_fd", "new_do_write", "mmap_remap_check", "decide_maybe_mmap", "flush_cleanup", "save_for_backup", "enlarge_userbuf", "malloc_printerr", "int_mallinfo", "alloc_perturb", "munmap_chunk", "detach_arena.part.0", "unlink_chunk.isra.0", "malloc_consolidate", "ptmalloc_init.part.0", "alloc_new_heap", "new_heap", "arena_get2", "arena_get_retry", "sysmalloc_mmap_fallback.constprop.0", "sysmalloc_mmap.isra.0", "systrim.constprop.0", "sysmalloc", "tcache_init.part.0", "two_way_long_needle", "next_line", "read_sysfs_file", "get_nproc_stat", "do_tunable_update_val", "tunable_initialize", "parse_tunables", "elf_machine_matches_host", "free_derivation", "free_modules_db", "derivation_compare", "find_derivation", "insert_module", "add_module", "add_alias2.part.0", "read_conf_file.isra.0", "find_module_idx", "find_module.constprop.0", "known_compare", "do_release_shlib", "do_release_all", "new_composite_name", "free_category", "plural_eval", "transcmp", "alias_compare", "read_alias_file", "do_swap", "msort_with_tmp.part.0", "indirect_msort_with_tmp", "read_int", "outstring_converted_wide_string", "group_number", "printf_positional", "read_int", "save_for_wbackup", "adjust_wide_data", "clear_once_control", "opendir_tail", "trecurse", "trecurse_r", "tdestroy_recurse", "maybe_split_for_insert.isra.0", "fatal_error", "length_mismatch", "is_dst", "is_trusted_path_normalize", "add_path.constprop.0.isra.0", "add_name_to_object.isra.0", "open_verify.constprop.0", "open_path.isra.0", "elf_machine_matches_host", "expand_dynamic_string_token", "fillin_rpath.isra.0", "decompose_rpath", "check_match", "do_lookup_x", "elf_machine_matches_host", "elf_machine_matches_host", "search_cache", "do_dlopen", "dlerror_run", "do_dlsym_private", "do_dlsym", "do_dlvsym", "do_dlclose", "free_slotinfo", "gconv_parse_code", "hack_digit", "two_way_long_needle", "remove_slotinfo", "call_dl_init", "add_to_global_update", "dl_open_worker", "dl_open_worker_begin", "add_to_global_resize_failure.isra.0", "add_to_global_resize", "elf_machine_matches_host", "dfs_traversal.part.0", "dlinfo_doit", "dlmopen_doit", "dlopen_doit", "dlsym_doit", "dlvsym_doit", "openaux", "call_init", "check_match", "call_dl_lookup", "do_sym", "base_of_encoded_value", "read_encoded_value_with_base", "stpcpy", "tsearch", "clock_gettime", "secure_getenv", "strcpy", "unsetenv", "gsignal", "bcmp", "setjmp", "getrlimit", "ioctl", "dlerror", "fstatat", "strtok_r", "dlinfo", "sysconf", "vsprintf", "getdtablesize", "fdopendir", "strtoull_l", "pthread_rwlock_rdlock", "memrchr", "strndup", "argz_add_sep", "stat64", "memmove", "pthread_rwlock_init", "getauxval", "snprintf", "munmap", "sched_getparam", "malloc_stats", "strtoimax", "getenv", "wcslen", "memmem", "wmempcpy", "sys_veval", "getpid", "getpagesize", "arc4random", "qsort", "dlmopen", "getrlimit64", "memcpy", "strtoll_l", "rewinddir", "sys_vcall", "malloc", "isatty", "openat64", "sched_setscheduler", "sysinfo", "vsnprintf", "strtoll", "register_printf_modifier", "strtoumax", "strtoul", "sched_getscheduler", "pthread_rwlock_unlock", "readdir", "pvalloc", "dladdr", "lseek", "wmemcpy", "clearenv", "mmap", "strtol_l", "dlclose", "abort", "sys_print", "fstat64", "tdelete", "strtoq", "strtol", "strnlen", "strrchr", "calloc", "strcasecmp_l", "malloc_usable_size", "tdestroy", "rindex", "write", "dladdr1", "wmemchr", "fstat", "dcgettext", "pthread_once", "madvise", "mremap", "getdelim", "memchr", "tfind", "lstat", "strstr", "pthread_kill", "read", "getentropy", "strncmp", "dlopen", "wcsrtombs", "getdents64", "pthread_rwlock_wrlock", "get_avphys_pages", "setenv", "sched_get_priority_max", "funlockfile", "pthread_sigmask", "realloc", "readdir64", "wcsnlen", "register_printf_specifier", "memcmp", "sched_get_priority_min", "malloc_trim", "lstat64", "sigaction", "twalk_r", "dlsym", "sbrk", "strdup", "strtoull", "index", "fopen", "memset", "get_phys_pages", "mallinfo2", "mallopt", "fclose", "open64", "malloc_info", "tcgetattr", "opendir", "strcmp", "pthread_mutex_unlock", "register_printf_function", "strtoul_l", "getcwd", "fstatat64", "memalign", "asprintf", "strerror_r", "strcspn", "setlocale", "mmap64", "strsep", "valloc", "dlvsym", "fputc", "register_printf_type", "mallinfo", "pthread_self", "twalk", "argz_create_sep", "stat", "fast_exit", "wmemmove", "fwrite", "qsort_r", "strtouq", "pthread_mutex_lock", "gettext", "get_nprocs", "exit", "brk", "openat", "fgets_unlocked", "strspn", "strlen", "lseek64", "open", "strchr", "arc4random_buf", "fputs", "mprotect", "closedir", "vasprintf", "strchrnul", "get_nprocs_conf", "aligned_alloc", "posix_memalign", "wcrtomb", "close", "raise", "free", "sigprocmask", "fopen64" };

PackedStringArray Sandbox::get_functions() const {
	PackedStringArray array;
	// Get all unmangled public functions from the guest program.
	for (auto &functions : machine().memory.all_unmangled_function_symbols()) {
		// Exclude functions that belong to the C/C++ runtime, as well as compiler-generated functions.
		if (exclude_functions.count(functions) == 0) {
			array.append(String(std::string(functions).c_str()));
		}
	}
	return array;
}

PackedStringArray Sandbox::get_functions_from_binary(const PackedByteArray &binary) {
	PackedStringArray array;
	if (binary.is_empty()) {
		return array;
	}

	const auto binary_view = std::string_view{ (const char *)binary.ptr(), static_cast<size_t>(binary.size()) };
	try {
		// Instantiate Machine without loading the ELF
		machine_t machine{ binary_view, riscv::MachineOptions<RISCV_ARCH>{
												.load_program = false, // Do not load the ELF program.
										} };
		// Get all unmangled public functions from the guest program.
		for (auto &functions : machine.memory.all_unmangled_function_symbols()) {
			// Exclude functions that belong to the C/C++ runtime, as well as compiler-generated functions.
			if (exclude_functions.count(functions) == 0) {
				array.append(String(std::string(functions).c_str()));
			}
		}
	} catch (const std::exception &e) {
		ERR_PRINT("Failed to get functions from binary. " + String(e.what()));
	}
	return array;
}
