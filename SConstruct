#!/usr/bin/env python
import os
import sys

env = SConscript("godot-cpp/SConstruct")


env.Prepend(CPPPATH=["libriscv/lib"])
env.Append(CPPPATH=["src/", "."])


sources = [Glob("src/*.cpp")]

librisc_sources = [
     # switch-case:
"libriscv/bytecode_dispatch.cpp"
# threaded:
#"libriscv/threaded_dispatch.cpp"
# ignore this file:
#"libriscv/tailcall_dispatch.cpp"

"libriscv/bytecode_impl.cpp"
"libriscv/cpu.cpp"
"libriscv/cpu_dispatch.cpp"
"libriscv/cpu_inaccurate_dispatch.cpp"
"libriscv/debug.cpp"
"libriscv/decode_bytecodes.cpp"
"libriscv/decoder_cache.cpp"
"libriscv/machine.cpp"
"libriscv/machine_defaults.cpp"
"libriscv/memory.cpp"
"libriscv/memory_elf.cpp"
"libriscv/memory_mmap.cpp"
"libriscv/memory_rw.cpp"
"libriscv/multiprocessing.cpp"
"libriscv/native_libc.cpp"
"libriscv/native_threads.cpp"
"libriscv/rv32i.cpp"
"libriscv/rv64i.cpp"
"libriscv/rva_instr.cpp"
"libriscv/rvc_instr.cpp"
"libriscv/rvf_instr.cpp"
"libriscv/rvi_instr.cpp"
#"libriscv/rvv_instr.cpp"
"libriscv/serialize.cpp"
"libriscv/threaded_rewriter.cpp"

# Binary translator
#"libriscv/tr_api.cpp"
#"libriscv/tr_emit.cpp"
#"libriscv/tr_emit_rvc.cpp"
#"libriscv/tr_translate.cpp"

# Binary translator - TCC
#"libriscv/tr_tcc.cpp"
# Binary translator - System compiler
#"libriscv/tr_compiler.cpp"
    ]

if env["platform"] == "windows":    
    librisc_sources += [
        "libriscv/windows/dlfcn.cpp",
        "libriscv/windows/system_calls.cpp",
        "libriscv/windows/tr_msvc.cpp",
    ]
elif env["platform"] == "linux": 
    librisc_sources += [
        "libriscv/linux/syscalls_epoll.cpp",
        "libriscv/linux/syscalls_mman.cpp",
        "libriscv/linux/syscalls_poll.cpp",
        "libriscv/linux/syscalls_select.cpp",
        "libriscv/linux/system_calls.cpp",
    ]
    
if env["platform"] != "windows":  
    librisc_sources += [
        "libriscv/linux/system_calls.cpp",
    ]

if env["platform"] != "windows" or env["use_mingw"]:
    env.Append(CXXFLAGS=["-std=c++20"])
else:
    env.Append(CXXFLAGS=["/std:c++20"])
    

sources.extend(librisc_sources)

if env["platform"] == "windows":
	env.Prepend(CPPPATH=["libriscv/lib/win32"])
elif env["platform"] == "macos":
    env.Prepend(CPPPATH=["libriscv/lib/macos"])
elif env["platform"] == "linux" or env["platform"] == "android":
    env.Prepend(CPPPATH=["libriscv/lib/linux"])

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
