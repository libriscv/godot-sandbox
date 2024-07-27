#include "riscv.hpp"

#include <godot_cpp/variant/utility_functions.hpp>
#include <libriscv/util/crc32.hpp>

inline uint32_t GuestVariant::hash() const noexcept {
	return riscv::crc32c(v.opaque, sizeof(Variant));
}

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
		case Variant::STRING: {
			auto s = emu.machine().memory.rvspan<GuestStdString>(v.s, 1);
			return s[0].to_godot_string(emu.machine());
		}

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

		case Variant::DICTIONARY:
		case Variant::ARRAY:
		case Variant::CALLABLE:
			if (emu.is_scoped_variant(this->hash()))
				return *(Variant *)&v.opaque[0];
			else
				throw std::runtime_error("GuestVariant::toVariant(): Dictionary/Array/Callable is not known/scoped");
		default:
			UtilityFunctions::print("GuestVariant::toVariant(): Unsupported type: ", type);
			return Variant();
	}
}

Variant *GuestVariant::toVariantPtr(const Sandbox &emu) const {
	switch (type) {
		case Variant::CALLABLE: {
			if (emu.is_scoped_variant(this->hash()))
				return (Variant *)&v.opaque[0];
			else
				throw std::runtime_error("GuestVariant::toVariantPtr(): Callable is not known/scoped");
		}
		default:
			throw std::runtime_error("Don't use toVariantPtr() on unsupported type: " + std::to_string(type));
	}
}

void GuestVariant::set(Sandbox &emu, const Variant &value) {
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
		case Variant::STRING: {
			auto s = value.operator String();
			auto str = s.utf8();
			// Allocate memory for the GuestStdString (which is a std::string)
			// TODO: Improve this by allocating string + contents + null terminator in one go
			auto &machine = emu.machine();
			auto ptr = machine.arena().malloc(sizeof(GuestStdString));
			auto gstr = machine.memory.rvspan<GuestStdString>(ptr, 1);
			gstr[0].set_string(machine, ptr, str.get_data(), str.length());
			this->v.s = ptr;
			break;
		}
		case Variant::VECTOR2:
			this->v.v2f = std::to_array(value.operator godot::Vector2().coord);
			break;
		case Variant::VECTOR2I:
			this->v.v2i = std::to_array(value.operator godot::Vector2i().coord);
			break;
		case Variant::RECT2: {
			auto rect = value.operator godot::Rect2();
			this->v.v4f[0] = rect.position[0];
			this->v.v4f[1] = rect.position[1];
			this->v.v4f[2] = rect.size[0];
			this->v.v4f[3] = rect.size[1];
			break;
		}
		case Variant::RECT2I: {
			auto rect = value.operator godot::Rect2i();
			this->v.v4i[0] = rect.position[0];
			this->v.v4i[1] = rect.position[1];
			this->v.v4i[2] = rect.size[0];
			this->v.v4i[3] = rect.size[1];
			break;
		}
		case Variant::VECTOR3:
			this->v.v3f = std::to_array(value.operator godot::Vector3().coord);
			break;
		case Variant::VECTOR3I:
			this->v.v3i = std::to_array(value.operator godot::Vector3i().coord);
			break;
		case Variant::VECTOR4:
			this->v.v4f = std::to_array(value.operator godot::Vector4().components);
			break;
		case Variant::VECTOR4I:
			this->v.v4i = std::to_array(value.operator godot::Vector4i().coord);
			break;

		case Variant::DICTIONARY:
		case Variant::ARRAY:
		case Variant::CALLABLE: {
			std::memcpy(this->v.opaque, &value, sizeof(Variant));
			emu.add_scoped_variant(this->hash());
			break;
		}
		default:
			UtilityFunctions::print("SetVariant(): Unsupported type: ", value.get_type());
	}
}
