#pragma once
#include <godot_cpp/variant/variant.hpp>
using namespace godot;

class Sandbox;
struct GuestVariant;

class RiscvCallable : public CallableCustom {
public:
	// Identity is what the callable names, not the object holding it: connect()
	// makes a new RiscvCallable every time, so comparing addresses would leave
	// disconnect() and is_connected() unable to find a connection that is there.
	uint32_t hash() const override {
		uint32_t value = uint32_t(uint64_t(address)) ^ uint32_t(uint64_t(address) >> 32);
		value = value * 31 + (uint32_t(uint64_t(instance_base)) ^ uint32_t(uint64_t(instance_base) >> 32));
		value = value * 31 + uint32_t(uint64_t(sandbox_id));
		return value * 31 + uint32_t(m_varargs_base_count);
	}

	String get_as_text() const override {
		return "<RiscvCallable>";
	}

	// Same sandbox, same script instance, same guest function, same bound
	// arguments. Godot compares the two functions before calling one, so both
	// sides are RiscvCallable here.
	bool equals(const RiscvCallable &other) const {
		if (sandbox_id != other.sandbox_id || instance_base != other.instance_base ||
				address != other.address || m_varargs_base_count != other.m_varargs_base_count) {
			return false;
		}
		for (int i = 0; i < m_varargs_base_count; i++) {
			if (m_varargs[i] != other.m_varargs[i]) {
				return false;
			}
		}
		return true;
	}

	// Ordering over the same fields, so that "not less either way" is "equal".
	bool less_than(const RiscvCallable &other) const {
		if (sandbox_id != other.sandbox_id) {
			return uint64_t(sandbox_id) < uint64_t(other.sandbox_id);
		}
		if (instance_base != other.instance_base) {
			return instance_base < other.instance_base;
		}
		if (address != other.address) {
			return address < other.address;
		}
		if (m_varargs_base_count != other.m_varargs_base_count) {
			return m_varargs_base_count < other.m_varargs_base_count;
		}
		for (int i = 0; i < m_varargs_base_count; i++) {
			if (m_varargs[i] != other.m_varargs[i]) {
				return m_varargs[i] < other.m_varargs[i];
			}
		}
		return false;
	}

	CompareEqualFunc get_compare_equal_func() const override {
		return [](const CallableCustom *p_a, const CallableCustom *p_b) {
			return static_cast<const RiscvCallable *>(p_a)->equals(
					*static_cast<const RiscvCallable *>(p_b));
		};
	}

	CompareLessFunc get_compare_less_func() const override {
		return [](const CallableCustom *p_a, const CallableCustom *p_b) {
			return static_cast<const RiscvCallable *>(p_a)->less_than(
					*static_cast<const RiscvCallable *>(p_b));
		};
	}

	bool is_valid() const override;

	ObjectID get_object() const override {
		return sandbox_id;
	}

	void call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, GDExtensionCallError &r_call_error) const override;


	void init(Sandbox *self, gaddr_t address, Array args, bool variant_arguments = false);

private:
	Sandbox *sandbox() const;

	ObjectID sandbox_id;
	// Not part of identity: does not distinguish callables naming the same thing.
	ObjectID tree_base_id;
	gaddr_t instance_base = 0x0;
	gaddr_t address = 0x0;
	bool m_variant_arguments = false;

	std::array<Variant, 8> m_varargs;
	mutable std::array<const Variant *, 8> m_varargs_ptrs;
	int m_varargs_base_count = 0;
};
