#include "guest_datatypes.h"

#include <godot_cpp/variant/utility_functions.hpp>
#include <libriscv/util/crc32.hpp>

Variant GuestVariant::toVariant(const Sandbox &emu) const {
	switch (type) {
		case Variant::NIL:
			return Variant();
		case Variant::BOOL:
			return v.b;
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

		case Variant::OBJECT: {
			godot::Object *obj = (godot::Object *)uintptr_t(v.i);
			if (emu.is_scoped_object(obj))
				return Variant{ obj };
			else
				throw std::runtime_error("GuestVariant::toVariant(): Object is not known/scoped");
		}

		case Variant::DICTIONARY:
		case Variant::ARRAY:
		case Variant::CALLABLE:
		case Variant::STRING:
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
		case Variant::PACKED_BYTE_ARRAY:
		case Variant::PACKED_FLOAT32_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY:
		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_INT64_ARRAY:
		case Variant::PACKED_VECTOR2_ARRAY:
		case Variant::PACKED_VECTOR3_ARRAY: {
			if (std::optional<const Variant *> v = emu.get_scoped_variant(this->v.i)) {
				return *v.value();
			} else
				throw std::runtime_error("GuestVariant::toVariant(): Dictionary/Array/Callable is not known/scoped");
		}
		default:
			ERR_PRINT(("GuestVariant::toVariant(): Unsupported type: " + std::to_string(type)).c_str());
			return Variant();
	}
}

const Variant *GuestVariant::toVariantPtr(const Sandbox &emu) const {
	switch (type) {
		case Variant::DICTIONARY:
		case Variant::ARRAY:
		case Variant::CALLABLE:
		case Variant::STRING:
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
		case Variant::PACKED_BYTE_ARRAY:
		case Variant::PACKED_FLOAT32_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY:
		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_INT64_ARRAY:
		case Variant::PACKED_VECTOR2_ARRAY:
		case Variant::PACKED_VECTOR3_ARRAY: {
			if (std::optional<const Variant *> v = emu.get_scoped_variant(this->v.i))
				return v.value();
			else
				throw std::runtime_error("GuestVariant::toVariantPtr(): Callable is not known/scoped");
		}
		default:
			throw std::runtime_error("Don't use toVariantPtr() on unsupported type: " + std::to_string(type));
	}
}

void GuestVariant::set(Sandbox &emu, const Variant &value, bool implicit_trust) {
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

		case Variant::VECTOR2:
			this->v.v2f[0] = value.operator godot::Vector2().x;
			this->v.v2f[1] = value.operator godot::Vector2().y;
			break;
		case Variant::VECTOR2I:
			this->v.v2i[0] = value.operator godot::Vector2i().x;
			this->v.v2i[1] = value.operator godot::Vector2i().y;
			break;
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
		case Variant::VECTOR3:
			this->v.v3f[0] = value.operator godot::Vector3().x;
			this->v.v3f[1] = value.operator godot::Vector3().y;
			this->v.v3f[2] = value.operator godot::Vector3().z;
			break;
		case Variant::VECTOR3I:
			this->v.v3i[0] = value.operator godot::Vector3i().x;
			this->v.v3i[1] = value.operator godot::Vector3i().y;
			this->v.v3i[2] = value.operator godot::Vector3i().z;
			break;
		case Variant::VECTOR4:
			this->v.v4f[0] = value.operator godot::Vector4().x;
			this->v.v4f[1] = value.operator godot::Vector4().y;
			this->v.v4f[2] = value.operator godot::Vector4().z;
			this->v.v4f[3] = value.operator godot::Vector4().w;
			break;
		case Variant::VECTOR4I:
			this->v.v4i[0] = value.operator godot::Vector4i().x;
			this->v.v4i[1] = value.operator godot::Vector4i().y;
			this->v.v4i[2] = value.operator godot::Vector4i().z;
			this->v.v4i[3] = value.operator godot::Vector4i().w;
			break;

		case Variant::OBJECT: { // Objects are represented as uintptr_t
			if (!implicit_trust)
				throw std::runtime_error("GuestVariant::set(): Cannot set OBJECT type without implicit trust");
			// TODO: Check if the object is already scoped?
			godot::Object *obj = value.operator godot::Object *();
			emu.add_scoped_object(obj);
			this->v.i = (uintptr_t)obj;
			break;
		}

		case Variant::DICTIONARY:
		case Variant::ARRAY:
		case Variant::CALLABLE:
		case Variant::STRING:
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
		case Variant::PACKED_BYTE_ARRAY:
		case Variant::PACKED_FLOAT32_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY:
		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_INT64_ARRAY:
		case Variant::PACKED_VECTOR2_ARRAY:
		case Variant::PACKED_VECTOR3_ARRAY: {
			if (!implicit_trust)
				throw std::runtime_error("GuestVariant::set(): Cannot set complex type without implicit trust");
			this->v.i = emu.add_scoped_variant(&value);
			break;
		}
		default:
			ERR_PRINT(("SetVariant(): Unsupported type: " + std::to_string(value.get_type())).c_str());
	}
}

void GuestVariant::create(Sandbox &emu, Variant &&value) {
	this->type = value.get_type();

	switch (this->type) {
		case Variant::NIL:
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:

		case Variant::VECTOR2:
		case Variant::VECTOR2I:
		case Variant::RECT2:
		case Variant::RECT2I:
		case Variant::VECTOR3:
		case Variant::VECTOR3I:
		case Variant::VECTOR4:
		case Variant::VECTOR4I:
			this->set(emu, value, true); // Trust the value
			break;

		case Variant::OBJECT: {
			godot::Object *obj = value.operator godot::Object *();
			emu.add_scoped_object(obj);
			this->v.i = (uintptr_t)obj;
			break;
		}

		case Variant::DICTIONARY:
		case Variant::ARRAY:
		case Variant::CALLABLE:
		case Variant::STRING:
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
		case Variant::PACKED_BYTE_ARRAY:
		case Variant::PACKED_FLOAT32_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY:
		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_INT64_ARRAY:
		case Variant::PACKED_VECTOR2_ARRAY:
		case Variant::PACKED_VECTOR3_ARRAY: {
			// Store the variant in the current state
			unsigned int idx = emu.create_scoped_variant(std::move(value));
			this->v.i = idx;
			break;
		}
		default:
			ERR_PRINT(("CreateVariant(): Unsupported type: " + std::to_string(value.get_type())).c_str());
	}
}

void GuestVariant::free(Sandbox &emu) {
	throw std::runtime_error("GuestVariant::free(): Not implemented");
}
