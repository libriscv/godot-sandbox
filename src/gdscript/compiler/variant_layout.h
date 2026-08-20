#pragma once

namespace gdscript {

// Godot can be built with real_t = float (the default) or real_t = double
// (REAL_T_IS_DOUBLE on the host, DOUBLE_PRECISION_REAL_T in the sandbox API).
// That choice changes the width of every real_t-based type that a Variant
// stores inline -- Vector2/3/4, Rect2, Plane, Quaternion and Color -- and with
// it the size of the Variant itself: 24 bytes normally, 40 bytes with doubles.
//
// What does NOT change:
//   - the 4-byte type tag and the 8-byte offset of the data union
//   - Variant::FLOAT, because Godot's float is a 64-bit double in both builds
//   - Variant::INT and Variant::OBJECT, which live in the 64-bit v.i field
//   - the integer vectors (Vector2i/3i/4i, Rect2i), always int32_t components
//
// Every layout-dependent number in the RISC-V backend goes through this struct
// so that the two builds share one code path.
struct VariantLayout {
	bool double_precision = false;

	VariantLayout() = default;
	explicit VariantLayout(bool dp) :
			double_precision(dp) {}

	// Offset of the 4-byte m_type tag
	static constexpr int TYPE_OFFSET = 0;
	// Offset of the data union (bool / int64_t / double / real_t[4] / int32_t[4])
	static constexpr int DATA_OFFSET = 8;
	// The data union always holds room for 4 components
	static constexpr int COMPONENTS = 4;
	// int32_t components are 4 bytes wide in both builds
	static constexpr int INT_COMPONENT_SIZE = 4;

	// sizeof(real_t)
	int real_size() const { return double_precision ? 8 : 4; }
	// sizeof(Variant): 24 with float real_t, 40 with double real_t
	int variant_size() const { return DATA_OFFSET + COMPONENTS * real_size(); }
	// How many 64-bit words a whole Variant spans (3 or 5)
	int variant_words() const { return variant_size() / 8; }
	// Offset of real_t component `index` (x/y/z/w, or r/g/b/a)
	int real_offset(int index) const { return DATA_OFFSET + index * real_size(); }
	// Offset of int32_t component `index`
	static constexpr int int_offset(int index) { return DATA_OFFSET + index * INT_COMPONENT_SIZE; }

	bool operator==(const VariantLayout& other) const { return double_precision == other.double_precision; }
	bool operator!=(const VariantLayout& other) const { return !(*this == other); }
};

// The layout that matches the build this compiler was compiled for. The guest
// program and the host addon both define DOUBLE_PRECISION_REAL_T when Godot
// uses double precision, so this is the right default in either context.
inline VariantLayout native_variant_layout() {
#ifdef DOUBLE_PRECISION_REAL_T
	return VariantLayout(true);
#else
	return VariantLayout(false);
#endif
}

} // namespace gdscript
