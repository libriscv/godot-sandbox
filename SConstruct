#!/usr/bin/env python
import os
import sys

ARGUMENTS["disable_exceptions"] = "0"

env = SConscript("godot-cpp/SConstruct")


env.Prepend(CPPPATH=["libriscv/lib"])
env.Append(CPPPATH=["src/", "."])

sources = [Glob("src/*.cpp")]

librisc_sources = [
     # switch-case:
"libriscv/lib/libriscv/bytecode_dispatch.cpp",
# threaded:
#"libriscv/lib/libriscv/threaded_dispatch.cpp",
# ignore this file:
#"libriscv/lib/libriscv/tailcall_dispatch.cpp",

#"libriscv/lib/libriscv/bytecode_impl.cpp",
"libriscv/lib/libriscv/cpu.cpp",
#"libriscv/lib/libriscv/cpu_dispatch.cpp",
#"libriscv/lib/libriscv/cpu_inaccurate_dispatch.cpp",
"libriscv/lib/libriscv/debug.cpp",
"libriscv/lib/libriscv/decode_bytecodes.cpp",
"libriscv/lib/libriscv/decoder_cache.cpp",
"libriscv/lib/libriscv/machine.cpp",
"libriscv/lib/libriscv/machine_defaults.cpp",
"libriscv/lib/libriscv/memory.cpp",
"libriscv/lib/libriscv/memory_elf.cpp",
"libriscv/lib/libriscv/memory_mmap.cpp",
"libriscv/lib/libriscv/memory_rw.cpp",
"libriscv/lib/libriscv/multiprocessing.cpp",
"libriscv/lib/libriscv/native_libc.cpp",
"libriscv/lib/libriscv/native_threads.cpp",
"libriscv/lib/libriscv/rv32i.cpp",
"libriscv/lib/libriscv/rv64i.cpp",
#"libriscv/lib/libriscv/rva_instr.cpp",
#"libriscv/lib/libriscv/rvc_instr.cpp",
#"libriscv/lib/libriscv/rvf_instr.cpp",
#"libriscv/lib/libriscv/rvi_instr.cpp",
#"libriscv/lib/libriscv/rvv_instr.cpp",
"libriscv/lib/libriscv/serialize.cpp",
"libriscv/lib/libriscv/threaded_rewriter.cpp",

# Binary translator
#"libriscv/lib/libriscv/tr_api.cpp"
#"libriscv/lib/libriscv/tr_emit.cpp"
#"libriscv/lib/libriscv/tr_emit_rvc.cpp"
#"libriscv/lib/libriscv/tr_translate.cpp"

# Binary translator - TCC
#"libriscv/lib/libriscv/tr_tcc.cpp"
# Binary translator - System compiler
#"libriscv/lib/libriscv/tr_compiler.cpp"
    ]

if env["platform"] == "windows":    
    librisc_sources += [
        "libriscv/lib/libriscv/win32/dlfcn.cpp",
        "libriscv/lib/libriscv/win32/system_calls.cpp",
        "libriscv/lib/libriscv/win32/tr_msvc.cpp",
    ]
elif env["platform"] == "linux": 
    librisc_sources += [
        "libriscv/lib/libriscv/linux/syscalls_epoll.cpp",
        "libriscv/lib/libriscv/linux/syscalls_mman.cpp",
        "libriscv/lib/libriscv/linux/syscalls_poll.cpp",
        "libriscv/lib/libriscv/linux/syscalls_select.cpp",
        "libriscv/lib/libriscv/linux/system_calls.cpp",
    ]
    
if env["platform"] != "windows":  
    librisc_sources += [
        "libriscv/lib/libriscv/linux/system_calls.cpp",
    ]

if env["platform"] != "windows" or env["use_mingw"]:
    env.Append(CXXFLAGS=["-std=c++20"])
else:
    env.Append(CXXFLAGS=["/std:c++20"])
    

sources.extend(librisc_sources)

if env["platform"] == "windows":
	env.Prepend(CPPPATH=["libriscv/lib/libriscv/lib/win32"])
elif env["platform"] == "macos":
    env.Prepend(CPPPATH=["libriscv/lib/libriscv/lib/macos"])
elif env["platform"] == "linux" or env["platform"] == "android":
    env.Prepend(CPPPATH=["libriscv/lib/libriscv/lib/linux"])

if env["platform"] == "macos" or env["platform"] == "ios":
	library = env.SharedLibrary(
		"bin/addons/godot_riscv/bin/libgodot_riscv{}.framework/libgodot_riscv{}".format(
			env["suffix"], env["suffix"]
		),
		source=sources,
	)
else:
	library = env.SharedLibrary(
		"bin/addons/godot_riscv/bin/libgodot_riscv{}{}".format(env["suffix"], env["SHLIBSUFFIX"]),
		source=sources,
	)
Default(library)
