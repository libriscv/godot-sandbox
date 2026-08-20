#include "ir_verifier.h"
#include "compiler_exception.h"
#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gdscript {

namespace {

// A register's type as the verifier tracks it. UNKNOWN is the top of the
// lattice: it means "could be anything", and every check that consults a type
// passes when it sees UNKNOWN. Two paths that disagree merge to UNKNOWN.
constexpr IRInstruction::TypeHint TYPE_UNKNOWN = -2;

static_assert(TYPE_UNKNOWN != IRInstruction::TypeHint_NONE,
	"TypeHint_NONE is a real hint value and cannot double as 'unknown'");

// The state of every register at a point in the function.
struct RegisterState {
	// Whether the register is defined on every path reaching this point.
	std::vector<bool> defined;
	// The Variant type the register is known to hold, or TYPE_UNKNOWN.
	std::vector<IRInstruction::TypeHint> type;

	explicit RegisterState(size_t count)
		: defined(count, false), type(count, TYPE_UNKNOWN) {}

	// Intersection: a register is defined here only if it was defined on every
	// incoming path, and its type is known only if every path agreed on it.
	// Returns whether anything changed.
	bool merge_from(const RegisterState& other) {
		bool changed = false;
		for (size_t i = 0; i < defined.size(); i++) {
			if (defined[i] && !other.defined[i]) {
				defined[i] = false;
				changed = true;
			}
			if (type[i] != TYPE_UNKNOWN && type[i] != other.type[i]) {
				type[i] = TYPE_UNKNOWN;
				changed = true;
			}
		}
		return changed;
	}
};

struct BasicBlock {
	size_t begin = 0;         // First instruction index
	size_t end = 0;           // One past the last instruction index
	std::vector<size_t> successors;
	bool reachable = false;
	RegisterState entry { 0 };
	bool entry_initialized = false;
};

class Verifier {
public:
	Verifier(const IRFunction& func, const char* after_pass)
		: m_func(func), m_after_pass(after_pass) {}

	void run() {
		// max_registers has to cover everything before anything else can index
		// by register number.
		check_register_range();
		check_operands();
		check_labels();
		check_definedness_and_types();
	}

private:
	[[noreturn]] void fail(const std::string& message, size_t instr_idx) const {
		std::ostringstream oss;
		oss << "IR verification failed in function '" << m_func.name << "'";
		if (m_after_pass != nullptr) {
			oss << " after pass '" << m_after_pass << "'";
		}
		oss << ", instruction " << instr_idx;
		if (instr_idx < m_func.instructions.size()) {
			oss << " (" << m_func.instructions[instr_idx].to_string() << ")";
		}
		oss << ": " << message;
		throw CompilerException(ErrorType::IR_VERIFIER_ERROR, oss.str(), 0, 0, m_func.name);
	}

	[[noreturn]] void fail(const std::string& message) const {
		std::ostringstream oss;
		oss << "IR verification failed in function '" << m_func.name << "'";
		if (m_after_pass != nullptr) {
			oss << " after pass '" << m_after_pass << "'";
		}
		oss << ": " << message;
		throw CompilerException(ErrorType::IR_VERIFIER_ERROR, oss.str(), 0, 0, m_func.name);
	}

	size_t register_count() const {
		return static_cast<size_t>(std::max(m_func.max_registers, 0));
	}

	// -= max_registers covers every register mentioned =-
	void check_register_range() {
		if (m_func.max_registers < 0) {
			fail("max_registers is negative");
		}
		for (size_t i = 0; i < m_func.instructions.size(); i++) {
			for (const auto& operand : m_func.instructions[i].operands) {
				if (operand.type != IRValue::Type::REGISTER) {
					continue;
				}
				const int reg = std::get<int>(operand.value);
				if (reg < 0) {
					fail("negative register number r" + std::to_string(reg), i);
				}
				if (reg >= m_func.max_registers) {
					fail("register r" + std::to_string(reg) + " is outside max_registers (" +
						std::to_string(m_func.max_registers) + ")", i);
				}
			}
		}
	}

	// -= Arity and operand kinds match the signature =-
	void check_operands() {
		for (size_t i = 0; i < m_func.instructions.size(); i++) {
			const IRInstruction& instr = m_func.instructions[i];
			const IROpcodeInfo& info = ir_opcode_info(instr.opcode);
			const IROperandSignature& signature = info.signature;

			// RETURN is written both bare (returning whatever is in r0) and,
			// historically, with an operand. Everything else has to match.
			const size_t fixed = signature.fixed_count();
			if (instr.operands.size() < fixed) {
				fail(std::string(info.mnemonic) + " needs at least " + std::to_string(fixed) +
					" operands but has " + std::to_string(instr.operands.size()), i);
			}
			if (!signature.is_variadic() && instr.operands.size() > signature.count) {
				fail(std::string(info.mnemonic) + " takes " + std::to_string(signature.count) +
					" operands but has " + std::to_string(instr.operands.size()), i);
			}

			for (size_t j = 0; j < instr.operands.size(); j++) {
				const IROperandKind kind = signature.kind_at(j);
				if (!operand_matches(instr.operands[j], kind)) {
					fail(std::string(info.mnemonic) + " operand " + std::to_string(j) + ": expected " +
						ir_operand_kind_name(kind) + ", got " + instr.operands[j].to_string(), i);
				}
			}

			check_argument_count(instr, i);
			check_call_destination(instr, i);
		}
	}

	static bool operand_matches(const IRValue& operand, IROperandKind kind) {
		switch (kind) {
			case IROperandKind::DST:
			case IROperandKind::SRC:
			case IROperandKind::SRC_LIST:
				return operand.type == IRValue::Type::REGISTER;
			case IROperandKind::IMM:
			case IROperandKind::CNT:
			case IROperandKind::CNT2:
				return operand.type == IRValue::Type::IMMEDIATE;
			case IROperandKind::FIMM:
				return operand.type == IRValue::Type::FLOAT;
			case IROperandKind::STR:
				return operand.type == IRValue::Type::STRING;
			case IROperandKind::LBL:
				return operand.type == IRValue::Type::LABEL;
			case IROperandKind::ARG_LIST:
				// A syscall's arguments are registers or immediates, depending
				// on the syscall.
				return operand.type == IRValue::Type::REGISTER ||
					operand.type == IRValue::Type::IMMEDIATE;
			case IROperandKind::NONE:
				return false;
		}
		return false;
	}

	// A CNT / CNT2 operand states how long the trailing list is; a list that
	// does not match is a truncated or over-long instruction.
	void check_argument_count(const IRInstruction& instr, size_t instr_idx) {
		const IROperandSignature& signature = ir_opcode_info(instr.opcode).signature;
		for (size_t j = 0; j < signature.fixed_count(); j++) {
			const IROperandKind kind = signature.kinds[j];
			if (kind != IROperandKind::CNT && kind != IROperandKind::CNT2) {
				continue;
			}
			if (j >= instr.operands.size()) {
				return;
			}
			const int64_t declared = std::get<int64_t>(instr.operands[j].value);
			if (declared < 0) {
				fail("negative argument count " + std::to_string(declared), instr_idx);
			}
			const size_t multiplier = (kind == IROperandKind::CNT2) ? 2 : 1;
			const size_t expected = signature.fixed_count() + static_cast<size_t>(declared) * multiplier;
			if (instr.operands.size() != expected) {
				fail(std::string(ir_opcode_name(instr.opcode)) + " declares " + std::to_string(declared) +
					(multiplier == 2 ? " pairs" : " arguments") + " but has " +
					std::to_string(instr.operands.size()) + " operands, not " + std::to_string(expected),
					instr_idx);
			}
		}
	}

	// The register a call defines must not also be one of its arguments: the
	// backend writes the result into it, so an argument sharing the register is
	// read after it has already been overwritten.
	void check_call_destination(const IRInstruction& instr, size_t instr_idx) {
		if (!ir_has_effect(instr.opcode, IR_CALL)) {
			return;
		}
		const int dst = ir_destination_register(instr);
		if (dst < 0) {
			return;
		}
		std::vector<int> reads;
		ir_collect_read_registers(instr, reads);
		for (int reg : reads) {
			if (reg == dst) {
				fail(std::string(ir_opcode_name(instr.opcode)) + " defines r" + std::to_string(dst) +
					" and also reads it as an argument", instr_idx);
			}
		}
	}

	// -= Labels are defined exactly once, and every branch target exists =-
	void check_labels() {
		m_label_index.clear();
		for (size_t i = 0; i < m_func.instructions.size(); i++) {
			const IRInstruction& instr = m_func.instructions[i];
			if (!ir_has_effect(instr.opcode, IR_LABEL)) {
				continue;
			}
			const std::string& name = std::get<std::string>(instr.operands.at(0).value);
			if (m_label_index.count(name) != 0) {
				fail("label '" + name + "' is defined more than once", i);
			}
			m_label_index[name] = i;
		}

		for (size_t i = 0; i < m_func.instructions.size(); i++) {
			const IRInstruction& instr = m_func.instructions[i];
			if (ir_has_effect(instr.opcode, IR_LABEL)) {
				continue;
			}
			for (const auto& operand : instr.operands) {
				if (operand.type != IRValue::Type::LABEL) {
					continue;
				}
				const std::string& name = std::get<std::string>(operand.value);
				if (m_label_index.count(name) == 0) {
					fail("branch target '" + name + "' has no label", i);
				}
			}
		}
	}

	// -= Every register read is defined on every path that reaches the read =-
	void check_definedness_and_types() {
		build_blocks();

		const size_t regs = register_count();

		// The entry block starts with the parameters defined and nothing else.
		RegisterState entry(regs);
		for (size_t i = 0; i < m_func.parameters.size() && i < regs; i++) {
			entry.defined[i] = true;
		}

		if (m_blocks.empty()) {
			return;
		}

		m_blocks[0].entry = entry;
		m_blocks[0].entry_initialized = true;
		m_blocks[0].reachable = true;

		// Forward dataflow to a fixpoint. Blocks are few and the lattice is
		// short, so a simple worklist is enough.
		std::vector<size_t> worklist { 0 };
		while (!worklist.empty()) {
			const size_t index = worklist.back();
			worklist.pop_back();

			RegisterState state = m_blocks[index].entry;
			transfer(m_blocks[index], state, /*report=*/false);

			for (size_t successor : m_blocks[index].successors) {
				BasicBlock& target = m_blocks[successor];
				bool changed = false;
				if (!target.entry_initialized) {
					target.entry = state;
					target.entry_initialized = true;
					target.reachable = true;
					changed = true;
				} else {
					changed = target.entry.merge_from(state);
				}
				if (changed) {
					worklist.push_back(successor);
				}
			}
		}

		// Now walk each reachable block once more, reporting.
		for (auto& block : m_blocks) {
			if (!block.reachable) {
				// Unreachable code is not wrong on its own -- a pass can leave
				// a block stranded -- and nothing in it can be executed, so
				// there is nothing to check.
				continue;
			}
			RegisterState state = block.entry;
			transfer(block, state, /*report=*/true);
		}
	}

	void build_blocks() {
		m_blocks.clear();
		const size_t count = m_func.instructions.size();
		if (count == 0) {
			return;
		}

		// Leaders: the first instruction, every label, and everything that
		// follows a branch or a jump.
		std::vector<bool> is_leader(count, false);
		is_leader[0] = true;
		for (size_t i = 0; i < count; i++) {
			const IROpcode op = m_func.instructions[i].opcode;
			if (ir_has_effect(op, IR_LABEL)) {
				is_leader[i] = true;
			}
			if (ir_has_effect(op, IR_BRANCH) || ir_has_effect(op, IR_TERMINATOR)) {
				if (i + 1 < count) {
					is_leader[i + 1] = true;
				}
			}
		}

		std::unordered_map<size_t, size_t> block_of_instruction;
		for (size_t i = 0; i < count; i++) {
			if (is_leader[i]) {
				BasicBlock block;
				block.begin = i;
				block.end = i;
				m_blocks.push_back(block);
			}
			block_of_instruction[i] = m_blocks.size() - 1;
			m_blocks.back().end = i + 1;
		}

		for (size_t b = 0; b < m_blocks.size(); b++) {
			BasicBlock& block = m_blocks[b];
			const IRInstruction& last = m_func.instructions[block.end - 1];

			// A branch target is a successor, and so is the next block unless
			// control cannot fall through.
			for (const auto& operand : last.operands) {
				if (operand.type != IRValue::Type::LABEL) {
					continue;
				}
				if (ir_has_effect(last.opcode, IR_LABEL)) {
					continue; // The label's own operand names itself.
				}
				auto it = m_label_index.find(std::get<std::string>(operand.value));
				if (it != m_label_index.end()) {
					block.successors.push_back(block_of_instruction[it->second]);
				}
			}

			const bool falls_through = !ir_has_effect(last.opcode, IR_TERMINATOR);
			if (falls_through && b + 1 < m_blocks.size()) {
				block.successors.push_back(b + 1);
			}
		}

		for (auto& block : m_blocks) {
			block.entry = RegisterState(register_count());
		}
	}

	// Run one block's instructions over `state`, optionally reporting problems.
	void transfer(const BasicBlock& block, RegisterState& state, bool report) {
		const size_t regs = state.defined.size();
		std::vector<int> reads;

		for (size_t i = block.begin; i < block.end; i++) {
			const IRInstruction& instr = m_func.instructions[i];

			// A bare RETURN returns whatever is in r0, including nothing at
			// all in a function that returns no value, so its implicit read is
			// not a read that has to be defined.
			const bool implicit_return_read =
				instr.opcode == IROpcode::RETURN && instr.operands.empty();

			if (report && !implicit_return_read) {
				reads.clear();
				ir_collect_read_registers(instr, reads);
				for (int reg : reads) {
					if (reg >= 0 && static_cast<size_t>(reg) < regs && !state.defined[reg]) {
						fail("r" + std::to_string(reg) + " is read but is not defined on every path "
							"that reaches this instruction", i);
					}
				}
				check_type_hint(instr, state, i);
			}

			const int dst = ir_destination_register(instr);
			if (dst >= 0 && static_cast<size_t>(dst) < regs) {
				state.defined[dst] = true;
				state.type[dst] = result_type(instr, state);
			}
		}
	}

	// The type the instruction's destination is known to hold afterwards.
	IRInstruction::TypeHint result_type(const IRInstruction& instr, const RegisterState& state) const {
		switch (instr.opcode) {
			case IROpcode::LOAD_IMM: return Variant::INT;
			case IROpcode::LOAD_FLOAT_IMM: return Variant::FLOAT;
			case IROpcode::LOAD_BOOL: return Variant::BOOL;
			case IROpcode::LOAD_STRING: return Variant::STRING;
			case IROpcode::CONVERT: return instr.type_hint;
			case IROpcode::MOVE: {
				const int src = std::get<int>(instr.operands.at(1).value);
				if (src >= 0 && static_cast<size_t>(src) < state.type.size()) {
					return state.type[src];
				}
				return TYPE_UNKNOWN;
			}
			case IROpcode::MAKE_VECTOR2: return Variant::VECTOR2;
			case IROpcode::MAKE_VECTOR3: return Variant::VECTOR3;
			case IROpcode::MAKE_VECTOR4: return Variant::VECTOR4;
			case IROpcode::MAKE_VECTOR2I: return Variant::VECTOR2I;
			case IROpcode::MAKE_VECTOR3I: return Variant::VECTOR3I;
			case IROpcode::MAKE_VECTOR4I: return Variant::VECTOR4I;
			case IROpcode::MAKE_COLOR: return Variant::COLOR;
			case IROpcode::MAKE_ARRAY: return Variant::ARRAY;
			case IROpcode::MAKE_DICTIONARY: return Variant::DICTIONARY;
			default:
				break;
		}
		// A comparison always produces a boolean; its type_hint describes the
		// operands it compares, not its result.
		if (ir_has_effect(instr.opcode, IR_COMPARISON)) {
			return Variant::BOOL;
		}
		if (ir_has_effect(instr.opcode, IR_ARITHMETIC) && instr.type_hint != IRInstruction::TypeHint_NONE) {
			return instr.type_hint;
		}
		return TYPE_UNKNOWN;
	}

	// -= Type hints are consistent with the opcode =-
	void check_type_hint(const IRInstruction& instr, const RegisterState& state, size_t instr_idx) {
		if (instr.opcode == IROpcode::CONVERT) {
			if (instr.type_hint == IRInstruction::TypeHint_NONE) {
				fail("CONVERT does not say what it converts to", instr_idx);
			}
			return;
		}

		// A hint on an arithmetic or comparison opcode is what puts the backend
		// on a native path instead of a Variant one, so it is a claim about the
		// operands. Register types leaking between functions is what made that
		// claim false and picked an integer path for a Variant that was not an
		// integer; here the claim is checked against what the operands are known
		// to hold.
		if (instr.type_hint == IRInstruction::TypeHint_NONE) {
			return;
		}
		if (!ir_has_effect(instr.opcode, IR_ARITHMETIC)) {
			return;
		}

		std::vector<int> reads;
		ir_collect_read_registers(instr, reads);
		for (int reg : reads) {
			if (reg < 0 || static_cast<size_t>(reg) >= state.type.size()) {
				continue;
			}
			const IRInstruction::TypeHint known = state.type[reg];
			if (known == TYPE_UNKNOWN) {
				continue;
			}
			if (known != instr.type_hint) {
				fail(std::string(ir_opcode_name(instr.opcode)) + " is hinted " +
					variant_type_name(instr.type_hint) + " but r" + std::to_string(reg) +
					" holds " + variant_type_name(known), instr_idx);
			}
		}
	}

	const IRFunction& m_func;
	const char* m_after_pass;
	std::unordered_map<std::string, size_t> m_label_index;
	std::vector<BasicBlock> m_blocks;
};

bool g_verification_enabled =
#ifdef NDEBUG
	false;
#else
	true;
#endif

} // namespace

bool ir_verification_enabled() {
	return g_verification_enabled;
}

void set_ir_verification_enabled(bool enabled) {
	g_verification_enabled = enabled;
}

void ir_verify(const IRFunction& func, const char* after_pass) {
	Verifier verifier(func, after_pass);
	verifier.run();
}

void ir_verify(const IRProgram& program, const char* after_pass) {
	for (const auto& func : program.functions) {
		ir_verify(func, after_pass);
	}
	if (program.has_global_init) {
		ir_verify(program.global_init, after_pass);
	}
}

} // namespace gdscript
