#!/usr/bin/env python
import os
import sys

ARGUMENTS["disable_exceptions"] = "0"
ARGUMENTS["use_mingw"] = "yes"

# Use a double-precision extension_api.json for double builds, since the
# default one shipped with godot-cpp is single-precision only.
if ARGUMENTS.get("precision", "single") == "double" and "custom_api_file" not in ARGUMENTS:
    ARGUMENTS["custom_api_file"] = os.path.join(Dir('.').abspath, "gdextension", "extension_api_double.json")

env = SConscript("ext/godot-cpp/SConstruct")

env.Append(CPPDEFINES = ['RISCV_SYSCALLS_MAX=600', 'RISCV_BRK_MEMORY_SIZE=0x100000'])
# The external-compiler translator is always present. It performs cache lookup
# only during play; it never embeds TCC or invokes a compiler at run time.
env.Append(CPPDEFINES = ['RISCV_BINARY_TRANSLATION'])
env.Prepend(CPPPATH=["ext/libriscv/lib"])
env.Append(CPPPATH=["src/", "."])

# Pieces of the compiler the host needs whatever the policy below is: the
# definition of the formats the compiler publishes beside the ELF, and the
# symbol tables editor completion and lookup resolve from.
compiler_host_sources = [
    "src/gdscript/compiler/function_signature.cpp",
	"src/gdscript/compiler/property_signature.cpp",
	"src/gdscript/compiler/source_model.cpp",
    "src/gdscript/compiler/line_table.cpp",
    "src/gdscript/compiler/gdsmeta.cpp",
    "src/gdscript/compiler/globals.cpp",
    "src/gdscript/compiler/compiler_exception.cpp",
	"src/gdscript/compiler/debug_layout.cpp",
]

# The rest of the compiler, linked in when SafeGDScript compiles in process.
compiler_sources = compiler_host_sources + [
    "src/gdscript/compiler/token.cpp",
    "src/gdscript/compiler/lexer.cpp",
    "src/gdscript/compiler/parser.cpp",
    "src/gdscript/compiler/ast_clone.cpp",
    "src/gdscript/compiler/traits.cpp",
    "src/gdscript/compiler/chain.cpp",
    "src/gdscript/compiler/ir.cpp",
    "src/gdscript/compiler/ir_optimizer.cpp",
    "src/gdscript/compiler/ir_verifier.cpp",
    "src/gdscript/compiler/codegen.cpp",
    "src/gdscript/compiler/riscv_codegen.cpp",
    "src/gdscript/compiler/riscv_globals.cpp",
    "src/gdscript/compiler/riscv_profiling.cpp",
    "src/gdscript/compiler/riscv_debug.cpp",
    "src/gdscript/compiler/register_allocator.cpp",
    "src/gdscript/compiler/elf_builder.cpp",
    "src/gdscript/compiler/export_hints.cpp",
    "src/gdscript/compiler/compiler.cpp",
]

# Where the SafeGDScript compiler runs (src/safegdscript/compiler_backend.h):
#   sandboxed   gdscript.elf in a Sandbox, for every script
#   restricted  in process, except for a restricted Sandbox's scripts (default)
#   direct      in process, for every script -- gdb sees the compiler's frames
safegdscript_compiler = ARGUMENTS.get("safegdscript_compiler", "restricted")
compiler_policies = {"sandboxed": 0, "restricted": 1, "direct": 2}
if safegdscript_compiler not in compiler_policies:
    raise SystemExit("safegdscript_compiler must be one of: " + ", ".join(compiler_policies))
compiler_policy = compiler_policies[safegdscript_compiler]
env.Append(CPPDEFINES=["SAFEGDSCRIPT_COMPILER_POLICY=%d" % compiler_policy])

if compiler_policy == 0:
    gdscript_compiler_sources = compiler_host_sources
else:
    gdscript_compiler_sources = compiler_sources
    env.Append(CPPDEFINES=["SAFEGDSCRIPT_DIRECT_COMPILER"])
    # The compiler emits for the host's Variant layout, and real_t decides it.
    if ARGUMENTS.get("precision", "single") == "double":
        env.Append(CPPDEFINES=["DOUBLE_PRECISION_REAL_T"])

sources = [Glob("src/*.cpp"), Glob("src/cpp/*.cpp"), Glob("src/rust/*.cpp"), Glob("src/zig/*.cpp"), Glob("src/elf/*.cpp"), Glob("src/godot/*.cpp"), Glob("src/safegdscript/*.cpp"), gdscript_compiler_sources, ["src/tests/dummy_assault.cpp"], Glob("src/bintr/*.cpp")]

librisc_sources = [
    # threaded fast-path:
    "ext/libriscv/lib/libriscv/threaded_dispatch.cpp",
    "ext/libriscv/lib/libriscv/threaded_inaccurate_dispatch.cpp",

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
    "ext/libriscv/lib/libriscv/native_libc.cpp",
    "ext/libriscv/lib/libriscv/native_threads.cpp",
    #"ext/libriscv/lib/libriscv/rv32i.cpp",
    "ext/libriscv/lib/libriscv/rv64i.cpp",
    "ext/libriscv/lib/libriscv/serialize.cpp",
    "ext/libriscv/lib/libriscv/shared_rodata.cpp",

    # POSIX
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

if env["platform"] != "windows" or env["use_mingw"]:
    env.Append(CXXFLAGS=["-std=c++20"])
else:
    env.Append(CXXFLAGS=["/std:c++20"])

sources.extend(librisc_sources)

# GodotCPP XML documenation
def add_godot_cpp_doc_data(env, sources):
    try:
        doc_data = env.GodotCPPDocData("src/gen/doc_data.gen.cpp", source=Glob("doc_classes/*.xml"))
        sources.append(doc_data)
    except AttributeError:
        print("Not including class reference as we're targeting a pre-4.3 baseline.")

if env["platform"] == "windows":
    env.Prepend(CPPPATH=["ext/libriscv/lib/libriscv/lib/win32"])
    env.Prepend(LIBS=['ws2_32']) # for socket calls
    add_godot_cpp_doc_data(env, sources)
elif env["platform"] == "macos":
    env.Prepend(CPPPATH=["ext/libriscv/lib/libriscv/lib/macos"])
    env.Append(LINKFLAGS=["-framework", "Security"])
    add_godot_cpp_doc_data(env, sources)
elif env["platform"] == "linux" or env["platform"] == "android":
    env.Prepend(CPPPATH=["ext/libriscv/lib/libriscv/lib/linux"])
    add_godot_cpp_doc_data(env, sources)
    if env["platform"] == "linux":
        env.Append(LIBS=["dl"])

if "static_build" not in ARGUMENTS or ARGUMENTS["static_build"]!="yes":
    if env["platform"] == "macos" or env["platform"] == "ios":
        library = env.SharedLibrary(
            "bin/addons/godot_sandbox/bin/libgodot_riscv{}.framework/libgodot_riscv{}".format(
                env["suffix"], env["suffix"]
            ),
            source=sources,
        )
    else:
        library = env.SharedLibrary(
            "bin/addons/godot_sandbox/bin/libgodot_riscv{}{}".format(env["suffix"], env["SHLIBSUFFIX"]),
            source=sources,
        )
    Default(library)
else:
    library = env.StaticLibrary(
        "bin/libsandbox{}{}".format(env["suffix"], env["LIBSUFFIX"]),
        source=sources,
    )
    Default(library)
