#include "sandbox.h"

#include <libriscv/native_heap.hpp>

// -= Fast-path Variant Arguments =-

struct GDNativeVariant {
	uint8_t type;
	uint8_t padding[7];
	union {
		struct {
			double flt;
			uint64_t flt_padding1;
		};
		struct {
			uint64_t value;
			uint64_t i64_padding;
		};
		struct {
			real_t vec2_flt[2];
		};
		struct {
			int32_t ivec2_int[2];
		};
		struct {
			real_t vec3_flt[3];
		};
		struct {
			int32_t ivec3_int[3];
		};
		struct {
			real_t vec4_flt[4];
		};
		struct {
			int32_t ivec4_int[4];
		};
		struct {
			float color_flt[4];
		};
		struct {
			uint64_t object_id;
			GodotObject *object_ptr;
		};
	};

	godot::Object *to_object() const {
		if (object_ptr == nullptr)
			return nullptr;
		return internal::get_object_instance_binding(object_ptr);
	}

} __attribute__((packed));

// -= Guest Data Types =-

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
		std::string_view view = machine.memory.rvview(ptr, size);
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

	void set_string(machine_t &machine, gaddr_t self, const void *str, std::size_t len) {
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

struct GuestStdU32String {
	gaddr_t ptr;
	gaddr_t size;
	gaddr_t capacity;

	char32_t *to_array(const machine_t &machine) const {
		return machine.memory.memarray<char32_t>(ptr, size);
	}
	std::u32string to_u32string(const machine_t &machine, std::size_t max_len = 4UL << 20) const {
		if (size > max_len)
			throw std::runtime_error("Guest std::u32string too large (size > 4MB)");
		// Copy the string from guest memory
		const std::u32string_view view{ to_array(machine), size_t(size) };
		return std::u32string(view.begin(), view.end());
	}

	String to_godot_string(const machine_t &machine, std::size_t max_len = 1UL << 20) const {
		if (size > max_len)
			throw std::runtime_error("Guest std::u32string too large (size > 4MB)");
		// Get a view of the string from guest memory, including the null terminator
		const std::u32string_view view{ to_array(machine), size_t(size + 1) };
		if (view.back() != U'\0')
			throw std::runtime_error("Guest std::u32string is not null-terminated");
		// Convert the string to a godot String
		return String(view.data());
	}

	void set_string(machine_t &machine, gaddr_t self, const char32_t *str, std::size_t len) {
		// Allocate memory for the string
		this->ptr = machine.arena().malloc(len * sizeof(char32_t));
		this->size = len;
		this->capacity = len;
		// Copy the string to guest memory
		char32_t *guest_ptr = machine.memory.memarray<char32_t>(this->ptr, len);
		std::memcpy(guest_ptr, str, len * sizeof(char32_t));
	}

	void free(machine_t &machine) {
		if (ptr != 0x0)
			machine.arena().free(ptr);
	}
};

struct GuestStdVector {
	gaddr_t ptr_begin;
	gaddr_t ptr_end;
	gaddr_t ptr_capacity;

	gaddr_t data() const noexcept { return ptr_begin; }
	std::size_t size_bytes() const noexcept { return ptr_end - ptr_begin; }
	std::size_t capacity() const noexcept { return ptr_capacity - ptr_begin; }

	template <typename T>
	std::size_t size(std::size_t max_bytes = 16UL << 20) const {
		if (size_bytes() > max_bytes)
			throw std::runtime_error("Guest std::vector too large (size > 16MB)");
		return size_bytes() / sizeof(T);
	}

	template <typename T>
	T *view_as(const machine_t &machine, std::size_t max_bytes = 16UL << 20) const {
		return machine.memory.memarray<T>(data(), size<T>(max_bytes));
	}

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

	template <typename T>
	inline void assign_shared(machine_t &machine, gaddr_t shared_addr, std::size_t elements) {
		this->ptr_begin = shared_addr;
		this->ptr_end = shared_addr + elements * sizeof(T);
		this->ptr_capacity = this->ptr_end;
	}

	void free(machine_t &machine) {
		if (capacity() > 0)
			machine.arena().free(this->data());
	}
};

struct GuestVariant {
	/**
	 * @brief Creates a new godot Variant from a GuestVariant that comes from a sandbox.
	 *
	 * @param emu The sandbox that the GuestVariant comes from.
	 * @return Variant The new godot Variant.
	 **/
	Variant toVariant(const Sandbox &emu) const;

	/**
	 * @brief Returns a pointer to a Variant that comes from a sandbox. This Variant must be
	 * already stored in the sandbox's state. This usually means it's a complex Variant that is
	 * never copied to the guest memory. Most complex types are stored in the call state and are
	 * referenced by their index.
	 *
	 * @param emu The sandbox that the GuestVariant comes from.
	 * @return const Variant* The pointer to the Variant.
	 **/
	const Variant *toVariantPtr(const Sandbox &emu) const;

	/**
	 * @brief Sets the value of the GuestVariant from a godot Variant.
	 *
	 * @param emu The sandbox that the GuestVariant comes from.
	 * @param value The godot Variant.
	 * @param implicit_trust If true, creating a complex type will not throw an error.
	 * @throw std::runtime_error If the type is not supported.
	 * @throw std::runtime_error If the type is complex and implicit_trust is false.
	 **/
	void set(Sandbox &emu, const Variant &value, bool implicit_trust = false);

	/**
	 * @brief Sets the value of the GuestVariant to a godot Object. The object is added to the
	 * scoped objects list and the GuestVariant stores the untranslated address to the object.
	 * @warning The object is implicitly trusted, treated as allowed.
	 *
	 * @param emu The sandbox that the GuestVariant comes from.
	 * @param obj The godot Object.
	 **/
	void set_object(Sandbox &emu, godot::Object *obj);

	/**
	 * @brief Creates a new GuestVariant from a godot Variant. Trust is implicit for complex types.
	 * The variant is constructed directly into the call state, and the GuestVariant is an index
	 * to it. For objects, the object is added to the scoped objects list and the GuestVariant
	 * stores the untranslated address to the object.
	 *
	 * @param emu The sandbox that the GuestVariant comes from.
	 * @param value The godot Variant.
	 * @throw std::runtime_error If the type is not supported.
	 **/
	void create(Sandbox &emu, Variant &&value);

	/**
	 * @brief Check if the GuestVariant is implemented using an index to a scoped Variant.
	 *
	 * @return true If the type of the GuestVariant is implemented using an index to a scoped Variant.
	 */
	bool is_scoped_variant() const noexcept;

	Variant::Type type = Variant::NIL;
	union alignas(8) {
		int64_t i = 0;
		bool b;
		double f;
		std::array<float, 2> v2f;
		std::array<float, 3> v3f;
		std::array<float, 4> v4f;
		std::array<int32_t, 2> v2i;
		std::array<int32_t, 3> v3i;
		std::array<int32_t, 4> v4i;
	} v;

	void free(Sandbox &emu);

	static const char *type_name(int type);
	static const char *type_name(Variant::Type type) { return type_name(type); }
};

inline bool GuestVariant::is_scoped_variant() const noexcept {
	switch (type) {
		case Variant::STRING:
		case Variant::TRANSFORM2D:
		case Variant::QUATERNION:
		case Variant::AABB:
		case Variant::BASIS:
		case Variant::TRANSFORM3D:
		case Variant::PROJECTION:
		case Variant::DICTIONARY:
		case Variant::ARRAY:
		case Variant::CALLABLE:
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
		case Variant::RID:
		case Variant::PACKED_BYTE_ARRAY:
		case Variant::PACKED_FLOAT32_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY:
		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_INT64_ARRAY:
		case Variant::PACKED_VECTOR2_ARRAY:
		case Variant::PACKED_VECTOR3_ARRAY:
		case Variant::PACKED_COLOR_ARRAY:
		case Variant::PACKED_STRING_ARRAY: {
			return true;
		}
		case Variant::OBJECT: // Objects are raw pointers.
		default:
			return false;
	}
}

static_assert(sizeof(GuestVariant) == 24, "GuestVariant size mismatch");

static inline void hash_combine(gaddr_t &seed, gaddr_t hash) {
	hash += 0x9e3779b9 + (seed << 6) + (seed >> 2);
	seed ^= hash;
}
