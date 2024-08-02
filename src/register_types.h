#pragma once
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/script_language.hpp>

using namespace godot;

void initialize_riscv_module(ModuleInitializationLevel p_level);
void uninitialize_riscv_module(ModuleInitializationLevel p_level);
ScriptLanguage * get_cpp_language();
