#pragma once
#include <cstdint>

namespace gdscript {

enum class ProfilingClock : uint32_t {
	// rdtime (0xC01). Host-provided; default is 4us-granularity, override with
	// set_rdtime() for nanoseconds. Includes syscall time.
	TIME = 0,
	// rdcycle (0xC00). Retired instruction count. Deterministic but blind to
	// time spent inside syscalls.
	INSTRUCTIONS = 1,
};

// R+W data area exported as PROFILING_SYMBOL. Not emitted when profiling is off.
// Layout: header | records[function_count] | shadow_stack[MAX_DEPTH].
// Record order matches IRProgram::functions (and ::signatures).
struct ProfilingLayout {
	static constexpr uint32_t MAGIC = 0x50534447; // 'GDSP'
	static constexpr uint32_t LAYOUT_VERSION = 1;

	// Overflows counted but not recorded; depth still tracks for balanced exit.
	static constexpr uint32_t MAX_DEPTH = 256;

	// Header. Compiler-written before DEPTH_OFF; guest-written from there on.
	static constexpr int32_t MAGIC_OFF = 0;
	static constexpr int32_t VERSION_OFF = 4;
	static constexpr int32_t FUNCTION_COUNT_OFF = 8;
	static constexpr int32_t RECORD_SIZE_OFF = 12;
	static constexpr int32_t MAX_DEPTH_OFF = 16;
	static constexpr int32_t CLOCK_OFF = 20; // ProfilingClock
	static constexpr int32_t DEPTH_OFF = 24;
	static constexpr int32_t OVERFLOW_OFF = 32;
	static constexpr int32_t HEADER_SIZE = 40;

	// Per-function record. Power-of-two sized for shift-based indexing.
	static constexpr int32_t CALL_COUNT_OFF = 0;
	static constexpr int32_t SELF_OFF = 8;
	static constexpr int32_t TOTAL_OFF = 16;
	static constexpr int32_t RESERVED_OFF = 24;
	static constexpr int32_t RECORD_SIZE = 32;
	static constexpr int32_t RECORD_SHIFT = 5;

	// Shadow frame. Function index is an immediate in the exit sequence.
	static constexpr int32_t ENTRY_STAMP_OFF = 0;
	static constexpr int32_t CHILD_OFF = 8;
	static constexpr int32_t FRAME_SIZE = 16;
	static constexpr int32_t FRAME_SHIFT = 4;

	static constexpr int32_t records_offset() { return HEADER_SIZE; }

	static constexpr int32_t record_offset(uint32_t function_index) {
		return records_offset() + int32_t(function_index) * RECORD_SIZE;
	}

	static constexpr int32_t shadow_offset(uint32_t function_count) {
		return records_offset() + int32_t(function_count) * RECORD_SIZE;
	}

	static constexpr int32_t frame_offset(uint32_t function_count, uint32_t depth) {
		return shadow_offset(function_count) + int32_t(depth) * FRAME_SIZE;
	}

	static constexpr int32_t area_size(uint32_t function_count) {
		return shadow_offset(function_count) + int32_t(MAX_DEPTH) * FRAME_SIZE;
	}
};

inline constexpr const char *PROFILING_SYMBOL = "__gdsc_profiling";

} // namespace gdscript
