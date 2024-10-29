#pragma once
#include <godot_cpp/godot.hpp>
#include <godot_cpp/classes/script_language.hpp>

GODOT_NAMESPACE

ScriptLanguage *get_elf_language();

static void initialize_sandbox_module(ModuleInitializationLevel p_level);
static void uninitialize_sandbox_module(ModuleInitializationLevel p_level);
