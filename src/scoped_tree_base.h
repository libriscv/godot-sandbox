#pragma once

#include "sandbox.h"
#include <godot_cpp/classes/node.hpp>

struct ScopedTreeBase {
	Sandbox *sandbox = nullptr;
	godot::ObjectID previous;

	ScopedTreeBase(Sandbox *sandbox, godot::Node *tree_base) :
			sandbox(sandbox),
			previous(sandbox->get_tree_base_id()) {
		sandbox->set_tree_base(tree_base);
	}

	~ScopedTreeBase() {
		sandbox->set_tree_base_id(previous);
	}
};
