#include "guest_datatypes.h"

#include <godot_cpp/variant/utility_functions.hpp>
#include <libriscv/util/crc32.hpp>

namespace riscv {
extern godot::Object *get_object_from_address(const Sandbox &emu, uint64_t addr);
} //namespace riscv

Variant GuestVariant::toVariant(const Sandbox &emu) const {
	switch (type) {
		case Variant::NIL:
			return Variant();
		case Variant::BOOL:
			return v.b_bits != 0;
		case Variant::INT:
			return v.i;
		case Variant::FLOAT:
			return v.f;

		case Variant::VECTOR2:
			return Variant{ godot::Vector2(v.v2f[0], v.v2f[1]) };
		case Variant::VECTOR2I:
			return Variant{ godot::Vector2i(v.v2i[0], v.v2i[1]) };
		case Variant::RECT2:
			return Variant{ godot::Rect2(v.v4f[0], v.v4f[1], v.v4f[2], v.v4f[3]) };
		case Variant::RECT2I:
			return Variant{ godot::Rect2i(v.v4i[0], v.v4i[1], v.v4i[2], v.v4i[3]) };
		case Variant::VECTOR3:
			return Variant{ godot::Vector3(v.v3f[0], v.v3f[1], v.v3f[2]) };
		case Variant::VECTOR3I:
			return Variant{ godot::Vector3i(v.v3i[0], v.v3i[1], v.v3i[2]) };
		case Variant::VECTOR4:
			return Variant{ godot::Vector4(v.v4f[0], v.v4f[1], v.v4f[2], v.v4f[3]) };
		case Variant::VECTOR4I:
			return Variant{ godot::Vector4i(v.v4i[0], v.v4i[1], v.v4i[2], v.v4i[3]) };
		case Variant::COLOR:
			return Variant{ godot::Color(v.v4f[0], v.v4f[1], v.v4f[2], v.v4f[3]) };
		case Variant::PLANE:
			return Variant{ godot::Plane(godot::Vector3(v.v4f[0], v.v4f[1], v.v4f[2]), v.v4f[3]) };

		case Variant::OBJECT: {
			godot::Object *obj = riscv::get_object_from_address(emu, v.i);
			return Variant{ obj };
		}

		default:
			if (std::optional<const Variant *> v = emu.get_scoped_variant(this->v.i)) {
				const Variant *var = *v;
				return *var;
			} else {
				char buffer[128];
				snprintf(buffer, sizeof(buffer), "GuestVariant::toVariant(): %u (%s) idx=%d is not known/scoped",
						type, GuestVariant::type_name(type), int32_t(this->v.i));
				throw std::runtime_error(buffer);
			}
	}
}

const Variant *GuestVariant::toVariantPtr(const Sandbox &emu) const {
	if (std::optional<const Variant *> v = emu.get_scoped_variant(this->v.i))
		return v.value();

	char buffer[128];
	snprintf(buffer, sizeof(buffer), "GuestVariant::toVariantPtr(): %u (%s) idx=%d is not known/scoped",
			type, GuestVariant::type_name(type), int32_t(this->v.i));
	throw std::runtime_error(buffer);
}

void GuestVariant::set_object(Sandbox &emu, godot::Object *obj) {
	this->type = Variant::OBJECT;
	this->v.i = emu.add_scoped_object(obj);
}

bool GuestVariant::set_inlined(const Variant &value) noexcept {
	// Godot only stores these types inline when real_t is a 32-bit float; with a wider
	// real_t the larger vectors move to the heap and the layout below no longer holds.
	if constexpr (sizeof(real_t) != sizeof(float)) {
		return false;
	} else {
		const GDNativeVariant *inner = (const GDNativeVariant *)value._native_ptr();

		// Only the bytes that actually belong to the value are copied. Godot leaves the
		// remainder of the payload holding whatever the Variant used to contain, and that
		// must never be handed to the guest.
		unsigned bytes;
		switch (inner->type) {
			case Variant::NIL:
				this->type = Variant::NIL;
				return true;
			case Variant::BOOL:
				bytes = sizeof(bool);
				break;
			case Variant::INT:
			case Variant::FLOAT:
			case Variant::VECTOR2:
			case Variant::VECTOR2I:
				bytes = 8;
				break;
			case Variant::VECTOR3:
			case Variant::VECTOR3I:
				bytes = 12;
				break;
			case Variant::RECT2:
			case Variant::RECT2I:
			case Variant::VECTOR4:
			case Variant::VECTOR4I:
			case Variant::COLOR:
			case Variant::PLANE:
				bytes = 16;
				break;
			default:
				return false; // Not stored inline: the caller has to scope it instead
		}
		this->type = Variant::Type(inner->type);
		std::memset(&this->v, 0, sizeof(this->v));
		std::memcpy(&this->v, &inner->value, bytes);
		return true;
	}
}

void GuestVariant::set(Sandbox &emu, const Variant &value, bool implicit_trust) {
	// Fast path: copy the payload straight out of the Variant. Every godot-cpp accessor
	// (including get_type()) is an out-of-line call into Godot, which is more than these
	// types are worth on a path taken by every API call that returns a value.
	if (LIKELY(this->set_inlined(value)))
		return;

	this->type = value.get_type();

	switch (this->type) {
		case Variant::NIL:
			break;
		case Variant::BOOL:
			this->v.b = value;
			break;
		case Variant::INT:
			this->v.i = value;
			break;
		case Variant::FLOAT:
			this->v.f = value;
			break;

		case Variant::VECTOR2: {
			godot::Vector2 vec = value.operator godot::Vector2();
			this->v.v2f[0] = vec.x;
			this->v.v2f[1] = vec.y;
			break;
		}
		case Variant::VECTOR2I: {
			godot::Vector2i vec = value.operator godot::Vector2i();
			this->v.v2i[0] = vec.x;
			this->v.v2i[1] = vec.y;
			break;
		}
		case Variant::RECT2: {
			Rect2 rect = value.operator godot::Rect2();
			this->v.v4f[0] = rect.position[0];
			this->v.v4f[1] = rect.position[1];
			this->v.v4f[2] = rect.size[0];
			this->v.v4f[3] = rect.size[1];
			break;
		}
		case Variant::RECT2I: {
			Rect2i rect = value.operator godot::Rect2i();
			this->v.v4i[0] = rect.position[0];
			this->v.v4i[1] = rect.position[1];
			this->v.v4i[2] = rect.size[0];
			this->v.v4i[3] = rect.size[1];
			break;
		}
		case Variant::VECTOR3: {
			godot::Vector3 vec = value.operator godot::Vector3();
			this->v.v3f[0] = vec.x;
			this->v.v3f[1] = vec.y;
			this->v.v3f[2] = vec.z;
			break;
		}
		case Variant::VECTOR3I: {
			godot::Vector3i vec = value.operator godot::Vector3i();
			this->v.v3i[0] = vec.x;
			this->v.v3i[1] = vec.y;
			this->v.v3i[2] = vec.z;
			break;
		}
		case Variant::VECTOR4: {
			godot::Vector4 vec = value.operator godot::Vector4();
			this->v.v4f[0] = vec.x;
			this->v.v4f[1] = vec.y;
			this->v.v4f[2] = vec.z;
			this->v.v4f[3] = vec.w;
			break;
		}
		case Variant::VECTOR4I: {
			godot::Vector4i vec = value.operator godot::Vector4i();
			this->v.v4i[0] = vec.x;
			this->v.v4i[1] = vec.y;
			this->v.v4i[2] = vec.z;
			this->v.v4i[3] = vec.w;
			break;
		}
		case Variant::COLOR: {
			godot::Color color = value.operator godot::Color();
			this->v.v4f[0] = color.r;
			this->v.v4f[1] = color.g;
			this->v.v4f[2] = color.b;
			this->v.v4f[3] = color.a;
			break;
		}
		case Variant::PLANE: {
			godot::Plane plane = value.operator godot::Plane();
			this->v.v4f[0] = plane.normal.x;
			this->v.v4f[1] = plane.normal.y;
			this->v.v4f[2] = plane.normal.z;
			this->v.v4f[3] = plane.d;
			break;
		}

		case Variant::OBJECT: { // Objects are represented as uintptr_t
			if (!implicit_trust)
				throw std::runtime_error("GuestVariant::set(): Cannot set OBJECT type without implicit trust");
			// A Variant outlives the object it names, and keeps pointing at it either way.
			// get_validated_object() answers through the instance id instead, so a Variant
			// left over from a freed object comes back null rather than as a stale pointer.
			godot::Object *obj = value.get_validated_object();
			if (obj == nullptr)
				throw std::runtime_error("GuestVariant::set(): Object no longer exists");
			if (!emu.is_allowed_object(obj))
				throw std::runtime_error("GuestVariant::set(): Object is not allowed");
			this->v.i = emu.add_scoped_object(obj);
			break;
		}

		default: {
			if (!implicit_trust)
				throw std::runtime_error("GuestVariant::set(): Cannot set complex type without implicit trust");
			this->v.i = emu.add_scoped_variant(&value);
		}
	}
}

void GuestVariant::create(Sandbox &emu, Variant &&value) {
	// Fast path for the types that live inline in the GuestVariant, which covers most
	// return values. See set_inlined() for why this beats going through godot-cpp.
	if (LIKELY(this->set_inlined(value)))
		return;

	this->type = value.get_type();

	switch (this->type) {
		case Variant::OBJECT: {
			// Validated through the instance id, for the reason given in set() above.
			godot::Object *obj = value.get_validated_object();
			if (obj == nullptr) {
				// A null object is an answer, not a fault: it is what
				// get_node_or_null() and every other _or_null form returns, and
				// what a Variant left over from a freed object reads as. The
				// guest gets null, which is what GDScript sees.
				this->type = Variant::NIL;
				this->v.i = 0;
				return;
			}
			if (!emu.is_allowed_object(obj))
				throw std::runtime_error("GuestVariant::create(): Object is not allowed");
			// value is an rvalue and dies right after this; add_scoped_object() takes a
			// reference of its own when the object is RefCounted, so the guest's handle
			// does not become a pointer to a freed Ref.
			this->v.i = emu.add_scoped_object(obj);
			break;
		}

		default: {
			// Store the variant in the current state
			unsigned int idx = emu.create_scoped_variant(std::move(value));
			this->v.i = idx;
		}
	}
}

void GuestVariant::free(Sandbox &emu) {
	throw std::runtime_error("GuestVariant::free(): Not implemented");
}

static const char *variant_type_names[] = {
	"Nil",

	"Bool", // 1
	"Int",
	"Float",
	"String",

	"Vector2", // 5
	"Vector2i",
	"Rect2",
	"Rect2i",
	"Vector3",
	"Vector3i",
	"Transform2D",
	"Vector4",
	"Vector4i",
	"Plane",
	"Quaternion",
	"AABB",
	"Basis",
	"Transform3D",
	"Projection",

	"Color", // 20
	"StringName",
	"NodePath",
	"RID",
	"Object",
	"Callable",
	"Signal",
	"Dictionary",
	"Array",

	"PackedByteArray", // 29
	"PackedInt32Array",
	"PackedInt64Array",
	"PackedFloat32Array",
	"PackedFloat64Array",
	"PackedStringArray",
	"PackedVector2Array",
	"PackedVector3Array",
	"PackedColorArray",
	"PackedVector4Array",

	"Max",
};

const char *GuestVariant::type_name(int type) {
	if (type < 0 || type >= Variant::Type::VARIANT_MAX) {
		return "Unknown";
	}
	return variant_type_names[type];
}
