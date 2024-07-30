#pragma once

#include <godot_cpp/classes/control.hpp>

#include <godot_cpp/core/binder_common.hpp>
#include <libriscv/machine.hpp>
#include <libriscv/native_heap.hpp>
#include <unordered_set>

using namespace godot;
#define RISCV_ARCH riscv::RISCV64
using gaddr_t = riscv::address_type<RISCV_ARCH>;
using machine_t = riscv::Machine<RISCV_ARCH>;
#include "vmcallable.hpp"

class Sandbox : public Node
{
	GDCLASS(Sandbox, Node);

protected:
	static void _bind_methods();

	String _to_string() const;

public:
	static constexpr uint64_t MAX_INSTRUCTIONS = 16'000'000'000ULL;
	static constexpr unsigned MAX_LEVEL = 8;
	static constexpr unsigned GODOT_VARIANT_SIZE = sizeof(Variant);

	Sandbox();
	~Sandbox();

	auto& machine() { return *m_machine; }
	const auto& machine() const { return *m_machine; }

	// Functions.
	void load(Variant vbuf, const TypedArray<String>& arguments);
	// Make a function call to a function in the guest by its name.
	Variant vmcall(const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error);
	Variant vmcall_address(gaddr_t address, const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error);
	// Make a callable object that will call a function in the guest by its name.
	Variant vmcallable(String function);

	void print(std::string_view text);
	gaddr_t address_of(std::string_view name) const;
	gaddr_t cached_address_of(String name) const;

	Variant vmcall_internal(gaddr_t address, const Variant** args, int argc);

	void add_scoped_variant(uint32_t hash) { m_scoped_variants.insert(hash); }
	bool is_scoped_variant(uint32_t hash) const noexcept { return m_scoped_variants.count(hash) > 0; }
private:
	void handle_exception(gaddr_t);
	void handle_timeout(gaddr_t);
	void initialize_syscalls();
	void setup_arguments(const Variant** args, int argc);

	machine_t* m_machine = nullptr;
	std::vector<uint8_t> m_binary;

	mutable std::unordered_map<std::string, gaddr_t> m_lookup;

	bool m_last_newline = false;
	uint8_t  m_level = 0;
	unsigned m_budget_overruns = 0;

	std::unordered_set<uint32_t> m_scoped_variants;
};

struct GuestStdString
{
	static constexpr std::size_t SSO = 15;

	gaddr_t     ptr;
	std::size_t size;
	union {
		char    data[SSO + 1];
		gaddr_t capacity;
	};

	std::string to_string(const machine_t& machine, std::size_t max_len = 16UL << 20) const
	{
		if (size <= SSO)
			return std::string(data, size);
		else if (size > max_len)
			throw std::runtime_error("Guest std::string too large (size > 16MB)");
		// Copy the string from guest memory
		const auto view = machine.memory.rvview(ptr, size);
		return std::string(view.data(), view.size());
	}
	String to_godot_string(const machine_t& machine, std::size_t max_len = 16UL << 20) const
	{
		return String(to_string(machine, max_len).c_str());
	}

	void set_string(machine_t& machine, gaddr_t self, const char* str, std::size_t len)
	{
		if (len <= SSO)
		{
			this->ptr = self + offsetof(GuestStdString, data);
			std::memcpy(this->data, str, len);
			this->data[len] = '\0';
			this->size = len;
		}
		else
		{
			// Allocate memory for the string
			this->ptr = machine.arena().malloc(len + 1);
			this->size = len;
			// Copy the string to guest memory
			char *guest_ptr = machine.memory.memarray<char>(this->ptr, len + 1);
			std::memcpy(guest_ptr, data, len);
			guest_ptr[len] = '\0';
			this->capacity = len;
		}
	}
};

struct GuestStdVector
{
	gaddr_t ptr;
	std::size_t size;
	std::size_t capacity;

	template <typename T>
	std::vector<T> to_vector(const machine_t& machine) const
	{
		if (size > capacity)
			throw std::runtime_error("Guest std::vector has size > capacity");
		// Copy the vector from guest memory
		const T* array = machine.memory.memarray<T>(ptr, size);
		return std::vector<T>(array, size);
	}
};

struct GuestVariant {
	Variant toVariant(const Sandbox& emu) const;
	Variant* toVariantPtr(const Sandbox& emu) const;
	void set(Sandbox& emu, const Variant& value);

	inline uint32_t hash() const noexcept;

	static constexpr unsigned GODOT_VARIANT_SIZE = sizeof(Variant);
	Variant::Type type;
	union {
		uint8_t opaque[GODOT_VARIANT_SIZE];
		bool    b;
		int64_t i;
		double  f;
		gaddr_t s;
		std::array<float, 2> v2f;
		std::array<float, 3> v3f;
		std::array<float, 4> v4f;
		std::array<int32_t, 2> v2i;
		std::array<int32_t, 3> v3i;
		std::array<int32_t, 4> v4i;
	} v;
};
