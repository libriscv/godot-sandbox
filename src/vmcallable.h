#pragma once
#include <godot_cpp/variant/variant.hpp>
using namespace godot;

class Sandbox;
struct GuestVariant;

class RiscvCallable : public CallableCustom {
public:
	virtual uint32_t hash() const {
		return address;
	}

	virtual String get_as_text() const {
		return "<RiscvCallable>";
	}

	static bool compare_equal_func(const CallableCustom *p_a, const CallableCustom *p_b) {
		return p_a == p_b;
	}

	virtual CompareEqualFunc get_compare_equal_func() const {
		return &RiscvCallable::compare_equal_func;
	}

	static bool compare_less_func(const CallableCustom *p_a, const CallableCustom *p_b) {
		return (void *)p_a < (void *)p_b;
	}

	virtual CompareLessFunc get_compare_less_func() const {
		return &RiscvCallable::compare_less_func;
	}

	bool is_valid() const {
		return self != nullptr;
	}

	virtual ObjectID get_object() const {
		return ObjectID();
	}

	virtual void call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, GDExtensionCallError &r_call_error) const;

	void init(Sandbox *self, gaddr_t address) {
		this->self = self;
		this->address = address;
	}

private:
	Sandbox *self = nullptr;
	gaddr_t address = 0x0;
};
