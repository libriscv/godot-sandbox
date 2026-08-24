#pragma once
#include <cstdint>

namespace gdscript {

struct InstanceLayout {
	static constexpr uint32_t MAGIC = 0x49534447; // 'GDSI'
	static constexpr uint32_t LAYOUT_VERSION = 1;

	static constexpr int32_t MAGIC_OFF = 0;
	static constexpr int32_t VERSION_OFF = 4;
	static constexpr int32_t DEFAULT_BASE_OFF = 8;
	static constexpr int32_t RECORD_SIZE_OFF = 16;
	static constexpr int32_t MEMBER_COUNT_OFF = 20;
	static constexpr int32_t BLOB_SIZE = 24;
};

inline constexpr const char *INSTANCE_SYMBOL = "__gdsc_instance";
inline constexpr const char *INSTANCE_INIT_SYMBOL = "__gdsc_instance_init";

} // namespace gdscript
