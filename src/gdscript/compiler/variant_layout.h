#pragma once

namespace gdscript {

// real_t width determines Variant size: 24B (float) or 40B (double).
// INT, FLOAT, OBJECT and integer vectors are unaffected.
struct VariantLayout {
	bool double_precision = false;

	VariantLayout() = default;
	explicit VariantLayout(bool dp) :
			double_precision(dp) {}

	static constexpr int TYPE_OFFSET = 0;
	static constexpr int DATA_OFFSET = 8;
	static constexpr int COMPONENTS = 4;
	static constexpr int INT_COMPONENT_SIZE = 4;

	int real_size() const { return double_precision ? 8 : 4; }
	int variant_size() const { return DATA_OFFSET + COMPONENTS * real_size(); }
	int variant_words() const { return variant_size() / 8; }
	int real_offset(int index) const { return DATA_OFFSET + index * real_size(); }
	static constexpr int int_offset(int index) { return DATA_OFFSET + index * INT_COMPONENT_SIZE; }

	bool operator==(const VariantLayout& other) const { return double_precision == other.double_precision; }
	bool operator!=(const VariantLayout& other) const { return !(*this == other); }
};

inline VariantLayout native_variant_layout() {
#ifdef DOUBLE_PRECISION_REAL_T
	return VariantLayout(true);
#else
	return VariantLayout(false);
#endif
}

} // namespace gdscript
