#pragma once

#include <godot_cpp/classes/control.hpp>

#include <godot_cpp/core/binder_common.hpp>
#include <libriscv/machine.hpp>
#include <libriscv/native_heap.hpp>

using namespace godot;
#define RISCV_ARCH riscv::RISCV64
using gaddr_t = riscv::address_type<RISCV_ARCH>;
using machine_t = riscv::Machine<RISCV_ARCH>;
#include "elf/script_elf.h"
#include "vmcallable.h"
#include "vmproperty.h"

class Sandbox : public Node {
	GDCLASS(Sandbox, Node);

protected:
	static void _bind_methods();

	String _to_string() const;

public:
	static constexpr unsigned MAX_INSTRUCTIONS = 16;
	static constexpr unsigned MAX_HEAP = 16ul;
	static constexpr unsigned MAX_VMEM = 16ul;
	static constexpr unsigned MAX_LEVEL = 8;
	static constexpr unsigned GODOT_VARIANT_SIZE = sizeof(Variant);
	static constexpr unsigned MAX_PROPERTIES = 16;

	Sandbox();
	~Sandbox();

	auto &machine() { return *m_machine; }
	const auto &machine() const { return *m_machine; }

	// Functions.
	bool has_program_loaded() const;
	void set_program(Ref<ELFScript> program);
	Ref<ELFScript> get_program();
	PackedStringArray get_functions() const;
	static PackedStringArray get_functions_from_binary(const PackedByteArray &binary);
	// Make a function call to a function in the guest by its name.
	Variant vmcall(const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error);
	Variant vmcall_fn(const StringName &function, const Variant **args, GDExtensionInt arg_count);
	Variant vmcall_address(gaddr_t address, const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error);
	// Make a callable object that will call a function in the guest by its name.
	Variant vmcallable(const String &function);

	// Properties.
	uint32_t get_max_refs() const { return m_max_refs; }
	void set_max_refs(uint32_t max) { m_max_refs = max; }
	void set_memory_max(uint32_t max) { m_memory_max = max; }
	uint32_t get_memory_max() const { return m_memory_max; }
	void set_instructions_max(int64_t max) { m_insn_max = max; }
	int64_t get_instructions_max() const { return m_insn_max; }
	void set_heap_usage(int64_t) {} // Do nothing (it's a read-only property)
	int64_t get_heap_usage() const;
	void set_budget_overruns(unsigned budget) {} // Do nothing (it's a read-only property)
	unsigned get_budget_overruns() const { return m_budget_overruns; }
	void set_calls_made(unsigned calls) {} // Do nothing (it's a read-only property)
	unsigned get_calls_made() const { return m_calls_made; }

	void print(std::string_view text);
	gaddr_t address_of(std::string_view name) const;
	gaddr_t cached_address_of(int64_t hash) const;
	gaddr_t cached_address_of(int64_t hash, const String &name) const;

	Variant vmcall_internal(gaddr_t address, const Variant **args, int argc, GDExtensionCallError &error);

	// Testing
	void assault(const String &test, int64_t iterations);

	auto &state() const { return *m_current_state; }
	auto &state() { return *m_current_state; }

	void set_tree_base(godot::Node *tree_base) { this->m_tree_base = tree_base; }
	godot::Node *get_tree_base() const { return this->m_tree_base; }

	void add_scoped_variant(uint32_t hash);
	bool is_scoped_variant(uint32_t hash) const noexcept { return state().scoped_variants.end() != std::find(state().scoped_variants.begin(), state().scoped_variants.end(), hash); }

	void add_scoped_object(const void *ptr);
	void rem_scoped_object(const void *ptr) { state().scoped_objects.erase(std::remove(state().scoped_objects.begin(), state().scoped_objects.end(), reinterpret_cast<uintptr_t>(ptr)), state().scoped_objects.end()); }
	bool is_scoped_object(const void *ptr) const noexcept { return state().scoped_objects.end() != std::find(state().scoped_objects.begin(), state().scoped_objects.end(), reinterpret_cast<uintptr_t>(ptr)); }

	// Properties
	void add_property(const String &name, Variant::Type vtype, uint64_t setter, uint64_t getter, const Variant &def = "") const;
	bool set_property(const StringName &name, const Variant &value);
	bool get_property(const StringName &name, Variant &r_ret);
	SandboxProperty *find_property_or_null(const StringName &name) const;
	std::vector<SandboxProperty> &get_properties() { return m_properties; }
	const std::vector<SandboxProperty> &get_properties() const { return m_properties; }

private:
	void load(PackedByteArray &&vbuf, const std::vector<std::string> *argv = nullptr);
	void read_program_properties(bool editor) const;
	void handle_exception(gaddr_t);
	void handle_timeout(gaddr_t);
	void print_backtrace(gaddr_t);
	void initialize_syscalls();
	GuestVariant *setup_arguments(gaddr_t &sp, const Variant **args, int argc);

	Ref<ELFScript> m_program_data;
	machine_t *m_machine = nullptr;
	godot::Node *m_tree_base = nullptr;
	PackedByteArray m_binary;
	uint32_t m_max_refs = 100;
	uint32_t m_memory_max = MAX_VMEM;
	int64_t m_insn_max = MAX_INSTRUCTIONS;

	mutable std::unordered_map<int64_t, gaddr_t> m_lookup;

	bool m_last_newline = false;
	uint8_t m_level = 0;
	unsigned m_budget_overruns = 0;
	unsigned m_calls_made = 0;

	struct CurrentState {
		godot::Node *tree_base;
		std::vector<uint32_t> scoped_variants;
		std::vector<uintptr_t> scoped_objects;
	};
	CurrentState *m_current_state = nullptr;
	std::array<CurrentState, MAX_LEVEL> m_states;

	// Properties
	mutable std::vector<SandboxProperty> m_properties;
};

struct GuestStdString {
	static constexpr std::size_t SSO = 15;

	gaddr_t ptr;
	gaddr_t size;
	union {
		char data[SSO + 1];
		gaddr_t capacity;
	};

	std::string_view to_view(const machine_t &machine, std::size_t max_len = 4UL << 20) const {
		if (size <= SSO)
			return std::string_view(data, size);
		else if (size > max_len)
			throw std::runtime_error("Guest std::string too large (size > 4MB)");
		// View the string from guest memory
		return machine.memory.rvview(ptr, size);
	}
	String to_godot_string(const machine_t &machine, std::size_t max_len = 4UL << 20) const {
		if (size <= SSO)
			return String::utf8(data, size);
		else if (size > max_len)
			throw std::runtime_error("Guest std::string too large (size > 4MB)");
		// View the string from guest memory, but include the null terminator
		auto view = machine.memory.rvview(ptr, size);
		return String::utf8(view.data(), view.size());
	}
	size_t copy_unterminated_to(const machine_t &machine, void *dst, std::size_t max_len) const {
		if (size <= SSO && size <= max_len) {
			std::memcpy(dst, data, size);
			return size;
		} else if (size >= max_len) {
			// Copy the string from guest memory
			machine.copy_from_guest(dst, ptr, size);
			return size;
		}
		return 0; // Failed to copy
	}
	PackedByteArray to_packed_byte_array(const machine_t &machine, std::size_t max_len = 4UL << 20) const {
		PackedByteArray arr;
		arr.resize(size);
		const size_t res = copy_unterminated_to(machine, arr.ptrw(), max_len);
		if (res != size) {
			ERR_PRINT("GuestVariant::toVariant(): PackedByteArray copy failed");
			return Variant();
		}
		return arr;
	}

	void set_string(machine_t &machine, gaddr_t self, const char *str, std::size_t len) {
		if (len <= SSO) {
			this->ptr = self + offsetof(GuestStdString, data);
			std::memcpy(this->data, str, len);
			this->data[len] = '\0';
			this->size = len;
		} else {
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

	void free(machine_t &machine, gaddr_t self) {
		// Free the string if it was allocated on the guest heap
		if (ptr != self + offsetof(GuestStdString, data))
			machine.arena().free(ptr);
	}
};
static_assert(sizeof(GuestStdString) == 32, "GuestStdString size mismatch");

struct GuestStdVector {
	gaddr_t ptr_begin;
	gaddr_t ptr_end;
	gaddr_t ptr_capacity;

	gaddr_t data() const noexcept { return ptr_begin; }
	std::size_t size_bytes() const noexcept { return ptr_end - ptr_begin; }
	std::size_t capacity() const noexcept { return ptr_capacity - ptr_begin; }

	template <typename T>
	std::vector<T> to_vector(const machine_t &machine) const {
		if (size_bytes() > capacity())
			throw std::runtime_error("Guest std::vector has size > capacity");
		// Copy the vector from guest memory
		const size_t elements = size_bytes() / sizeof(T);
		const T *array = machine.memory.memarray<T>(data(), elements);
		return std::vector<T>(&array[0], &array[elements]);
	}

	PackedFloat32Array to_f32array(const machine_t &machine) const {
		PackedFloat32Array array;
		const size_t elements = this->size_bytes() / sizeof(float);
		array.resize(elements);
		const float *fptr = machine.memory.memarray<float>(this->data(), elements);
		std::memcpy(array.ptrw(), fptr, elements * sizeof(float));
		return array;
	}
	PackedFloat64Array to_f64array(const machine_t &machine) const {
		PackedFloat64Array array;
		const size_t elements = this->size_bytes() / sizeof(double);
		array.resize(elements);
		const double *fptr = machine.memory.memarray<double>(this->data(), elements);
		std::memcpy(array.ptrw(), fptr, elements * sizeof(double));
		return array;
	}

	template <typename T>
	inline std::tuple<T *, gaddr_t> alloc(machine_t &machine, std::size_t elements) {
		this->ptr_begin = machine.arena().malloc(elements * sizeof(T));
		this->ptr_end = this->ptr_begin + elements * sizeof(T);
		this->ptr_capacity = this->ptr_end;
		return { machine.memory.memarray<T>(this->data(), elements), this->data() };
	}

	void free(machine_t &machine) {
		if (capacity() > 0)
			machine.arena().free(this->data());
	}
};

struct GuestVariant {
	Variant toVariant(const Sandbox &emu) const;
	Variant *toVariantPtr(const Sandbox &emu) const;
	void set(Sandbox &emu, const Variant &value);

	inline uint32_t hash() const noexcept;

	static constexpr unsigned GODOT_VARIANT_SIZE = sizeof(Variant);
	Variant::Type type;
	union {
		uint8_t opaque[GODOT_VARIANT_SIZE];
		bool b;
		int64_t i;
		double f;
		gaddr_t s; // String & PackedByteArray & Node2D -> GuestStdString
		gaddr_t vf32; // PackedFloat32Array -> GuestStdVector<float>
		gaddr_t vf64; // PackedFloat64Array -> GuestStdVector<double>
		std::array<float, 2> v2f;
		std::array<float, 3> v3f;
		std::array<float, 4> v4f;
		std::array<int32_t, 2> v2i;
		std::array<int32_t, 3> v3i;
		std::array<int32_t, 4> v4i;
	} v;

	void free(Sandbox &emu);
};
