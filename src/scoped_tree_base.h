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

	ScopedTreeBase(Sandbox *sandbox, godot::ObjectID tree_base) :
			sandbox(sandbox),
			previous(sandbox->get_tree_base_id()) {
		sandbox->set_tree_base_id(tree_base);
	}

	~ScopedTreeBase() {
		sandbox->set_tree_base_id(previous);
	}
};

struct ScopedInstanceBase {
	Sandbox *sandbox = nullptr;
	gaddr_t previous = 0;

	ScopedInstanceBase(Sandbox *sandbox, gaddr_t base) :
			sandbox(sandbox),
			previous(sandbox->get_instance_base()) {
		if (base != 0) {
			sandbox->set_instance_base(base);
		}
	}

	~ScopedInstanceBase() {
		sandbox->set_instance_base(previous);
	}
};
