from SCons.Script import Glob

def add_compilation_flags(env, thirdparty=""):
	env.Append(CPPDEFINES = ['RISCV_SYSCALLS_MAX=600', 'RISCV_BRK_MEMORY_SIZE=0x100000'])
	env.Prepend(CPPPATH=[thirdparty + "ext/libriscv/lib"])
	env.Append(CPPPATH=["src/", "."])
	if env["platform"] != "windows" or env["use_mingw"]:
		env.Append(CXXFLAGS=["-std=c++20"])
	else:
		env.Append(CXXFLAGS=["/std:c++20"])

	if env["platform"] == "windows":
		env.Prepend(CPPPATH=[thirdparty + "ext/libriscv/lib/libriscv/lib/win32"])
		env.Prepend(LIBS=['ws2_32']) # for socket calls
	elif env["platform"] == "macos":
		env.Prepend(CPPPATH=[thirdparty + "ext/libriscv/lib/libriscv/lib/macos"])
	elif env["platform"] == "linux" or env["platform"] == "android":
		env.Prepend(CPPPATH=[thirdparty + "ext/libriscv/lib/libriscv/lib/linux"])
	return env

def get_sources(env, thirdparty=""):
    sources = [Glob("*.cpp"), Glob("src/*.cpp"), Glob("src/cpp/*.cpp"), Glob("src/rust/*.cpp"), Glob("src/zig/*.cpp"), Glob("src/elf/*.cpp"), Glob("src/godot/*.cpp"), ["src/tests/dummy_assault.cpp"], Glob("src/bintr/*.cpp")]

    librisc_sources = [
        # threaded fast-path:
        "ext/libriscv/lib/libriscv/threaded_dispatch.cpp",

        "ext/libriscv/lib/libriscv/cpu.cpp",
        "ext/libriscv/lib/libriscv/debug.cpp",
        "ext/libriscv/lib/libriscv/decode_bytecodes.cpp",
        "ext/libriscv/lib/libriscv/decoder_cache.cpp",
        "ext/libriscv/lib/libriscv/machine.cpp",
        "ext/libriscv/lib/libriscv/machine_defaults.cpp",
        "ext/libriscv/lib/libriscv/memory.cpp",
        "ext/libriscv/lib/libriscv/memory_elf.cpp",
        "ext/libriscv/lib/libriscv/memory_mmap.cpp",
        "ext/libriscv/lib/libriscv/memory_rw.cpp",
        "ext/libriscv/lib/libriscv/multiprocessing.cpp",
        "ext/libriscv/lib/libriscv/native_libc.cpp",
        "ext/libriscv/lib/libriscv/native_threads.cpp",
        #"ext/libriscv/lib/libriscv/rv32i.cpp",
        "ext/libriscv/lib/libriscv/rv64i.cpp",
        "ext/libriscv/lib/libriscv/serialize.cpp",

        # POSIX
        "ext/libriscv/lib/libriscv/posix/socket_calls.cpp",
        "ext/libriscv/lib/libriscv/posix/minimal.cpp",
        "ext/libriscv/lib/libriscv/posix/signals.cpp",
        "ext/libriscv/lib/libriscv/posix/threads.cpp",
        "ext/libriscv/lib/libriscv/util/crc32c.cpp",

        # Binary translator
        "ext/libriscv/lib/libriscv/tr_api.cpp",
        "ext/libriscv/lib/libriscv/tr_emit.cpp",
        "ext/libriscv/lib/libriscv/tr_translate.cpp",
    ]

    if env["platform"] == "windows":
        librisc_sources += [
            "ext/libriscv/lib/libriscv/win32/dlfcn.cpp",
            "ext/libriscv/lib/libriscv/win32/system_calls.cpp",
            "ext/libriscv/lib/libriscv/win32/tr_msvc.cpp",
        ]
    else:
        librisc_sources += [
            "ext/libriscv/lib/libriscv/linux/system_calls.cpp",

            # Binary translator - TCC
            #"ext/libriscv/lib/libriscv/tr_tcc.cpp"
            # Binary translator - System compiler
            "ext/libriscv/lib/libriscv/tr_compiler.cpp",
        ]
    
	# add to the librisc_sources in front of each string the thirdparty string
    for i in range(len(librisc_sources)):
        librisc_sources[i] = thirdparty + librisc_sources[i]
    sources.extend(librisc_sources)
    return sources
