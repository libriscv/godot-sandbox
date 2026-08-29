#pragma once
#include <cstddef>

namespace gdscript {

// One direct-mapped cache per trait, keyed by object handle: 8 bytes of key
// followed by 8 bytes of answer. Exported per trait so the host can zero them
// when a script change makes a cached 'is Trait' answer stale.
struct TraitCacheLayout {
	static constexpr size_t ENTRIES = 64;
	static constexpr size_t ENTRY_SIZE = 16;
	static constexpr size_t AREA_SIZE = ENTRIES * ENTRY_SIZE;
};

inline constexpr const char *TRAIT_CACHE_SYMBOL_PREFIX = "__gdsc_trait_cache_";

} // namespace gdscript
