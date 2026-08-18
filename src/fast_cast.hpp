#pragma once
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/godot.hpp>
#include <type_traits>

/// @brief True for classes declared with GDCLASS() (our own GDExtension classes),
/// false for godot-cpp's engine classes, which use GDEXTENSION_CLASS().
/// @note GDCLASS() is the only one of the two that adds these public bind helpers,
/// which makes them the one compile-time difference between the two kinds of class.
/// The distinction matters because Godot hands every GDExtension class the class tag
/// of its closest *engine* base class, so a class tag cannot tell a Sandbox from any
/// other Node. sandbox.cpp asserts that this detection still holds.
template <typename T, typename = void>
struct is_extension_class : std::false_type {};
template <typename T>
struct is_extension_class<T, std::void_t<decltype(&T::notification_bind), decltype(&T::to_string_bind)>> : std::true_type {};
template <typename T>
inline constexpr bool is_extension_class_v = is_extension_class<T>::value;

/// @brief The class tag Godot uses to answer "is this object a T?", resolved once per T.
/// The regular lookup goes through a StringName and a ClassDB hash, neither of which
/// belongs on a path taken by every single object cast.
template <typename T>
static inline void *class_tag_for() {
	static void *const tag = godot::internal::gdextension_interface_classdb_get_class_tag(
			T::get_class_static()._native_ptr());
	return tag;
}

/// @brief Downcast an object, without godot::Object::cast_to()'s overhead.
/// @return The object as a T, or nullptr if it is not one (or is null).
/// @note Compared to cast_to() this always skips the per-call StringName construction
/// and ClassDB lookup. For engine classes it also replaces the dynamic_cast with a
/// class tag check and a static_cast.
template <typename T>
static inline T *fast_cast_to(godot::Object *obj) {
	if (obj == nullptr)
		return nullptr;
	if constexpr (is_extension_class_v<T>) {
		// The class tag of a GDExtension class is its engine base class' tag, so it would
		// happily accept any Node as a Sandbox. Only the C++ type can answer this one.
		return dynamic_cast<T *>(obj);
	} else {
		if (godot::internal::gdextension_interface_object_cast_to(obj->_owner, class_tag_for<T>()) == nullptr)
			return nullptr;
		// Safe as a static_cast: the engine has just confirmed the object derives from T, and the
		// binding for such an object is always a godot-cpp T, or a subclass of one.
		return static_cast<T *>(obj);
	}
}

/// @brief Const overload of fast_cast_to().
template <typename T>
static inline const T *fast_cast_to(const godot::Object *obj) {
	return fast_cast_to<T>(const_cast<godot::Object *>(obj));
}
