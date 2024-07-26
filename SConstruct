#!/usr/bin/env python
import os
import sys

env = SConscript("godot-cpp/SConstruct")


env.Prepend(CPPPATH=["libriscv/lib", "libriscv/lib/posix"])
env.Append(CPPPATH=["src/"])

sources = [Glob("src/*.cpp")]

sources.extend([
    Glob("libriscv/lib/libriscv/*.c"),
])

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
