#pragma once
#include "call_abi.h"
#include "export_hints.h"
#include "small_vector.h"
#include <vector>
#include <string>
#include <cstdint>
#include <variant>
#include <unordered_map>
#include <memory>
#include "function_signature.h"
#include "variant_types.h"

namespace gdscript {

// Generated from ir_opcodes.def.
enum class IROpcode {
#define IR_OPCODE(name, mnemonic, sig, effects) name,
#include "ir_opcodes.def"
#undef IR_OPCODE
};

// Table sizing and completeness checks.
enum : size_t {
	IR_OPCODE_COUNT =
#define IR_OPCODE(name, mnemonic, sig, effects) + 1
#include "ir_opcodes.def"
#undef IR_OPCODE
};

// Operand role. All read/write queries go through these.
enum class IROperandKind : uint8_t {
	NONE,
	DST,
	// Read-modify-write: copy propagation and DCE must preserve the register.
	INOUT,
	SRC,
	IMM,
	FIMM,
	STR,
	LBL,
	CNT,       // trailing-list length
	CNT2,      // trailing-list pair count
	SRC_LIST,
	ARG_LIST,  // registers and/or immediates
	LBL_LIST,
};

enum IREffect : uint32_t {
	IR_PURE          = 0,
	IR_SIDE_EFFECTS  = 1u << 0,
	IR_BRANCH        = 1u << 1,
	IR_FUSED_BRANCH  = 1u << 2,
	IR_TERMINATOR    = 1u << 3,
	IR_LABEL         = 1u << 4,
	IR_CALL          = 1u << 5,
	IR_READS_GLOBAL  = 1u << 6,
	IR_WRITES_GLOBAL = 1u << 7,
	IR_ARITHMETIC    = 1u << 8,   // foldable when operands are known
	IR_COMPARISON    = 1u << 9,   // result is boolean 0/1
	IR_SIMPLE_LOAD   = 1u << 10,  // loads and MOVE; no computation
};

struct IROperandSignature {
	static constexpr size_t MAX_OPERANDS = 8;

	IROperandKind kinds[MAX_OPERANDS] {};
	uint8_t count = 0;

	constexpr IROperandSignature() = default;

	template <typename... Kinds>
	constexpr IROperandSignature(Kinds... kinds_in)
		: kinds { kinds_in... }, count(sizeof...(Kinds))
	{
		static_assert(sizeof...(Kinds) <= MAX_OPERANDS, "IR opcode has too many operands");
	}

	// Ends in a variadic list.
	constexpr bool is_variadic() const {
		return count > 0 &&
			(kinds[count - 1] == IROperandKind::SRC_LIST ||
			 kinds[count - 1] == IROperandKind::ARG_LIST ||
			 kinds[count - 1] == IROperandKind::LBL_LIST);
	}

	// Operands before the repeating tail.
	constexpr size_t fixed_count() const {
		return is_variadic() ? static_cast<size_t>(count) - 1 : static_cast<size_t>(count);
	}

	// Role of operand `index`; NONE past the end.
	constexpr IROperandKind kind_at(size_t index) const {
		if (index < fixed_count()) {
			return kinds[index];
		}
		if (is_variadic()) {
			return kinds[count - 1];
		}
		return IROperandKind::NONE;
	}
};

struct IROpcodeInfo {
	IROpcode opcode;
	const char* mnemonic;
	IROperandSignature signature;
	uint32_t effects;
};

// Lookup; every opcode has an entry, checked at build time.
const IROpcodeInfo& ir_opcode_info(IROpcode op);

inline bool ir_has_effect(IROpcode op, IREffect effect) {
	return (ir_opcode_info(op).effects & effect) != 0;
}

inline bool ir_is_pure(IROpcode op) {
	return (ir_opcode_info(op).effects & IR_SIDE_EFFECTS) == 0;
}

inline bool ir_is_control_flow(IROpcode op) {
	return (ir_opcode_info(op).effects & (IR_LABEL | IR_BRANCH | IR_TERMINATOR)) != 0;
}

// LABEL/VARIABLE/STRING operand names, interned. Keeps IRValue a 16-byte POD:
// no allocation per operand, integer compare and hash on labels.
//
// Not IRProgram::string_constants -- that one's indices are the guest constant
// pool's ELF layout, and only LOAD_STRING operands belong in it.
class IRStringTable {
public:
	// No operand carries it; identify_loops() uses it for "no exit label".
	static constexpr uint32_t INVALID_ID = UINT32_MAX;

	uint32_t intern(const std::string& text) {
		auto it = m_ids.find(text);
		if (it != m_ids.end()) {
			return it->second;
		}
		const uint32_t id = static_cast<uint32_t>(m_strings.size());
		m_strings.push_back(text);
		m_ids.emplace(text, id);
		return id;
	}

	const std::string& operator[](uint32_t id) const {
		static const std::string missing;
		return id < m_strings.size() ? m_strings[id] : missing;
	}

	size_t size() const { return m_strings.size(); }

private:
	std::vector<std::string> m_strings;
	std::unordered_map<std::string, uint32_t> m_ids;
};

struct IRValue {
	enum class Type : uint8_t {
		REGISTER,
		IMMEDIATE,
		FLOAT,
		LABEL,
		VARIABLE,
		STRING
	};

	Type type = Type::IMMEDIATE;
	union {
		int reg_value;
		int64_t imm_value;
		double float_value;
		// LABEL, VARIABLE, STRING: IRStringTable id.
		uint32_t string_id;
	};

	IRValue() : type(Type::IMMEDIATE), imm_value(0) {}

	static IRValue reg(int r) {
		IRValue v;
		v.type = Type::REGISTER;
		v.reg_value = r;
		return v;
	}

	static IRValue imm(int64_t i) {
		IRValue v;
		v.type = Type::IMMEDIATE;
		v.imm_value = i;
		return v;
	}

	static IRValue fimm(double d) {
		IRValue v;
		v.type = Type::FLOAT;
		v.float_value = d;
		return v;
	}

	static IRValue label(uint32_t id) {
		IRValue v;
		v.type = Type::LABEL;
		v.string_id = id;
		return v;
	}

	static IRValue var(uint32_t id) {
		IRValue v;
		v.type = Type::VARIABLE;
		v.string_id = id;
		return v;
	}

	static IRValue str(uint32_t id) {
		IRValue v;
		v.type = Type::STRING;
		v.string_id = id;
		return v;
	}

	int reg_index() const { return reg_value; }
	int64_t immediate() const { return imm_value; }
	double float_number() const { return float_value; }

	bool operator==(const IRValue& other) const {
		if (type != other.type) {
			return false;
		}
		switch (type) {
			case Type::REGISTER: return reg_value == other.reg_value;
			case Type::IMMEDIATE: return imm_value == other.imm_value;
			case Type::FLOAT: return float_value == other.float_value;
			case Type::LABEL:
			case Type::VARIABLE:
			case Type::STRING: return string_id == other.string_id;
		}
		return false;
	}

	std::string to_string(const IRStringTable* strings = nullptr) const;
};

struct IRInstruction {
	// Variant::Type, or -1 for NONE.
	static constexpr int32_t TypeHint_NONE = -1;
	using TypeHint = int32_t;

	IROpcode opcode {};
	// 3 operands or fewer covers 92% of instructions; those allocate nothing.
	SmallVector<IRValue, 3> operands;
	TypeHint type_hint = TypeHint_NONE;
	// Source types for mixed typed binary operations. Most instructions leave
	// these unset; vector/scalar lowering uses them to select the broadcast side
	// without a run-time tag test.
	TypeHint lhs_type_hint = TypeHint_NONE;
	TypeHint rhs_type_hint = TypeHint_NONE;

	// 1-based source line; 0 for prologue/synthesised. Metadata only.
	int32_t line = 0;

	// 1-based position in the unoptimized body, stamped before the optimizer
	// runs and carried through copies. Debug ranges are recorded as positions in
	// that body, and passes insert and delete; this is how they are found again.
	// 0 means an instruction a pass synthesised. Not part of equality.
	uint32_t debug_order = 0;

	// VCALL only: `super.method()` on a native base. The object carries the
	// class's own script instance, which would answer the call and recurse back
	// into the method that made it, so the host bypasses it for this one call.
	bool super_call = false;

	// CALL only: every typed argument already has the callee's declared type, so
	// the backend may use the callee's trusted entry past parameter coercion.
	bool trusted_internal_call = false;

	IRInstruction(IROpcode op) : opcode(op) {}
	IRInstruction(IROpcode op, IRValue a) : opcode(op), operands{a} {}
	IRInstruction(IROpcode op, IRValue a, IRValue b) : opcode(op), operands{a, b} {}
	IRInstruction(IROpcode op, IRValue a, IRValue b, IRValue c) : opcode(op), operands{a, b, c} {}

	// Without a table, names render as their id.
	std::string to_string(const IRStringTable* strings = nullptr) const;

	bool operator==(const IRInstruction& other) const {
		if (opcode != other.opcode || type_hint != other.type_hint ||
			lhs_type_hint != other.lhs_type_hint || rhs_type_hint != other.rhs_type_hint ||
			line != other.line || super_call != other.super_call ||
			trusted_internal_call != other.trusted_internal_call ||
			operands.size() != other.operands.size()) {
			return false;
		}
		for (size_t i = 0; i < operands.size(); i++) {
			if (!(operands[i] == other.operands[i])) {
				return false;
			}
		}
		return true;
	}
};

struct IRFunction {
	struct DebugLocal {
		std::string name;
		int register_num = -1;
		IRInstruction::TypeHint type_hint = IRInstruction::TypeHint_NONE;
		size_t begin_instruction = 0;
		size_t end_instruction = SIZE_MAX; // exclusive
		bool parameter = false;
	};

	std::string name;
	std::vector<std::string> parameters;
	std::vector<IRInstruction> instructions;
	std::vector<DebugLocal> debug_locals;
	int max_registers = 0;
	IRInstruction::TypeHint return_type_hint = IRInstruction::TypeHint_NONE;
	std::vector<uint64_t> param_sets;
	uint64_t return_set = 0;
	// Has AWAIT; gets a resume entry, all parameters forced live.
	bool is_coroutine = false;

	// Parameters in r0..N-1, return value in r0.
	static constexpr int RETURN_REGISTER = 0;

	// Sandbox ABI: a0=return, a1-a7=the first arguments, then pointers at 0(sp).
	static constexpr size_t REGISTER_PARAMETERS = CallABI::REGISTER_ARGUMENTS;
	static constexpr size_t MAX_PARAMETERS = CallABI::MAX_ARGUMENTS;
};

struct IRGlobalVar {
	std::string name;
	std::string class_name;
	uint32_t declaration_line = 0;
	bool is_const = false;
	bool is_property = false;
	bool is_static = false;

	enum class Storage : uint8_t {
		Data,
		Instance,
	};
	Storage storage = Storage::Data;
	bool is_member() const { return storage == Storage::Instance; }

	bool publishes_to_host() const { return is_property || (is_member() && !is_const); }

	IRInstruction::TypeHint type_hint = IRInstruction::TypeHint_NONE;
	uint64_t declared_set = 0;

	enum class InitType {
		NONE,
		INT,
		FLOAT,
		STRING,
		BOOL,
		NULL_VAL,
		EMPTY_ARRAY,
		EMPTY_DICT,
		RUNTIME         // evaluated by global_init at startup
	};
	InitType init_type = InitType::NONE;
	std::variant<int64_t, double, std::string, bool> init_value;

	// Drives @export registration and VASSIGN decisions. TypeHint_NONE = any Variant.
	IRInstruction::TypeHint value_type = IRInstruction::TypeHint_NONE;

	bool holds_object = false;

	std::string setter_function;
	std::string getter_function;

	ExportHint export_hint;
};

struct IRProgram {
	bool is_tool = false;
	std::string class_name;
	std::string base_class;
	bool base_is_path = false;
	std::string native_base_class;
	bool native_base_is_path = false;
	std::vector<IRGlobalVar> globals;
	std::vector<IRFunction> functions;
	std::vector<std::string> string_constants;

	// Label, variable and operand names.
	IRStringTable strings;

	// One entry per function, same order.
	std::vector<FunctionSignature> signatures;

	std::vector<FunctionSignature> signals;

	// Top-level methods marked @rpc. Displaced base implementations are omitted;
	// only the method visible on the final script can be remotely invoked.
	std::vector<RPCConfig> rpc_configs;

	// One per nested class with an engine base; the host attaches a Script to each.
	std::vector<ClassSignature> class_signatures;
	// Compiler-only declarations are also published for editor completion and
	// supply the method-name table used by TRAIT_TEST.
	std::vector<ClassSignature> trait_signatures;
	std::vector<std::string> script_uses;
	bool trait_structural_fallback = true;

	// File-scope `const` and `enum`. Compile-time only in the guest; published so
	// the host can answer `Autoload.NAME` the way GDScript's Script::constants does.
	std::vector<ScriptConstant> constants;

	// Evaluates non-constant global initializers at startup, before @export registration.
	IRFunction global_init;
	bool has_global_init = false;

	IRFunction member_init;
	bool has_member_init = false;

	bool has_breakpoint_statement = false;
};

const char* ir_opcode_name(IROpcode op);

// For verifier diagnostics.
const char* ir_operand_kind_name(IROperandKind kind);

// For diagnostics and dumps.
const char* variant_type_name(IRInstruction::TypeHint hint);

// Fixed-width register bitset for the liveness analyses. Sized once from
// max_registers; merges word-wide, allocates nothing per instruction visit.
class RegisterSet {
public:
	RegisterSet() = default;
	explicit RegisterSet(size_t bits) { resize(bits); }

	void resize(size_t bits) {
		m_bits = bits;
		m_words.assign((bits + 63) / 64, 0);
	}

	size_t size() const { return m_bits; }

	void set(size_t bit) { m_words[bit >> 6] |= uint64_t(1) << (bit & 63); }
	void reset(size_t bit) { m_words[bit >> 6] &= ~(uint64_t(1) << (bit & 63)); }
	bool test(size_t bit) const { return (m_words[bit >> 6] >> (bit & 63)) & 1; }

	void clear() {
		for (uint64_t& word : m_words) {
			word = 0;
		}
	}

	void set_all() {
		for (uint64_t& word : m_words) {
			word = ~uint64_t(0);
		}
		trim();
	}

	bool any() const {
		for (uint64_t word : m_words) {
			if (word != 0) {
				return true;
			}
		}
		return false;
	}

	RegisterSet& operator|=(const RegisterSet& other) {
		for (size_t i = 0; i < m_words.size(); i++) {
			m_words[i] |= other.m_words[i];
		}
		return *this;
	}

	bool operator==(const RegisterSet& other) const { return m_words == other.m_words; }
	bool operator!=(const RegisterSet& other) const { return m_words != other.m_words; }

private:
	void trim() {
		const size_t tail = m_bits & 63;
		if (tail != 0 && !m_words.empty()) {
			m_words.back() &= (uint64_t(1) << tail) - 1;
		}
	}

	std::vector<uint64_t> m_words;
	size_t m_bits = 0;
};

// Operand role queries. All are ir_opcodes.def lookups.
// DST position varies by opcode (CALL uses operand 1, VSET has none);
// hardcoding "operand 0 = DST" miscompiles those opcodes.
int ir_destination_operand_index(IROpcode op);

int ir_destination_register(const IRInstruction& instr);

bool ir_reads_operand(const IRInstruction& instr, size_t index);

// True for DST or INOUT operands; substituting these redirects the write.
bool ir_writes_operand(const IRInstruction& instr, size_t index);

// Bare RETURN implicitly reads r0.
void ir_collect_read_registers(const IRInstruction& instr, std::vector<int>& out);

// Per-instruction purity. Unlike ir_is_pure(), checks GLOBAL_CALL's GlobalFn:
// impure globals (randi etc.) are not deletable even when their result is unused.
bool ir_instruction_is_pure(const IRInstruction& instr);

namespace TypeHintUtils {
	inline bool is_variant(IRInstruction::TypeHint hint) {
		return hint != IRInstruction::TypeHint_NONE;
	}

	inline bool is_vector(IRInstruction::TypeHint hint) {
		return hint == Variant::VECTOR2 ||
		       hint == Variant::VECTOR3 ||
		       hint == Variant::VECTOR4 ||
		       hint == Variant::COLOR ||
		       hint == Variant::VECTOR2I ||
		       hint == Variant::VECTOR3I ||
		       hint == Variant::VECTOR4I;
	}

	inline bool is_int_vector(IRInstruction::TypeHint hint) {
		return hint == Variant::VECTOR2I ||
		       hint == Variant::VECTOR3I ||
		       hint == Variant::VECTOR4I;
	}

	inline bool is_float_vector(IRInstruction::TypeHint hint) {
		return hint == Variant::VECTOR2 ||
		       hint == Variant::VECTOR3 ||
		       hint == Variant::VECTOR4 ||
		       hint == Variant::COLOR;
	}
} // namespace TypeHintUtils

} // namespace gdscript
