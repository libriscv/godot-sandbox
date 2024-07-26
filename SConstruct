#!/usr/bin/env python
import os
import sys

env = SConscript("godot-cpp/SConstruct")


env.Prepend(CPPPATH=["thirdparty", "include"])
env.Append(CPPPATH=["src/"])
#env.Append(CPPDEFINES=['WHISPER_SHARED', 'GGML_SHARED'])

sources = [Glob("src/*.cpp")]

sources.extend([
    #Glob("thirdparty/libsamplerate/src/*.c"),
    #Glob("thirdparty/whisper.cpp/*.c"),
    #Glob("thirdparty/whisper.cpp/whisper.cpp"),
])

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
