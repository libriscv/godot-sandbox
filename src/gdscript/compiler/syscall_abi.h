#pragma once
#include <cstdint>
#include "syscall_numbers.h"

namespace gdscript {
// Register counts describe the host handler, not the number of C++ arguments
// or whether its result is live. Strings/views occupy pointer+length; writes
// through guest pointers are not register outputs. Keep this outside the
// frozen numeric contract: compiler and runtime include the same metadata.
struct SyscallABI {
	uint8_t inputs;
	uint8_t outputs;
	bool counted;
	bool floats = false;
};

// FP calls synchronize fa0-fa7 inputs and fa0-fa1 outputs. Integer-only
// counted calls clear bit 17 to skip guest FP register synchronization. Handlers
// that inspect other CPU state or re-enter the VM still need the ordinary
// system-call boundary. Dyncalls now synchronize instruction penalties when
// the translation enables its instruction limit.
constexpr SyscallABI syscall_abi(unsigned number, int64_t operation = -1) {
	switch (number) {
	case ECALL_PRINT: return {2, 0, false}; // Object stringification can re-enter.
	case ECALL_PRINT_CHANNEL: return {3, 0, false};
	case ECALL_VCALL: case ECALL_VCALL_SUPER: return {6, 0, false};
	case ECALL_VEVAL: return {4, 1, true};
	case ECALL_VASSIGN: return {2, 1, true};
	case ECALL_GET_OBJ: return {2, 1, true};
	case ECALL_GET_NODE: return {3, 1, true};
	case ECALL_THROW: return {6, 0, true};
	case ECALL_VCREATE: case ECALL_VCONSTRUCT: return {4, 0, true};
	case ECALL_VSTORE_GLOBAL: return {2, 0, true};
	case ECALL_ARRAY_OPS: return {4, 0, true};
	case ECALL_ARRAY_AT: return {3, 1, true};
	case ECALL_ARRAY_SIZE: return {1, 1, true};
	case ECALL_ARRAY_BATCH: return {4, 1, true};
	case ECALL_DICTIONARY_OPS:
		if (operation == int(Dictionary_Op::GET_SIZE)) return {2, 1, true};
		return {5, 1, true}; // some forms use a4
	case ECALL_STRING_AT: return {2, 1, true};
	case ECALL_STRING_SIZE: return {1, 1, true};
	case ECALL_STRING_BATCH: return {3, 1, true};
	case ECALL_STRING_CODEPOINT_BATCH: return {4, 1, true};
	case ECALL_VFETCH: return {5, 2, true};
	case ECALL_VSTORE: return {4, 0, true};
	case ECALL_NODE_CREATE: return {5, 1, true};
	case ECALL_CALLABLE_CREATE: return {4, 1, true};
	case ECALL_LOAD: return {3, 0, true};
	case ECALL_OBJ_PROP_GET: case ECALL_OBJ_PROP_SET: return {4, 0, false};
	case ECALL_VARIANT_GET: return {3, 1, false};
	case ECALL_VARIANT_SET: return {3, 0, false};
	case ECALL_SANDBOX_ADD: return {7, 0, true};
	case ECALL_CALL_GUEST: return {4, 0, false};
	case ECALL_VSCOPE:
		if (operation == int(Scope_Op::MARK)) return {1, 1, true};
		return {7, 1, false}; // RELEASE also reads tp
	case ECALL_OBJ_RETAIN: return {1, 0, true};
	case ECALL_CLASS_BIND: return {3, 0, true};
	case ECALL_OBJ_USES_TRAIT: return {5, 1, true};
	case ECALL_PACKED_ARRAY_OPS: return {3, 0, true};
	case ECALL_UTILITY:
		if (operation == int(Utility_Op::RANDI)) return {1, 1, true};
		if (operation == int(Utility_Op::RANDI_RANGE)) return {3, 1, true};
		if (operation == int(Utility_Op::NEAREST_PO2)) return {2, 1, true};
		switch (operation) {
		case int(Utility_Op::FLOOR):
		case int(Utility_Op::CEIL):
		case int(Utility_Op::ROUND):
		case int(Utility_Op::SIGN):
		case int(Utility_Op::SIN):
		case int(Utility_Op::COS):
		case int(Utility_Op::TAN):
		case int(Utility_Op::ASIN):
		case int(Utility_Op::ACOS):
		case int(Utility_Op::ATAN):
		case int(Utility_Op::SINH):
		case int(Utility_Op::COSH):
		case int(Utility_Op::TANH):
		case int(Utility_Op::ASINH):
		case int(Utility_Op::ACOSH):
		case int(Utility_Op::ATANH):
		case int(Utility_Op::EXP):
		case int(Utility_Op::LOG):
		case int(Utility_Op::DEG_TO_RAD):
		case int(Utility_Op::RAD_TO_DEG):
		case int(Utility_Op::LINEAR_TO_DB):
		case int(Utility_Op::DB_TO_LINEAR):
		case int(Utility_Op::IS_NAN):
		case int(Utility_Op::IS_INF):
		case int(Utility_Op::IS_FINITE):
		case int(Utility_Op::IS_ZERO_APPROX):
		case int(Utility_Op::EASE):
		case int(Utility_Op::STEP_DECIMALS):
		case int(Utility_Op::ATAN2):
		case int(Utility_Op::POW):
		case int(Utility_Op::FMOD):
		case int(Utility_Op::FPOSMOD):
		case int(Utility_Op::SNAPPED):
		case int(Utility_Op::IS_EQUAL_APPROX):
		case int(Utility_Op::ANGLE_DIFFERENCE):
		case int(Utility_Op::PINGPONG):
		case int(Utility_Op::LERP):
		case int(Utility_Op::INVERSE_LERP):
		case int(Utility_Op::SMOOTHSTEP):
		case int(Utility_Op::MOVE_TOWARD):
		case int(Utility_Op::LERP_ANGLE):
		case int(Utility_Op::ROTATE_TOWARD):
		case int(Utility_Op::WRAP):
		case int(Utility_Op::REMAP):
		case int(Utility_Op::CUBIC_INTERPOLATE):
		case int(Utility_Op::CUBIC_INTERPOLATE_ANGLE):
		case int(Utility_Op::CUBIC_INTERPOLATE_IN_TIME):
		case int(Utility_Op::CUBIC_INTERPOLATE_ANGLE_IN_TIME):
		case int(Utility_Op::BEZIER_INTERPOLATE):
		case int(Utility_Op::BEZIER_DERIVATIVE):
		case int(Utility_Op::RANDF):
		case int(Utility_Op::RANDF_RANGE):
		case int(Utility_Op::RANDFN):
			return {1, 0, true, true}; // op in a0, doubles in fa0-fa7 -> fa0
		default: return {4, 1, false}; // boxed operations can re-enter through Objects
		}
	case ECALL_BREAKPOINT: return {3, 0, false};
	case ECALL_AWAIT: return {6, 1, true};
	case ECALL_AWAIT_RESTORE: return {2, 1, true};
	default: return {8, 2, false};
	}
}

// Mirrors riscv::Dyncall without coupling the sandboxed compiler to the
// host runtime headers. The codegen test checks this against the vendored ABI.
constexpr uint32_t encode_counted_syscall(unsigned number, unsigned inputs, unsigned outputs,
		bool floats = true) {
	return (number << 20) | ((24u | (floats ? 4u : 0u) | outputs) << 15) | (7u << 12) |
		((16u | inputs) << 7) | 0x5b;
}

constexpr bool valid_counted_syscall(uint32_t word, int64_t operation = -1) {
	const auto abi = syscall_abi(word >> 20, operation);
	// Older counted ELFs synchronize FP even for integer-only handlers. Keep
	// accepting them, but never let a floating-point handler opt out.
	if (abi.floats && !(word & (1u << 17))) return false;
	word |= 1u << 17;
	return abi.counted && (word == encode_counted_syscall(word >> 20, abi.inputs, abi.outputs) ||
		((word >> 20) == ECALL_DICTIONARY_OPS && word == encode_counted_syscall(ECALL_DICTIONARY_OPS, 5, 1)));
}

// Decode-time check: a0 is not yet synchronized. Operation-specific promises
// are checked by the handler as well, once a0 has been saved by the emitter.
constexpr bool valid_counted_syscall_encoding(uint32_t word) {
	if (valid_counted_syscall(word)) return true;
	switch (word >> 20) {
	case ECALL_VSCOPE: return valid_counted_syscall(word, int(Scope_Op::MARK));
	case ECALL_DICTIONARY_OPS: return valid_counted_syscall(word, int(Dictionary_Op::GET_SIZE));
	case ECALL_UTILITY:
		return valid_counted_syscall(word, int(Utility_Op::RANDI)) ||
			valid_counted_syscall(word, int(Utility_Op::RANDI_RANGE)) ||
			valid_counted_syscall(word, int(Utility_Op::NEAREST_PO2)) ||
			valid_counted_syscall(word, int(Utility_Op::SIN));
	default: return false;
	}
}
}
