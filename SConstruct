#!/usr/bin/env python
import os
import sys
from build_utils import add_compilation_flags, get_sources

ARGUMENTS["disable_exceptions"] = "0"
ARGUMENTS["use_mingw"] = "yes"

env = SConscript("ext/godot-cpp/SConstruct")
env = add_compilation_flags(env)
sources = get_sources(env)

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
