#pragma once

#ifdef GODOT_MODULE
#define GODOT_CPP_FUNC(name) name
#define GODOT_NAMESPACE
#else
#define GODOT_CPP_FUNC(name) _##name
#define GODOT_NAMESPACE using namespace godot;
#endif
