#pragma once
#ifdef GODOT_MODULE
#include "modules/register_module_types.h"
#else
#include "src/config.h"
#include <godot_cpp/godot.hpp>
GODOT_NAMESPACE
#endif

static void initialize_sandbox_module(ModuleInitializationLevel p_level);
static void uninitialize_sandbox_module(ModuleInitializationLevel p_level);
