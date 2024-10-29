#pragma once
//#include <godot_cpp/godot.hpp>
#ifdef GODOT_MODULE
#include "modules/register_module_types.h"
#else
#include <godot_cpp/godot.hpp>
GODOT_NAMESPACE
#endif


//ScriptLanguage *get_elf_language();

static void initialize_sandbox_module(ModuleInitializationLevel p_level);
static void uninitialize_sandbox_module(ModuleInitializationLevel p_level);
