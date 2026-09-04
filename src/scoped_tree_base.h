#pragma once

#include "fast_cast.hpp"
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

struct ScopedScriptInstanceOwner {
	Sandbox *sandbox = nullptr;
	godot::ObjectID previous;

	ScopedScriptInstanceOwner(Sandbox *sandbox, godot::Object *owner) :
			sandbox(sandbox), previous(sandbox->get_script_instance_owner_id()) {
		if (godot::Object::cast_to<godot::Node>(owner) == nullptr) {
			sandbox->set_script_instance_owner(owner);
		}
	}
	~ScopedScriptInstanceOwner() { sandbox->set_script_instance_owner_id(previous); }
};

struct ScopedCallContext {
	Sandbox *sandbox = nullptr;
	godot::ObjectID previous_tree_base;
	godot::ObjectID previous_owner;
	gaddr_t previous_instance_base = 0;

	ScopedCallContext(Sandbox *sandbox, godot::Object *owner, gaddr_t instance_base) :
			sandbox(sandbox),
			previous_tree_base(sandbox->get_tree_base_id()),
			previous_owner(sandbox->get_script_instance_owner_id()),
			previous_instance_base(sandbox->get_instance_base()) {
		// Nodes use tree_base; this channel is for non-Node owners only.
		godot::Node *node = fast_cast_to<godot::Node>(owner);
		sandbox->set_tree_base(node);
		if (node == nullptr) {
			sandbox->set_script_instance_owner(owner);
		}
		if (instance_base != 0) {
			sandbox->set_instance_base(instance_base);
		}
	}

	ScopedCallContext(Sandbox *sandbox, godot::ObjectID tree_base,
			godot::ObjectID owner, gaddr_t instance_base) :
			sandbox(sandbox),
			previous_tree_base(sandbox->get_tree_base_id()),
			previous_owner(sandbox->get_script_instance_owner_id()),
			previous_instance_base(sandbox->get_instance_base()) {
		sandbox->set_tree_base_id(tree_base);
		sandbox->set_script_instance_owner_id(owner);
		if (instance_base != 0) {
			sandbox->set_instance_base(instance_base);
		}
	}

	~ScopedCallContext() {
		sandbox->set_instance_base(previous_instance_base);
		sandbox->set_script_instance_owner_id(previous_owner);
		sandbox->set_tree_base_id(previous_tree_base);
	}
};
