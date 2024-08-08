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
#include "elf/script_elf.h"

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
	void set_program(Ref<ELFScript> program);
	Ref<ELFScript> get_program();
	PackedStringArray get_functions() const;
	static PackedStringArray get_functions_from_binary(const PackedByteArray& binary);
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
	void load(PackedByteArray&& vbuf, const TypedArray<String>& arguments);
	void handle_exception(gaddr_t);
	void handle_timeout(gaddr_t);
	void print_backtrace(gaddr_t);
	void initialize_syscalls();
	GuestVariant *setup_arguments(const Variant** args, int argc);

	Ref<ELFScript> m_program_data;
	TypedArray<String> m_program_arguments;
	machine_t* m_machine = nullptr;
	PackedByteArray m_binary;

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
	size_t copy_unterminated_to(const machine_t& machine, void* dst, size_t maxlen)
	{
		if (size <= SSO && size <= maxlen) {
			std::memcpy(dst, data, size);
			return size;
		}
		else if (size >= maxlen) {
			// Copy the string from guest memory
			machine.copy_from_guest(dst, ptr, size);
			return size;
		}

		return 0; // Failed to copy
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
			std::memcpy(guest_ptr, str, len);
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

	PackedFloat32Array to_f32array(const machine_t& machine) const
	{
		PackedFloat32Array array;
		array.resize(this->size);
		const float *fptr = machine.memory.memarray<float>(this->ptr, this->size);
		std::memcpy(array.ptrw(), fptr, this->size * sizeof(float));
		return array;
	}
	PackedFloat64Array to_f64array(const machine_t& machine) const
	{
		PackedFloat64Array array;
		array.resize(this->size);
		const double *fptr = machine.memory.memarray<double>(this->ptr, this->size);
		std::memcpy(array.ptrw(), fptr, this->size * sizeof(double));
		return array;
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
		gaddr_t s;    // String & PackedByteArray -> GuestStdString
		gaddr_t vf32; // PackedFloat32Array -> GuestStdVector<float>
		gaddr_t vf64; // PackedFloat64Array -> GuestStdVector<double>
		std::array<float, 2> v2f;
		std::array<float, 3> v3f;
		std::array<float, 4> v4f;
		std::array<int32_t, 2> v2i;
		std::array<int32_t, 3> v3i;
		std::array<int32_t, 4> v4i;
	} v;
};
