#include "riscv_codegen.h"
#include "compiler_exception.h"
#include "variant_types.h"
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <climits>

namespace gdscript {

RISCVCodeGen::RISCVCodeGen(const VariantLayout& layout) :
		m_layout(layout) {}

size_t RISCVCodeGen::add_constant(int64_t value) {
	// Check if constant already exists in pool
	auto it = m_constant_pool_map.find(value);
	if (it != m_constant_pool_map.end()) {
		return it->second;
	}

	// Add new constant
	size_t index = m_constant_pool.size();
	m_constant_pool.push_back(value);
	m_constant_pool_map[value] = index;
	return index;
}

std::string RISCVCodeGen::gen_local_label(const std::string& prefix) {
	return prefix + std::to_string(m_label_counter++);
}

void RISCVCodeGen::emit_variant_component_to_real(int comp_offset, int result_offset, int store_offset, bool normalize_by_255) {
	// Convert a Variant component (at comp_offset) to real_t and store to result_offset + store_offset
	// Handles both INT (type=2) and FLOAT (type=3) Variants
	// If normalize_by_255 is true, divides INTEGER values by 255.0 (for Color components)
	std::string label_float = gen_local_label(".float");
	std::string label_cont = gen_local_label(".cont");

	// Load the component's type field
	emit_lwu(REG_T0, REG_SP, comp_offset); // Load type (4 bytes, zero-extended)

	// Check if type is INT (2) or FLOAT (3)
	// Branch if INT (subtract 2, so 0 means INT, non-zero means FLOAT)
	emit_addi(REG_T1, REG_T0, -2);
	mark_label_use(label_float, m_code.size());
	emit_bne(REG_T1, REG_ZERO, 0); // Branch if not INT (i.e., if FLOAT)

	// INT case: Load integer and convert to float
	emit_ld(REG_T0, REG_SP, comp_offset + 8); // Load int64_t
	emit_fcvt_d_l(REG_FA0, REG_T0); // Convert int64 to double

	if (normalize_by_255) {
		// For Color, normalize integer values by dividing by 255.0
		// Load 255.0 from constant pool
		size_t const_idx = add_constant(0x406FC00000000000LL); // 255.0 as double
		std::string label_255 = ".LC" + std::to_string(const_idx);
		emit_la(REG_T0, label_255);
		emit_fld(REG_FA1, REG_T0, 0); // Load 255.0 into FA1
		emit_fdiv_d(REG_FA0, REG_FA0, REG_FA1); // Divide by 255.0
	}

	emit_fcvt_r_d(REG_FA0, REG_FA0); // Narrow to real_t (no-op in double-precision builds)
	mark_label_use(label_cont, m_code.size());
	emit_jal(REG_ZERO, 0); // Skip to end

	// FLOAT case: Load the double (Variant::FLOAT is always 64-bit) and narrow to real_t
	define_label(label_float);
	emit_fld(REG_FA0, REG_SP, comp_offset + VARIANT_DATA_OFFSET); // Load double
	// Note: We do NOT normalize float values - they're already in the correct range
	emit_fcvt_r_d(REG_FA0, REG_FA0); // Narrow to real_t (no-op in double-precision builds)
	define_label(label_cont);

	emit_fsr(REG_FA0, REG_SP, result_offset + store_offset); // Store real_t
}

std::vector<uint8_t> RISCVCodeGen::generate(const IRProgram& program) {
	m_code.clear();
	m_labels.clear();
	m_label_uses.clear();
	m_functions.clear();
	m_fn = FunctionState {};
	m_constant_pool.clear();
	m_constant_pool_map.clear();
	m_string_constants = &program.string_constants;

	// Entry point: Initialize global variables, then STOP
	// Store globals info early so we can reference it during init
	m_global_count = program.globals.size();
	m_globals = program.globals;

	// Generate initialization code for globals at entry point
	// We'll calculate the .globals virtual address when we define it later
	// For now, emit placeholder that will be resolved during label resolution
	for (size_t i = 0; i < program.globals.size(); i++) {
		const auto& global = program.globals[i];

		// NONE stays NIL; RUNTIME is written by the global init function below.
		if (global.init_type == IRGlobalVar::InitType::NONE ||
		    global.init_type == IRGlobalVar::InitType::RUNTIME) {
			continue;
		}

		// Load address of global variable into t0
		// Address = .globals + (i * sizeof(Variant)). The index is folded into
		// the relocation so that programs with many globals still address the
		// tail of the array correctly.
		emit_address_of_global(REG_T0, i);

		// Initialize based on type
		if (global.init_type == IRGlobalVar::InitType::INT) {
			// Write type field: m_type = 2 (INT)
			emit_li(REG_T1, Variant::INT);
			emit_sw(REG_T1, REG_T0, 0);

			// Write value: v.i at offset 8
			int64_t value = std::get<int64_t>(global.init_value);
			emit_li(REG_T1, value);
			emit_sd(REG_T1, REG_T0, 8);
		} else if (global.init_type == IRGlobalVar::InitType::FLOAT) {
			// Write type field: m_type = 3 (FLOAT)
			emit_li(REG_T1, Variant::FLOAT);
			emit_sw(REG_T1, REG_T0, 0);

			// Write value: v.f (64-bit double) at offset 8
			double value = std::get<double>(global.init_value);
			int64_t bits;
			memcpy(&bits, &value, sizeof(double));
			emit_li(REG_T1, bits);
			emit_sd(REG_T1, REG_T0, 8);
		} else if (global.init_type == IRGlobalVar::InitType::BOOL) {
			// Write type field: m_type = 1 (BOOL)
			emit_li(REG_T1, Variant::BOOL);
			emit_sw(REG_T1, REG_T0, 0);

			// Write value: v.b at offset 8
			bool value = std::get<bool>(global.init_value);
			emit_li(REG_T1, value ? 1 : 0);
			emit_sd(REG_T1, REG_T0, 8);
		} else if (global.init_type == IRGlobalVar::InitType::STRING) {
			// String initialization using VCREATE
			std::string str_value = std::get<std::string>(global.init_value);
			int str_len = static_cast<int>(str_value.length());

			// Allocate stack space for: string data + struct { char*, size_t }
			int str_space = ((str_len + 1) + 7) & ~7; // String + null terminator, aligned to 8 bytes
			int struct_space = 16; // Two 8-byte fields
			int total_space = str_space + struct_space;
			total_space = (total_space + 15) & ~15; // Align to 16 bytes

			// Adjust stack pointer
			emit_add_offset(REG_SP, REG_SP, -total_space);

			// Store string data on stack
			for (size_t j = 0; j < str_value.length(); j++) {
				emit_li(REG_T2, static_cast<unsigned char>(str_value[j]));
				emit_sb(REG_T2, REG_SP, j);
			}
			// Store null terminator
			emit_sb(REG_ZERO, REG_SP, str_len);

			// Create struct at sp + str_space
			// struct.str = sp (pointer to string data)
			emit_mv(REG_T2, REG_SP); // T2 = pointer to the string data at sp
			emit_sd(REG_T2, REG_SP, str_space); // Store pointer

			// struct.length = str_len
			emit_li(REG_T2, str_len);
			emit_sd(REG_T2, REG_SP, str_space + 8); // Store length

			// Prepare data pointer in T1 for VCREATE
			emit_add_offset(REG_T1, REG_SP, str_space);

			// Calculate the offset to the global Variant after stack adjustment
			// T0 still points to the global Variant (loaded with la before stack adjustment)
			// We need the offset from current SP to pass to emit_vcreate_syscall
			// Since T0 is an absolute address, we can't use it directly with emit_vcreate_syscall
			// We need to use VCREATE directly here

			// a0 = T0 (pointer to destination Variant - already calculated)
			emit_mv(REG_A0, REG_T0);

			// a1 = Variant::STRING
			emit_li(REG_A1, Variant::STRING);

			// a2 = method (1 for const char* + size_t)
			emit_li(REG_A2, 1);

			// a3 = pointer to struct (T1)
			emit_mv(REG_A3, REG_T1);

			// a7 = ECALL_VCREATE (517)
			emit_li(REG_A7, 517);
			emit_ecall();

			// Restore stack pointer
			emit_add_offset(REG_SP, REG_SP, total_space);
		} else if (global.init_type == IRGlobalVar::InitType::EMPTY_ARRAY) {
			// Empty Array initialization using VCREATE
			// a0 = pointer to destination Variant (T0 already points to it)
			emit_mv(REG_A0, REG_T0);

			// a1 = Variant::ARRAY
			emit_li(REG_A1, Variant::ARRAY);

			// a2 = method (0 for empty)
			emit_li(REG_A2, 0);

			// a3 = nullptr (0)
			emit_li(REG_A3, 0);

			// a7 = ECALL_VCREATE (517)
			emit_li(REG_A7, 517);
			emit_ecall();
		} else if (global.init_type == IRGlobalVar::InitType::EMPTY_DICT) {
			// Empty Dictionary initialization using VCREATE
			// a0 = pointer to destination Variant (T0 already points to it)
			emit_mv(REG_A0, REG_T0);

			// a1 = Variant::DICTIONARY
			emit_li(REG_A1, Variant::DICTIONARY);

			// a2 = method (0 for empty)
			emit_li(REG_A2, 0);

			// a3 = nullptr (0)
			emit_li(REG_A3, 0);

			// a7 = ECALL_VCREATE (517)
			emit_li(REG_A7, 517);
			emit_ecall();
		} else if (global.init_type == IRGlobalVar::InitType::NULL_VAL) {
			// Write type field: m_type = 0 (NIL)
			emit_li(REG_T1, Variant::NIL);
			emit_sw(REG_T1, REG_T0, 0);
		} else {
			// Unknown initialization type
			throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Global variable '" + global.name + "': Unknown initialization type.");
		}

	}

	// Evaluate the initializers that are not compile-time constants. This runs
	// before any property is registered, so an @export property is registered
	// with the value it was declared with.
	//
	// The init function follows the normal calling convention and writes a
	// return Variant through a0, so a0 has to point at real storage: the
	// scratch slot allocated past the end of the .globals array.
	if (program.has_global_init) {
		emit_address_of_global(REG_A0, m_global_count);
		mark_label_use(GLOBAL_INIT_LABEL, m_code.size());
		emit_jal(REG_RA, 0); // JAL ra, .init_globals
	}

	// Register @export properties, after every global holds its initial value.
	for (size_t i = 0; i < program.globals.size(); i++) {
		const auto& global = program.globals[i];
		if (!global.is_property) {
			continue;
		}

		// ECALL_SANDBOX_ADD (547)
		// A0 = 0 (add property)
		// A1 = name pointer
		// A2 = name length
		// A3 = Variant type
		// A4 = 0
		// A5 = 0
		// A6 = Variant pointer

		// Store the property name for later emission into the code section
		const std::string name_label = ".LPROP" + std::to_string(i);
		m_property_name_strings.push_back({global.name, name_label});

		// A0 = 0 (add property)
		emit_li(REG_A0, 0);

		// A1 = pointer to variable name string
		emit_la(REG_A1, name_label);

		// A2 = name length
		emit_li(REG_A2, static_cast<int64_t>(global.name.length()));

		// A3 = Variant type. The type is derived once, in the code generator,
		// where the initializer expression is still available; NIL means the
		// property holds an unconstrained Variant.
		const int32_t variant_type = global.value_type != IRInstruction::TypeHint_NONE
			? global.value_type
			: static_cast<int32_t>(Variant::NIL);
		emit_li(REG_A3, variant_type);
		emit_li(REG_A4, 0); // A4 = 0
		emit_li(REG_A5, 0); // A5 = 0

		// A6 = pointer to the global's Variant
		emit_address_of_global(REG_A6, i);

		// A7 = ECALL_SANDBOX_ADD (547)
		emit_li(REG_A7, 547);

		// Make the syscall
		emit_ecall();
	}

	// Emit STOP instruction after initialization
	// STOP is encoded as: SYSTEM instruction (I-type) with imm[11:0] = 0x7ff
	// SYSTEM opcode = 0x73, funct3 = 0, rs1 = 0, rd = 0, imm = 0x7ff
	emit_i_type(0x73, 0, 0, 0, 0x7ff);

	// Emit the global init function. It is deliberately not registered in
	// m_functions: it is internal and must not appear in the ELF's public
	// function table.
	if (program.has_global_init) {
		m_labels[GLOBAL_INIT_LABEL] = m_code.size();
		gen_function(program.global_init);
	}

	// Generate code for each function
	for (const auto& func : program.functions) {
		m_functions[func.name] = m_code.size();
		// Also register function name as a label for CALL instructions
		m_labels[func.name] = m_code.size();
		gen_function(func);
	}

	// Rewrite any conditional branch that cannot reach its target. This has to
	// happen while the buffer still holds nothing but instructions: it inserts
	// instructions, and everything appended below is placed relative to the
	// final code size.
	relax_branches();

	// Define constant pool labels at the end of code
	// Constants are appended after the code section
	size_t const_pool_base = m_code.size();
	for (size_t i = 0; i < m_constant_pool.size(); i++) {
		std::string label = ".LC" + std::to_string(i);
		m_labels[label] = const_pool_base + (i * 8);
	}

	// Append constant pool data to code
	for (int64_t constant : m_constant_pool) {
		for (int i = 0; i < 8; i++) {
			m_code.push_back(static_cast<uint8_t>((constant >> (i * 8)) & 0xFF));
		}
	}

	// Emit property name strings for @export globals
	for (const auto& [str, label] : m_property_name_strings) {
		// Align to 1-byte (strings are byte sequences)
		m_labels[label] = m_code.size();

		// Emit string bytes
		for (char c : str) {
			m_code.push_back(static_cast<uint8_t>(c));
		}

		// Null terminator
		m_code.push_back(0);
	}

	// Calculate global data size (m_global_count and m_globals already set earlier).
	// When there is a global init function, one extra Variant is allocated past
	// the end as its return slot: it follows the normal calling convention and
	// writes a return Variant through a0.
	const size_t global_slots = m_global_count + (program.has_global_init ? 1 : 0);
	m_global_data_size = global_slots * variant_size();

	// Define .globals label and allocate global data area
	// This will be placed in a separate R+W PT_LOAD segment by the ELF builder
	if (m_global_count > 0) {
		// Align to 8-byte boundary for proper Variant alignment
		while (m_code.size() % 8 != 0) {
			m_code.push_back(0);
		}

		// Calculate the virtual address for the .data segment
		// The .data segment will be loaded at: BASE_ADDR + text_size, aligned to 4KB
		size_t text_size = m_code.size();  // Current code size (before globals)
		size_t globals_vaddr = 0x10000 + text_size;
		globals_vaddr = (globals_vaddr + 0xFFF) & ~0xFFF;  // Align to 4KB page

		// For label resolution, we need to store this as if it were a code offset
		// Since BASE_ADDR (0x10000) is added during resolution, we store: vaddr - BASE_ADDR
		m_labels[GLOBALS_LABEL] = globals_vaddr - 0x10000;

		// Allocate global variables as empty Variants: type = NIL, payload =
		// INT32_MIN.
		//
		// INT32_MIN rather than 0 is what makes the payload "empty": for a
		// complex type the payload is a scoped-variant index, 0 is a perfectly
		// valid one, and VASSIGN only takes its "destination is empty, adopt the
		// source" path when the destination index is INT32_MIN. Leaving it 0
		// makes the first assignment into an uninitialized complex global assign
		// through whatever scoped variant happens to sit at index 0.
		for (size_t i = 0; i < global_slots; i++) {
			for (int j = 0; j < variant_size(); j++) {
				m_code.push_back(0); // type = NIL, and zero padding
			}
			// Overwrite the payload with a sign-extended INT32_MIN.
			const int64_t empty_index = static_cast<int64_t>(INT32_MIN);
			const size_t payload = m_code.size() - variant_size() + VARIANT_DATA_OFFSET;
			for (int j = 0; j < 8; j++) {
				m_code[payload + j] = static_cast<uint8_t>((empty_index >> (j * 8)) & 0xFF);
			}
		}
	}

	// Resolve all label references
	resolve_labels();

	return m_code;
}

void RISCVCodeGen::gen_function(const IRFunction& func) {
	// Godot Sandbox calling convention with Variants:
	// a0 = pointer to return Variant (pre-allocated by caller)
	// a1-a7 = pointers to argument Variants
	//
	// Everything this function keeps about itself lives in m_fn, which the
	// guard resets on the way in and clears on the way out.
	FunctionStateGuard function_state(*this);
	m_fn.num_params = func.parameters.size();

	// Initialize register allocator
	m_allocator.init(func);

	m_fn.in_function = true;

	// What has to be saved is a property of the instructions about to be
	// generated, so read it off them before the prologue is emitted. A leaf
	// function -- and every plain getter and setter is one -- keeps ra, and a
	// function that makes no system call keeps the caller's return-value
	// pointer in a0 for as long as it needs it.
	for (const auto& instr : func.instructions) {
		if (opcode_clobbers_abi_registers(instr.opcode)) {
			m_fn.spills_return_pointer = true;
		}
		if (instr.opcode == IROpcode::CALL) {
			m_fn.saves_return_address = true;
		}
	}
	m_fn.forward_to_return = find_return_forwarding(func);

	// A function that saves nothing, takes no parameters and writes every value
	// it produces through a0 never addresses anything off sp. Its frame is two
	// instructions that move the stack pointer down and back over memory
	// nothing touches, and every plain getter has exactly this shape.
	//
	// get_variant_stack_offset() and get_scratch_variant_offset() refuse to
	// answer once this is decided, so an expansion that turns out to want a
	// slot after all fails the compile rather than quietly addressing the
	// caller's frame through an sp that was never moved.
	m_fn.omits_frame = !m_fn.saves_return_address && !m_fn.spills_return_pointer &&
		m_fn.num_params == 0;
	for (size_t i = 0; m_fn.omits_frame && i < func.instructions.size(); i++) {
		switch (func.instructions[i].opcode) {
			case IROpcode::LABEL:
			case IROpcode::JUMP:
				break; // Control flow only; touches no memory.
			case IROpcode::RETURN:
				// RETURN copies the return register's slot into the caller's
				// Variant unless the instruction before it already wrote
				// through a0 -- or unless the function has no registers at all,
				// in which case there is no slot to copy.
				m_fn.omits_frame = func.max_registers == 0 ||
					(i > 0 && m_fn.forward_to_return[i - 1]);
				break;
			case IROpcode::LOAD_IMM:
			case IROpcode::LOAD_FLOAT_IMM:
			case IROpcode::LOAD_BOOL:
			case IROpcode::LOAD_GLOBAL:
				// These build their result through one base register, which is
				// a0 when the result is being returned and a slot otherwise.
				m_fn.omits_frame = m_fn.forward_to_return[i];
				break;
			default:
				m_fn.omits_frame = false;
				break;
		}
	}

	// Calculate stack frame size
	// Need space for: saved registers + space for Variants
	const int saved_reg_space = SAVED_REG_SPACE; // Save ra and a0 (return pointer)

	// Pre-allocate stack offsets for ALL virtual registers in order
	// Assign offsets deterministically based on virtual register number,
	// not based on the order instructions are visited.
	// Otherwise, optimizations that reorder or eliminate instructions will
	// cause different virtual registers to get different offsets, breaking the code.
	int max_variants = func.max_registers;
	// Some instructions have to materialize a Variant that no virtual register owns:
	// an immediate operand, or the result of a comparison the following branch reads.
	// Those live only for the length of one instruction's expansion, so a small shared
	// scratch area is enough -- but it has to be part of the frame. Allocating it after
	// the prologue was emitted would place it past the end of the frame, where the
	// stores land in the caller's frame instead.
	int variant_space = (max_variants + SCRATCH_VARIANT_SLOTS) * variant_size();

	// Pre-assign all virtual register offsets
	for (int vreg = 0; vreg < max_variants; vreg++) {
		int offset = saved_reg_space + (vreg * variant_size());
		m_fn.variant_offsets[vreg] = offset;
	}
	m_fn.scratch_slot_base = max_variants;
	m_fn.next_variant_slot = max_variants + SCRATCH_VARIANT_SLOTS;

	m_fn.stack_frame_size = m_fn.omits_frame ? 0 : saved_reg_space + variant_space;

	// Align to 16 bytes (RISC-V ABI requirement)
	m_fn.stack_frame_size = (m_fn.stack_frame_size + 15) & ~15;

	// Function prologue - allocate stack frame
	// addi sp, sp, -frame_size
	if (m_fn.stack_frame_size > 0) {
		emit_add_offset(REG_SP, REG_SP, -m_fn.stack_frame_size);
	}

	// Save return address: sd ra, 0(sp)
	if (m_fn.saves_return_address) {
		emit_sd(REG_RA, REG_SP, SAVED_RA_OFFSET);
	}

	// Save a0 (return Variant pointer): sd a0, 8(sp)
	if (m_fn.spills_return_pointer) {
		emit_sd(REG_A0, REG_SP, SAVED_A0_OFFSET);
	}

	// Copy parameter Variants from argument registers to stack
	// Parameters come in a1-a7 as POINTERS to Variants
	for (size_t i = 0; i < m_fn.num_params && i < 7; i++) {
		int param_vreg = static_cast<int>(i); // Parameters map to virtual registers 0-6
		int dst_offset = get_variant_stack_offset(param_vreg);
		uint8_t arg_reg = REG_A1 + static_cast<uint8_t>(i);

		// Copy the whole Variant from the pointer in arg_reg to the stack
		emit_variant_move(REG_SP, dst_offset, arg_reg, 0, REG_T0);
	}

	// Process each IR instruction
	for (size_t instr_idx = 0; instr_idx < func.instructions.size(); instr_idx++) {
		const auto& instr = func.instructions[instr_idx];
		const bool forward_return = m_fn.forward_to_return[instr_idx];
		m_fn.current_instr_idx++;

		// Where this instruction's result goes: normally the destination
		// register's slot in the frame, but for the last write before a RETURN,
		// straight through the caller's pointer in a0 -- which is where the
		// RETURN would have copied it from the slot anyway. Asking is what
		// commits to the forwarding, so only the expansion that is about to do
		// the writing may ask, and only once.
		auto value_destination = [&](int vreg) -> std::pair<uint8_t, int> {
			if (forward_return) {
				emit_load_return_pointer();
				m_fn.return_value_written = true;
				return { REG_A0, 0 };
			}
			return { REG_SP, get_variant_stack_offset(vreg) };
		};

		switch (instr.opcode) {
			case IROpcode::LABEL:
				define_label(std::get<std::string>(instr.operands[0].value));
				break;

			case IROpcode::LOAD_IMM: {
				int vreg = std::get<int>(instr.operands[0].value);
				int64_t value = std::get<int64_t>(instr.operands[1].value);
				auto [base, offset] = value_destination(vreg);
				emit_variant_create_int(offset, value, base);
				break;
			}

			case IROpcode::LOAD_FLOAT_IMM: {
				int vreg = std::get<int>(instr.operands[0].value);
				double value = std::get<double>(instr.operands[1].value);
				auto [base, offset] = value_destination(vreg);
				emit_variant_create_float(offset, value, base);
				break;
			}

			case IROpcode::LOAD_BOOL: {
				int vreg = std::get<int>(instr.operands[0].value);
				int64_t value = std::get<int64_t>(instr.operands[1].value);
				auto [base, offset] = value_destination(vreg);
				emit_variant_create_bool(offset, value != 0, base);
				break;
			}

			case IROpcode::LOAD_STRING: {
				int vreg = std::get<int>(instr.operands[0].value);
				int64_t string_idx = std::get<int64_t>(instr.operands[1].value);
				int stack_offset = get_variant_stack_offset(vreg);
				emit_variant_create_string(stack_offset, static_cast<int>(string_idx));
				break;
			}

			case IROpcode::MOVE: {
				int dst_vreg = std::get<int>(instr.operands[0].value);
				int src_vreg = std::get<int>(instr.operands[1].value);

				// Skip no-op moves
				if (dst_vreg == src_vreg) {
					break;
				}

				int dst_offset = get_variant_stack_offset(dst_vreg);
				int src_offset = get_variant_stack_offset(src_vreg);

				// Skip if source and destination are at same stack location
				if (dst_offset == src_offset) {
					break;
				}

				// Copy the whole Variant
				auto [base, offset] = value_destination(dst_vreg);
				emit_variant_move(base, offset, REG_SP, src_offset, REG_T0);
				break;
			}

			case IROpcode::CONVERT: {
				// CONVERT dst_reg, src_reg  with the target type in type_hint.
				int dst_vreg = std::get<int>(instr.operands[0].value);
				int src_vreg = std::get<int>(instr.operands[1].value);
				int dst_offset = get_variant_stack_offset(dst_vreg);
				int src_offset = get_variant_stack_offset(src_vreg);

				if (instr.type_hint != Variant::FLOAT) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR,
						std::string("CONVERT to ") + variant_type_name(instr.type_hint) +
						" is not implemented; only int -> float conversion is.");
				}

				// int -> float. Variant::FLOAT is always a 64-bit double, whatever
				// real_t is, so this is fcvt.d.l and a plain 64-bit store.
				emit_ld(REG_T0, REG_SP, src_offset + VARIANT_DATA_OFFSET);
				emit_fcvt_d_l(REG_FA0, REG_T0);
				emit_li(REG_T0, Variant::FLOAT);
				emit_sw(REG_T0, REG_SP, dst_offset);
				emit_fsd(REG_FA0, REG_SP, dst_offset + VARIANT_DATA_OFFSET);
				break;
			}

			case IROpcode::LOAD_GLOBAL: {
				// LOAD_GLOBAL dst_reg, global_index
				// Loads a global variable (Variant) from the global data area into a virtual register
				int dst_vreg = std::get<int>(instr.operands[0].value);
				int64_t global_idx = std::get<int64_t>(instr.operands[1].value);

				// Load global base address into t0
				// For now, we'll use a label to mark the global data section
				emit_address_of_global(REG_T0, static_cast<size_t>(global_idx));

				// Copy the whole Variant out of the global data area. The
				// destination is addressed off its base register directly
				// rather than through a second pointer register, which is one
				// instruction the stores were already able to do themselves.
				auto [base, offset] = value_destination(dst_vreg);
				emit_variant_move(base, offset, REG_T0, 0, REG_T1);
				break;
			}

			case IROpcode::STORE_GLOBAL: {
				// STORE_GLOBAL global_index, src_reg
				// Stores a virtual register (Variant) into a global variable
				int64_t global_idx = std::get<int64_t>(instr.operands[0].value);
				int src_vreg = std::get<int>(instr.operands[1].value);
				int src_offset = get_variant_stack_offset(src_vreg);

				// Get the global's type information
				const IRGlobalVar& global = m_globals[global_idx];

				// Determine if this is a complex type that needs VASSIGN
				// Complex types: STRING, STRING_NAME, NODE_PATH, RID, OBJECT, CALLABLE,
				//                SIGNAL, DICTIONARY, ARRAY, and all PACKED_*_ARRAY types
				bool needs_vassign = false;

				// The global's Variant type is derived once by the code
				// generator, from the type hint when there is one and from the
				// initializer otherwise, so this does not have to re-derive it
				// from init_type (which cannot describe a RUNTIME initializer).
				if (global.value_type != IRInstruction::TypeHint_NONE) {
					// is_complex_variant_type() is the single definition of which
					// Variant types are stored as a host-side index rather than
					// inline. The previous open-coded test here read `type == 3 ||
					// type >= 17`, which classified FLOAT as complex and Color,
					// Basis and Transform3D on the wrong sides of the line.
					needs_vassign = is_complex_variant_type(global.value_type);
				} else {
					// Type unknown at compile time. A raw copy of the Variant
					// would duplicate a scoped-variant index rather than assign
					// through it, so assign through VASSIGN, which is correct for
					// both trivial and reference-counted types.
					needs_vassign = true;
				}

				// Load address of global variable
				int global_offset = global_idx * variant_size();
				emit_address_of_global(REG_T0, static_cast<size_t>(global_idx));

				// Load source address
				emit_load_stack_offset(REG_T1, src_offset);

				if (needs_vassign) {
					// Complex type: Use VASSIGN to handle permanent indices
					// VASSIGN takes: a0 = dest_index, a1 = src_index
					// Returns: a0 = resulting index
					std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A7};
					auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_fn.current_instr_idx);

					for (const auto& move : moves) {
						emit_mv(move.second, move.first);
					}

					// Load indices (v.i at offset 8)
					emit_lw(REG_A0, REG_T0, 8); // dest index
					emit_lw(REG_A1, REG_T1, 8); // src index

					// Carry the source's Variant type across. Storing only the
					// index leaves a global that started out NIL claiming to be
					// NIL while holding a live index, and prevents an untyped
					// global from ever changing type - which GDScript allows.
					emit_lw(REG_T2, REG_T1, 0);
					emit_sw(REG_T2, REG_T0, 0);

					// Call VASSIGN (syscall 503)
					emit_li(REG_A7, 503);
					emit_ecall();

					// VASSIGN returns the new index in A0
					// Store it back to the global's v.i field (offset 8)
					emit_sd(REG_A0, REG_T0, 8);
				} else {
					// Simple type: copy the whole Variant
					emit_variant_move(REG_T0, 0, REG_T1, 0, REG_T2);
				}
				break;
			}


			case IROpcode::ADD:
			case IROpcode::SUB:
			case IROpcode::MUL:
			case IROpcode::DIV:
			case IROpcode::MOD:
			case IROpcode::BIT_AND:
			case IROpcode::BIT_OR:
			case IROpcode::BIT_XOR:
			case IROpcode::SHL:
			case IROpcode::SHR: {
				// Check that all operands are valid before processing
				if (instr.operands.size() < 3 ||
					instr.operands[0].type != IRValue::Type::REGISTER) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Arithmetic operations require at least 3 operands with first being REGISTER");
				}

				int dst_vreg = std::get<int>(instr.operands[0].value);
				int dst_offset = get_variant_stack_offset(dst_vreg);

				// Check if operands are registers
				bool lhs_is_reg = instr.operands[1].type == IRValue::Type::REGISTER;
				bool rhs_is_reg = instr.operands.size() > 2 && instr.operands[2].type == IRValue::Type::REGISTER;

				// Use optimized typed path if:
				// 1. Instruction has a type hint (INT, FLOAT, or vector type)
				// 2. Both operands are registers (not immediates)
				// This is the common case for type-hinted arithmetic
				//
				// IMPORTANT: We ONLY optimize Variants with type hints.
				// Untyped Variants fall back to VEVAL syscall, which also acts as
				// deoptimization to find bugs (unknown types are unpredictable).
				if (instr.type_hint != IRInstruction::TypeHint_NONE && lhs_is_reg && rhs_is_reg) {
					int lhs_vreg_local = std::get<int>(instr.operands[1].value);
					int rhs_vreg_local = std::get<int>(instr.operands[2].value);
					int lhs_offset = get_variant_stack_offset(lhs_vreg_local);
					int rhs_offset = get_variant_stack_offset(rhs_vreg_local);

					// Dispatch based on type hint
					if (instr.type_hint == Variant::INT) {
						// Scalar int: optimized 64-bit integer arithmetic
						emit_typed_int_binary_op(dst_offset, lhs_offset, rhs_offset, instr.opcode);
						break;
					} else if (instr.type_hint == Variant::FLOAT) {
						// Scalar float: optimized 64-bit double arithmetic
						emit_typed_float_binary_op(dst_offset, lhs_offset, rhs_offset, instr.opcode);
						break;
					} else if (TypeHintUtils::is_vector(instr.type_hint)) {
						// Vector types: element-wise arithmetic (2, 3, or 4 components)
						emit_typed_vector_binary_op(dst_offset, lhs_offset, rhs_offset, instr.opcode, instr.type_hint);
						break;
					}
					// Other type hints (if any) fall through to VEVAL
				}

				// Fall back to generic Variant evaluation for:
				// - Untyped operations (no type hint)
				// - Unsupported type hints
				// - Operations with immediate operands (handled via temporary Variants)

				// Map IR opcode to Variant::Operator
				int variant_op;
				switch (instr.opcode) {
					case IROpcode::ADD: variant_op = 6; break;  // OP_ADD
					case IROpcode::SUB: variant_op = 7; break;  // OP_SUBTRACT
					case IROpcode::MUL: variant_op = 8; break;  // OP_MULTIPLY
					case IROpcode::DIV: variant_op = 9; break;  // OP_DIVIDE
					case IROpcode::MOD: variant_op = 12; break; // OP_MODULE
					case IROpcode::SHL: variant_op = 14; break; // OP_SHIFT_LEFT
					case IROpcode::SHR: variant_op = 15; break; // OP_SHIFT_RIGHT
					case IROpcode::BIT_AND: variant_op = 16; break; // OP_BIT_AND
					case IROpcode::BIT_OR: variant_op = 17; break;  // OP_BIT_OR
					case IROpcode::BIT_XOR: variant_op = 18; break; // OP_BIT_XOR
					default: variant_op = 6; break;
				}

				if (lhs_is_reg && rhs_is_reg) {
					int lhs_vreg_local = std::get<int>(instr.operands[1].value);
					int rhs_vreg_local = std::get<int>(instr.operands[2].value);
					int lhs_offset = get_variant_stack_offset(lhs_vreg_local);
					int rhs_offset = get_variant_stack_offset(rhs_vreg_local);
					emit_variant_eval(dst_offset, lhs_offset, rhs_offset, variant_op);
				} else if (lhs_is_reg && !rhs_is_reg && instr.operands[2].type == IRValue::Type::IMMEDIATE) {
					// Left operand is register, right is immediate
					int lhs_vreg_local = std::get<int>(instr.operands[1].value);
					int64_t imm_val = std::get<int64_t>(instr.operands[2].value);
					int lhs_offset = get_variant_stack_offset(lhs_vreg_local);

					// Create a temporary Variant for the immediate value
					int imm_offset = get_scratch_variant_offset();
					emit_variant_create_int(imm_offset, static_cast<int>(imm_val));
					emit_variant_eval(dst_offset, lhs_offset, imm_offset, variant_op);
				} else if (!lhs_is_reg && rhs_is_reg && instr.operands[1].type == IRValue::Type::IMMEDIATE) {
					// Left operand is immediate, right is register
					int64_t imm_val = std::get<int64_t>(instr.operands[1].value);
					int rhs_vreg_local = std::get<int>(instr.operands[2].value);
					int rhs_offset = get_variant_stack_offset(rhs_vreg_local);

					// Create a temporary Variant for the immediate value
					int imm_offset = get_scratch_variant_offset();
					emit_variant_create_int(imm_offset, static_cast<int>(imm_val));
					emit_variant_eval(dst_offset, imm_offset, rhs_offset, variant_op);
				} else {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Unsupported operand types for arithmetic operation");
				}
				break;
			}

			case IROpcode::BIT_NOT: {
				// ~x is x XOR -1
				int dst_vreg = std::get<int>(instr.operands[0].value);
				int src_vreg = std::get<int>(instr.operands[1].value);

				int dst_offset = get_variant_stack_offset(dst_vreg);
				int src_offset = get_variant_stack_offset(src_vreg);

				if (instr.type_hint == Variant::INT) {
					// Native path: load the int64, invert it, store it back
					emit_load_variant_int(REG_T0, REG_SP, src_offset);
					emit_xori(REG_T1, REG_T0, -1);
					emit_li(REG_T0, Variant::INT);
					emit_store_variant_type(REG_T0, REG_SP, dst_offset);
					emit_store_variant_int(REG_T1, REG_SP, dst_offset);
					break;
				}

				// Untyped: let the host evaluate x ^ -1, which also produces the
				// correct type error for non-integer operands
				int minus_one_offset = get_scratch_variant_offset();
				emit_variant_create_int(minus_one_offset, -1);
				emit_variant_eval(dst_offset, src_offset, minus_one_offset, 18); // OP_BIT_XOR
				break;
			}

			case IROpcode::NEG: {
				int dst_vreg = std::get<int>(instr.operands[0].value);
				int src_vreg = std::get<int>(instr.operands[1].value);

				int dst_offset = get_variant_stack_offset(dst_vreg);
				int src_offset = get_variant_stack_offset(src_vreg);

				// Use OP_NEGATE (unary operation - use veval with same operand twice)
				// Actually for unary, we need a different approach - create a zero Variant
				// For now, use subtract: 0 - src
				// TODO: Add proper unary operation support
				int zero_offset = get_scratch_variant_offset();
				emit_variant_create_int(zero_offset, 0);
				emit_variant_eval(dst_offset, zero_offset, src_offset, 7); // OP_SUBTRACT
				break;
			}

			case IROpcode::CMP_EQ:
			case IROpcode::CMP_NEQ:
			case IROpcode::CMP_LT:
			case IROpcode::CMP_LTE:
			case IROpcode::CMP_GT:
			case IROpcode::CMP_GTE: {
				// Check that all operands are valid
				if (instr.operands.size() < 3 ||
					instr.operands[0].type != IRValue::Type::REGISTER) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Comparison operations require at least 3 operands with first being REGISTER");
				}

				int dst_vreg = std::get<int>(instr.operands[0].value);
				int dst_offset = get_variant_stack_offset(dst_vreg);

				// Check if operands are registers
				bool lhs_is_reg = instr.operands[1].type == IRValue::Type::REGISTER;
				bool rhs_is_reg = instr.operands.size() > 2 && instr.operands[2].type == IRValue::Type::REGISTER;

				// Use optimized typed path for INT comparisons with register operands
				// This is very common in loops: for i: int in range(N)
				if (instr.type_hint == Variant::INT && lhs_is_reg && rhs_is_reg) {
					int lhs_vreg = std::get<int>(instr.operands[1].value);
					int rhs_vreg = std::get<int>(instr.operands[2].value);
					int lhs_offset = get_variant_stack_offset(lhs_vreg);
					int rhs_offset = get_variant_stack_offset(rhs_vreg);

					// Use native RISC-V comparison instead of syscall
					emit_typed_int_comparison(dst_offset, lhs_offset, rhs_offset, instr.opcode);
					break;
				}

				// Fall back to generic Variant evaluation for untyped or non-INT comparisons

				// Map IR opcode to Variant::Operator
				int variant_op;
				switch (instr.opcode) {
					case IROpcode::CMP_EQ:  variant_op = 0; break; // OP_EQUAL
					case IROpcode::CMP_NEQ: variant_op = 1; break; // OP_NOT_EQUAL
					case IROpcode::CMP_LT:  variant_op = 2; break; // OP_LESS
					case IROpcode::CMP_LTE: variant_op = 3; break; // OP_LESS_EQUAL
					case IROpcode::CMP_GT:  variant_op = 4; break; // OP_GREATER
					case IROpcode::CMP_GTE: variant_op = 5; break; // OP_GREATER_EQUAL
					default: variant_op = 0; break;
				}

				if (lhs_is_reg && rhs_is_reg) {
					int lhs_vreg = std::get<int>(instr.operands[1].value);
					int rhs_vreg = std::get<int>(instr.operands[2].value);
					int lhs_offset = get_variant_stack_offset(lhs_vreg);
					int rhs_offset = get_variant_stack_offset(rhs_vreg);
					emit_variant_eval(dst_offset, lhs_offset, rhs_offset, variant_op);
				} else if (lhs_is_reg && !rhs_is_reg && instr.operands[2].type == IRValue::Type::IMMEDIATE) {
					// Left is register, right is immediate integer
					int lhs_vreg = std::get<int>(instr.operands[1].value);
					int lhs_offset = get_variant_stack_offset(lhs_vreg);
					int64_t imm_val = std::get<int64_t>(instr.operands[2].value);

					int imm_offset = get_scratch_variant_offset();
					emit_variant_create_int(imm_offset, static_cast<int>(imm_val));
					emit_variant_eval(dst_offset, lhs_offset, imm_offset, variant_op);
				} else if (!lhs_is_reg && rhs_is_reg && instr.operands[1].type == IRValue::Type::IMMEDIATE) {
					// Left is immediate integer, right is register
					int rhs_vreg = std::get<int>(instr.operands[2].value);
					int rhs_offset = get_variant_stack_offset(rhs_vreg);
					int64_t imm_val = std::get<int64_t>(instr.operands[1].value);

					int imm_offset = get_scratch_variant_offset();
					emit_variant_create_int(imm_offset, static_cast<int>(imm_val));
					emit_variant_eval(dst_offset, imm_offset, rhs_offset, variant_op);
				} else {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Unsupported operand types for comparison");
				}
				break;
			}

			case IROpcode::AND: {
				int dst_vreg = std::get<int>(instr.operands[0].value);
				int lhs_vreg = std::get<int>(instr.operands[1].value);
				int rhs_vreg = std::get<int>(instr.operands[2].value);

				int dst_offset = get_variant_stack_offset(dst_vreg);
				int lhs_offset = get_variant_stack_offset(lhs_vreg);
				int rhs_offset = get_variant_stack_offset(rhs_vreg);

				emit_variant_eval(dst_offset, lhs_offset, rhs_offset, 20); // OP_AND
				break;
			}

			case IROpcode::OR: {
				int dst_vreg = std::get<int>(instr.operands[0].value);
				int lhs_vreg = std::get<int>(instr.operands[1].value);
				int rhs_vreg = std::get<int>(instr.operands[2].value);

				int dst_offset = get_variant_stack_offset(dst_vreg);
				int lhs_offset = get_variant_stack_offset(lhs_vreg);
				int rhs_offset = get_variant_stack_offset(rhs_vreg);

				emit_variant_eval(dst_offset, lhs_offset, rhs_offset, 21); // OP_OR
				break;
			}

			case IROpcode::NOT: {
				int dst_vreg = std::get<int>(instr.operands[0].value);
				int src_vreg = std::get<int>(instr.operands[1].value);

				int dst_offset = get_variant_stack_offset(dst_vreg);
				int src_offset = get_variant_stack_offset(src_vreg);

				// OP_NOT - use veval (unary operations need special handling)
				// For now, use the same operand for both sides
				emit_variant_eval_unary(dst_offset, src_offset, 23); // OP_NOT
				break;
			}

			case IROpcode::BRANCH_ZERO: {
				int vreg = std::get<int>(instr.operands[0].value);
				int offset = get_variant_stack_offset(vreg);
				emit_variant_truthy(REG_T2, offset, instr.type_hint);
				mark_label_use(std::get<std::string>(instr.operands[1].value), m_code.size());
				emit_beq(REG_T2, REG_ZERO, 0);
				break;
			}

			case IROpcode::BRANCH_NOT_ZERO: {
				int vreg = std::get<int>(instr.operands[0].value);
				int offset = get_variant_stack_offset(vreg);
				emit_variant_truthy(REG_T2, offset, instr.type_hint);
				mark_label_use(std::get<std::string>(instr.operands[1].value), m_code.size());
				emit_bne(REG_T2, REG_ZERO, 0);
				break;
			}

			case IROpcode::BRANCH_EQ:
			case IROpcode::BRANCH_NEQ:
			case IROpcode::BRANCH_LT:
			case IROpcode::BRANCH_LTE:
			case IROpcode::BRANCH_GT:
			case IROpcode::BRANCH_GTE: {
				// Fused comparison + branch instructions
				// Format: BRANCH_* lhs, rhs, label
				// These directly emit BEQ/BNE/BLT/BGE without storing result
				if (instr.operands.size() < 3) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Fused branch requires 3 operands: lhs, rhs, label");
				}

				// Check if operands are registers
				bool lhs_is_reg = instr.operands[0].type == IRValue::Type::REGISTER;
				bool rhs_is_reg = instr.operands[1].type == IRValue::Type::REGISTER;

				// For now, only support register operands (can be extended later for immediates)
				if (!lhs_is_reg || !rhs_is_reg) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Fused branch requires register operands");
				}

				int lhs_vreg = std::get<int>(instr.operands[0].value);
				int rhs_vreg = std::get<int>(instr.operands[1].value);
				std::string label = std::get<std::string>(instr.operands[2].value);

				// Use optimized path for type-hinted INT comparisons
				if (instr.type_hint == Variant::INT) {
					int lhs_offset = get_variant_stack_offset(lhs_vreg);
					int rhs_offset = get_variant_stack_offset(rhs_vreg);

					// Load both int64 values
					emit_load_variant_int(REG_T0, REG_SP, lhs_offset);
					emit_load_variant_int(REG_T1, REG_SP, rhs_offset);

					// Emit direct branch based on comparison type
					mark_label_use(label, m_code.size());
					switch (instr.opcode) {
						case IROpcode::BRANCH_EQ:
							emit_beq(REG_T0, REG_T1, 0);
							break;
						case IROpcode::BRANCH_NEQ:
							emit_bne(REG_T0, REG_T1, 0);
							break;
						case IROpcode::BRANCH_LT:
							emit_blt(REG_T0, REG_T1, 0);
							break;
						case IROpcode::BRANCH_LTE:
							// t0 <= t1 is !(t0 > t1), which is !(t1 < t0)
							// So we branch if NOT (t1 < t0), i.e., t1 >= t0
							emit_bge(REG_T1, REG_T0, 0);
							break;
						case IROpcode::BRANCH_GT:
							// t0 > t1 is t1 < t0
							emit_blt(REG_T1, REG_T0, 0);
							break;
						case IROpcode::BRANCH_GTE:
							emit_bge(REG_T0, REG_T1, 0);
							break;
						default:
							throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Unknown fused branch opcode");
					}
				} else {
					// Fall back to comparison + branch for non-INT types
					// This is less optimal but maintains correctness
					int lhs_offset = get_variant_stack_offset(lhs_vreg);
					int rhs_offset = get_variant_stack_offset(rhs_vreg);
					int tmp_offset = get_scratch_variant_offset();

					// Map IR opcode to Variant::Operator
					int variant_op;
					switch (instr.opcode) {
						case IROpcode::BRANCH_EQ:  variant_op = 0; break; // OP_EQUAL
						case IROpcode::BRANCH_NEQ: variant_op = 1; break; // OP_NOT_EQUAL
						case IROpcode::BRANCH_LT:  variant_op = 2; break; // OP_LESS
						case IROpcode::BRANCH_LTE: variant_op = 3; break; // OP_LESS_EQUAL
						case IROpcode::BRANCH_GT:  variant_op = 4; break; // OP_GREATER
						case IROpcode::BRANCH_GTE: variant_op = 5; break; // OP_GREATER_EQUAL
						default: variant_op = 0; break;
					}

					// Perform comparison via syscall
					emit_variant_eval(tmp_offset, lhs_offset, rhs_offset, variant_op);

					// Load result and branch
					emit_load_variant_bool(REG_T0, REG_SP, tmp_offset);
					mark_label_use(label, m_code.size());
					emit_bne(REG_T0, REG_ZERO, 0);
				}
				break;
			}

			case IROpcode::JUMP:
				mark_label_use(std::get<std::string>(instr.operands[0].value), m_code.size());
				emit_jal(REG_ZERO, 0);
				break;

			case IROpcode::RETURN: {
				// Godot Sandbox calling convention with Variants:
				// a0 points to pre-allocated Variant for return value
				// Copy the return Variant (virtual register 0) to *a0

				// The instruction before this one may already have written the
				// value through a0, in which case there is nothing left to copy.
				if (m_fn.return_value_written) {
					m_fn.return_value_written = false;
				} else if (m_fn.variant_offsets.find(IRFunction::RETURN_REGISTER) != m_fn.variant_offsets.end()) {
					int src_offset = get_variant_stack_offset(IRFunction::RETURN_REGISTER);

					// Copy the return Variant from its slot to *a0, addressing
					// the source off sp rather than through a pointer register.
					emit_load_return_pointer();
					emit_variant_move(REG_A0, 0, REG_SP, src_offset, REG_T0);
				}

				// Function epilogue - restore registers and deallocate stack
				// Restore return address: ld ra, 0(sp)
				if (m_fn.saves_return_address) {
					emit_ld(REG_RA, REG_SP, SAVED_RA_OFFSET);
				}

				// Deallocate stack: addi sp, sp, frame_size
				if (m_fn.stack_frame_size > 0) {
					emit_add_offset(REG_SP, REG_SP, m_fn.stack_frame_size);
				}

				emit_ret();
				break;
			}

			case IROpcode::VCALL: {
				// VCALL format: result_reg, obj_reg, method_name, arg_count, arg1_reg, arg2_reg, ...
				if (instr.operands.size() < 4) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "VCALL requires at least 4 operands");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int obj_vreg = std::get<int>(instr.operands[1].value);
				std::string method_name = std::get<std::string>(instr.operands[2].value);
				int arg_count = static_cast<int>(std::get<int64_t>(instr.operands[3].value));

				if (instr.operands.size() != static_cast<size_t>(4 + arg_count)) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "VCALL argument count mismatch");
				}

				int result_offset = get_variant_stack_offset(result_vreg);
				int obj_offset = get_variant_stack_offset(obj_vreg);

				// VCALL clobbers a0-a7, handle register clobbering
				std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2, REG_A3, REG_A4, REG_A5, REG_A6, REG_A7};
				auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_fn.current_instr_idx);

				for (const auto& move : moves) {
					emit_mv(move.second, move.first);
				}

				// If we have arguments, allocate stack space for argument array
				int args_stack_offset = 0;
				if (arg_count > 0) {
					args_stack_offset = m_fn.stack_frame_size - (arg_count * variant_size());
					// Expand stack if needed
					int additional_space = arg_count * variant_size();
					additional_space = (additional_space + 15) & ~15; // Align to 16 bytes

					// Adjust stack pointer
					emit_stack_adjust(-additional_space);

					// Copy argument Variants to the new stack space
					for (int i = 0; i < arg_count; i++) {
						int arg_vreg = std::get<int>(instr.operands[4 + i].value);
						int arg_src_offset = get_variant_stack_offset(arg_vreg) + additional_space; // Adjust for moved stack
						int arg_dst_offset = i * variant_size();

						// Copy the whole Variant from src to dst
						emit_variant_move(REG_SP, arg_dst_offset, REG_SP, arg_src_offset, REG_T0);
					}

					// a3 = pointer to arguments array (sp + 0)
					emit_mv(REG_A3, REG_SP);
				} else {
					// No arguments, a3 = 0
					emit_mv(REG_A3, REG_ZERO);
				}

				// a0 = pointer to object Variant (sp + obj_offset + additional_space if we allocated stack)
				int adjusted_obj_offset = obj_offset;
				if (arg_count > 0) {
					int additional_space = arg_count * variant_size();
					additional_space = (additional_space + 15) & ~15;
					adjusted_obj_offset += additional_space;
				}

				emit_load_stack_offset(REG_A0, adjusted_obj_offset);

				// a1 = pointer to method name string (need to store in .rodata section)
				// For now, we'll use a temporary approach: store the string on stack
				// TODO: Better approach would be to use .rodata section
				int method_len = method_name.length();
				int str_space = ((method_len + 1) + 7) & ~7; // Align to 8 bytes, +1 for null terminator

				// Allocate more stack space for the string
				emit_stack_adjust(-str_space);

				// Store method name on stack
				for (size_t i = 0; i < method_name.length(); i++) {
					emit_li(REG_T0, static_cast<unsigned char>(method_name[i]));
					emit_sb(REG_T0, REG_SP, i); // SB (store byte)
				}
				// Store null terminator
				emit_sb(REG_ZERO, REG_SP, method_len); // SB

				// a1 = pointer to method name (sp)
				emit_mv(REG_A1, REG_SP);

				// a2 = method length
				emit_li(REG_A2, method_len);

				// a4 = argument count
				emit_li(REG_A4, arg_count);

				// a5 = pointer to result Variant
				int adjusted_result_offset = result_offset;
				if (arg_count > 0) {
					int additional_space = arg_count * variant_size();
					additional_space = (additional_space + 15) & ~15;
					adjusted_result_offset += additional_space;
				}
				adjusted_result_offset += str_space;

				emit_load_stack_offset(REG_A5, adjusted_result_offset);

				// a7 = ECALL_VCALL (501)
				emit_li(REG_A7, 501);
				emit_ecall();

				// Restore stack pointer
				int total_stack_adjust = str_space;
				if (arg_count > 0) {
					int additional_space = arg_count * variant_size();
					additional_space = (additional_space + 15) & ~15;
					total_stack_adjust += additional_space;
				}

				emit_stack_adjust(total_stack_adjust);

				// VCALL always writes the result as a full Variant on stack (via a5 pointer)
				break;
			}

			case IROpcode::CALL: {
				// CALL format: function_name, result_reg, arg_count, arg1_reg, arg2_reg, ...
				if (instr.operands.size() < 3) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "CALL requires at least 3 operands");
				}

				std::string func_name = std::get<std::string>(instr.operands[0].value);
				int result_vreg = std::get<int>(instr.operands[1].value);
				int arg_count = static_cast<int>(std::get<int64_t>(instr.operands[2].value));

				if (instr.operands.size() != static_cast<size_t>(3 + arg_count)) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "CALL argument count mismatch");
				}

				// Handle register clobbering (calls clobber a0-a7, ra, and temporaries)
				std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2, REG_A3, REG_A4, REG_A5, REG_A6, REG_A7, REG_RA};
				auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_fn.current_instr_idx);

				for (const auto& move : moves) {
					emit_mv(move.second, move.first);
				}

				// Allocate stack space for return value Variant
				int return_var_offset = get_variant_stack_offset(result_vreg);

				// Set up arguments: a1-a7 point to Variant arguments on stack
				// a0 points to return Variant
				for (int i = 0; i < arg_count && i < 7; i++) {
					int arg_vreg = std::get<int>(instr.operands[3 + i].value);
					int arg_offset = get_variant_stack_offset(arg_vreg);
					uint8_t arg_reg = REG_A1 + static_cast<uint8_t>(i);

					// Load address of argument Variant
					emit_add_offset(arg_reg, REG_SP, arg_offset);
				}

				// a0 = pointer to return Variant
				emit_add_offset(REG_A0, REG_SP, return_var_offset);

				// Call the function using JAL with label
				// We'll use the function name as a label that will be resolved later
				if (!m_fn.saves_return_address) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR,
						"Call emitted in a function whose prologue did not save the return address");
				}
				mark_label_use(func_name, m_code.size());
				emit_jal(REG_RA, 0); // JAL ra, func_name

				// Return value is already in the Variant at result_vreg
				break;
			}

			// Inline primitive construction (no syscalls!)
			case IROpcode::MAKE_VECTOR2:
			case IROpcode::MAKE_VECTOR3:
			case IROpcode::MAKE_VECTOR4: {
				// Format: MAKE_VECTORn result_reg, x_reg, y_reg, [z_reg], [w_reg]
				int num_components = (instr.opcode == IROpcode::MAKE_VECTOR2) ? 2 :
									 (instr.opcode == IROpcode::MAKE_VECTOR3) ? 3 : 4;

				if (instr.operands.size() != static_cast<size_t>(1 + num_components)) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "MAKE_VECTOR requires correct number of operands");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int result_offset = get_variant_stack_offset(result_vreg);

				// Set m_type field (offset 0)
				int variant_type = (instr.opcode == IROpcode::MAKE_VECTOR2) ? Variant::VECTOR2 :
								   (instr.opcode == IROpcode::MAKE_VECTOR3) ? Variant::VECTOR3 : Variant::VECTOR4;
				emit_li(REG_T0, variant_type);
				emit_sw(REG_T0, REG_SP, result_offset); // Store type at offset 0

				// Store each component as a real_t in v.v4[]
				for (int i = 0; i < num_components; i++) {
					int comp_vreg = std::get<int>(instr.operands[1 + i].value);
					int comp_offset = get_variant_stack_offset(comp_vreg);
					emit_variant_component_to_real(comp_offset, result_offset, real_offset(i));
				}

				break;
			}

			case IROpcode::MAKE_VECTOR2I:
			case IROpcode::MAKE_VECTOR3I:
			case IROpcode::MAKE_VECTOR4I: {
				// Format: MAKE_VECTORnI result_reg, x_reg, y_reg, [z_reg], [w_reg]
				int num_components = (instr.opcode == IROpcode::MAKE_VECTOR2I) ? 2 :
									 (instr.opcode == IROpcode::MAKE_VECTOR3I) ? 3 : 4;

				if (instr.operands.size() != static_cast<size_t>(1 + num_components)) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "MAKE_VECTORnI requires correct number of operands");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int result_offset = get_variant_stack_offset(result_vreg);

				// Set m_type field (offset 0)
				int variant_type = (instr.opcode == IROpcode::MAKE_VECTOR2I) ? Variant::VECTOR2I :
								   (instr.opcode == IROpcode::MAKE_VECTOR3I) ? Variant::VECTOR3I : Variant::VECTOR4I;
				emit_li(REG_T0, variant_type);
				emit_sw(REG_T0, REG_SP, result_offset); // Store type at offset 0

				// Store each component in v.v4i[] (int32_t, same width in both builds)
				for (int i = 0; i < num_components; i++) {
					int comp_vreg = std::get<int>(instr.operands[1 + i].value);
					int comp_offset = get_variant_stack_offset(comp_vreg);

					// Load from stack Variant
					emit_lw(REG_T0, REG_SP, comp_offset + VARIANT_DATA_OFFSET);
					emit_sw(REG_T0, REG_SP, result_offset + int_offset(i));
				}

				break;
			}

			case IROpcode::MAKE_COLOR: {
				// Format: MAKE_COLOR result_reg, r_reg, g_reg, b_reg, a_reg
				if (instr.operands.size() != 5) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "MAKE_COLOR requires 5 operands");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int result_offset = get_variant_stack_offset(result_vreg);

				// Set m_type field to COLOR
				emit_li(REG_T0, Variant::COLOR);
				emit_sw(REG_T0, REG_SP, result_offset);

				// Store each component (r, g, b, a) as real_t: a Color inside a Variant
				// shares the v.v4[] union with the vectors, so it follows real_t too.
				// Color components need to be normalized (0-255 integers -> 0.0-1.0 floats)
				for (int i = 0; i < 4; i++) {
					int comp_vreg = std::get<int>(instr.operands[1 + i].value);
					int comp_offset = get_variant_stack_offset(comp_vreg);
					emit_variant_component_to_real(comp_offset, result_offset, real_offset(i), true); // normalize_by_255 = true
				}

				break;
			}

			case IROpcode::PRINT: {
				// Format: PRINT result_reg, arg_count, [arg_reg1, arg_reg2, ...]
				// sys_print(const Variant *array, size_t len): the host walks a
				// contiguous array, so the arguments are copied out of their
				// stack slots into one run below sp.
				if (instr.operands.size() < 2) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "PRINT requires at least 2 operands");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int arg_count = static_cast<int>(std::get<int64_t>(instr.operands[1].value));

				if (instr.operands.size() != static_cast<size_t>(2 + arg_count)) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "PRINT argument count mismatch");
				}

				int result_offset = get_variant_stack_offset(result_vreg);

				// ECALL_PRINT reads a0-a1 and a7; the register allocator has to
				// evacuate anything live in them first.
				std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1};
				auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_fn.current_instr_idx);
				for (const auto& move : moves) {
					emit_mv(move.second, move.first);
				}

				if (arg_count == 0) {
					// print() with no arguments still prints a blank line, and
					// the host reads no memory when len is 0.
					emit_mv(REG_A0, REG_ZERO);
					emit_li(REG_A1, 0);
					emit_li(REG_A7, 500); // ECALL_PRINT
					emit_ecall();
				} else {
					int args_space = arg_count * variant_size();
					args_space = (args_space + 15) & ~15; // Align to 16 bytes

					emit_add_offset(REG_SP, REG_SP, -args_space);

					// Each argument lives at its own offset in the frame that sp
					// just moved away from, so read it back at +args_space.
					for (int i = 0; i < arg_count; i++) {
						int arg_vreg = std::get<int>(instr.operands[2 + i].value);
						int arg_src_offset = get_variant_stack_offset(arg_vreg) + args_space;
						emit_variant_move(REG_SP, i * variant_size(), REG_SP, arg_src_offset, REG_T0);
					}

					// a0 = the argument array, a1 = its length
					emit_mv(REG_A0, REG_SP);
					emit_li(REG_A1, arg_count);
					emit_li(REG_A7, 500); // ECALL_PRINT
					emit_ecall();

					emit_add_offset(REG_SP, REG_SP, args_space);
				}

				// print() evaluates to null. The host writes nothing back, so
				// the destination is set here.
				emit_li(REG_T0, Variant::NIL);
				emit_store_variant_type(REG_T0, REG_SP, result_offset);
				break;
			}

			case IROpcode::GLOBAL_CALL:
				// Every global except print(). See riscv_globals.cpp.
				emit_global_call(instr);
				break;

			case IROpcode::MAKE_ARRAY: {
				// Format: MAKE_ARRAY result_reg, element_count, [element_reg1, element_reg2, ...]
				// For empty arrays: element_count = 0, no element regs
				if (instr.operands.size() < 2) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "MAKE_ARRAY requires at least 2 operands");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int element_count = static_cast<int>(std::get<int64_t>(instr.operands[1].value));
				int result_offset = get_variant_stack_offset(result_vreg);

				// Handle register clobbering (VCREATE uses a0-a3)
				std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2, REG_A3};
				auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_fn.current_instr_idx);
				for (const auto& move : moves) {
					emit_mv(move.second, move.first);
				}

				if (element_count == 0) {
					// Empty Array: use the abstraction
					emit_variant_create_empty_array(result_offset);
				} else {
					// Array with elements: sys_vcreate(&v, ARRAY, size, data_pointer)
					// We need to copy the full Variant structures to stack
					int args_space = element_count * variant_size();
					args_space = (args_space + 15) & ~15; // Align to 16 bytes

					// Adjust stack pointer
					emit_add_offset(REG_SP, REG_SP, -args_space);

					// Copy full GuestVariant structures to stack
					for (int i = 0; i < element_count; i++) {
						int elem_vreg = std::get<int>(instr.operands[2 + i].value);
						int elem_offset = get_variant_stack_offset(elem_vreg);

						// Destination address for this element
						int dst_offset = i * variant_size();

						// The element Variant is at elem_offset from the ORIGINAL stack frame
						// After SP -= args_space, it's now at elem_offset + args_space from NEW SP
						// So we load from (elem_offset + args_space) and store to dst_offset
						emit_variant_move(REG_SP, dst_offset, REG_SP, elem_offset + args_space, REG_T0);
					}

					// a0 = pointer to destination Variant
					// The result Variant is at result_offset from the ORIGINAL stack frame
					// After SP -= args_space, it's now at result_offset + args_space from NEW SP
					int adjusted_dst_offset = result_offset + args_space;
					emit_add_offset(REG_A0, REG_SP, adjusted_dst_offset);

					// a1 = Variant::ARRAY
					emit_li(REG_A1, Variant::ARRAY);

					// a2 = size (element_count)
					emit_li(REG_A2, element_count);

					// a3 = pointer to element array (sp + 0)
					emit_mv(REG_A3, REG_SP);

					// a7 = ECALL_VCREATE (517)
					emit_li(REG_A7, 517);
					emit_ecall();

					// Restore stack pointer
					emit_add_offset(REG_SP, REG_SP, args_space);
				}

				break;
			}

			case IROpcode::MAKE_DICTIONARY: {
				// Format: MAKE_DICTIONARY result_reg, pair_count, [key1_reg, val1_reg, key2_reg, val2_reg, ...]
				// For empty dictionaries via Dictionary() constructor: MAKE_DICTIONARY result_reg
				// For empty dictionaries via literal {}: MAKE_DICTIONARY result_reg, 0
				// For dictionaries with elements: pair_count > 0 with key/value regs
				if (instr.operands.size() < 1) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "MAKE_DICTIONARY requires at least 1 operand");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int result_offset = get_variant_stack_offset(result_vreg);

				// Check if this is the old Dictionary() constructor format (only 1 operand)
				// or the new dictionary literal format (2+ operands)
				bool is_constructor_format = (instr.operands.size() == 1);
				int pair_count = is_constructor_format ? 0 : static_cast<int>(std::get<int64_t>(instr.operands[1].value));

				// Handle register clobbering (VCREATE uses a0-a3)
				std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2, REG_A3};
				auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_fn.current_instr_idx);
				for (const auto& move : moves) {
					emit_mv(move.second, move.first);
				}

				if (pair_count == 0) {
					// Empty Dictionary: use the abstraction
					emit_variant_create_empty_dictionary(result_offset);
				} else {
					// Dictionary with elements: sys_vcreate(&v, DICTIONARY, size, data_pointer)
					// We need to copy the full Variant structures to stack
					int total_variants = pair_count * 2; // Each pair has 2 variants (key and value)
					int args_space = total_variants * variant_size();
					args_space = (args_space + 15) & ~15; // Align to 16 bytes

					// Adjust stack pointer
					emit_add_offset(REG_SP, REG_SP, -args_space);

					// Copy full GuestVariant structures to stack
					// Operands are interleaved: key1, val1, key2, val2, ...
					for (int i = 0; i < total_variants; i++) {
						int variant_vreg = std::get<int>(instr.operands[2 + i].value);
						int variant_offset = get_variant_stack_offset(variant_vreg);

						// Destination address for this variant
						int dst_offset = i * variant_size();

						// The variant Variant is at variant_offset from the ORIGINAL stack frame
						// After SP -= args_space, it's now at variant_offset + args_space from NEW SP
						// So we load from (variant_offset + args_space) and store to dst_offset
						emit_variant_move(REG_SP, dst_offset, REG_SP, variant_offset + args_space, REG_T0);
					}

					// a0 = pointer to destination Variant
					// The result Variant is at result_offset from the ORIGINAL stack frame
					// After SP -= args_space, it's now at result_offset + args_space from NEW SP
					int adjusted_dst_offset = result_offset + args_space;
					emit_add_offset(REG_A0, REG_SP, adjusted_dst_offset);

					// a1 = Variant::DICTIONARY
					emit_li(REG_A1, Variant::DICTIONARY);

					// a2 = size (pair_count * 2 = total number of variants)
					emit_li(REG_A2, total_variants);

					// a3 = pointer to variant array (sp + 0)
					emit_mv(REG_A3, REG_SP);

					// a7 = ECALL_VCREATE (517)
					emit_li(REG_A7, 517);
					emit_ecall();

					// Restore stack pointer
					emit_add_offset(REG_SP, REG_SP, args_space);
				}

				break;
			}

			case IROpcode::MAKE_PACKED_BYTE_ARRAY:
			case IROpcode::MAKE_PACKED_INT32_ARRAY:
			case IROpcode::MAKE_PACKED_INT64_ARRAY:
			case IROpcode::MAKE_PACKED_FLOAT32_ARRAY:
			case IROpcode::MAKE_PACKED_FLOAT64_ARRAY:
			case IROpcode::MAKE_PACKED_STRING_ARRAY:
			case IROpcode::MAKE_PACKED_VECTOR2_ARRAY:
			case IROpcode::MAKE_PACKED_VECTOR3_ARRAY:
			case IROpcode::MAKE_PACKED_COLOR_ARRAY:
			case IROpcode::MAKE_PACKED_VECTOR4_ARRAY: {
				// Format: MAKE_PACKED_*_ARRAY result_reg, element_count, [element_reg1, element_reg2, ...]
				// Works identically to MAKE_ARRAY but with different Variant type
				if (instr.operands.size() < 2) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Packed array constructor requires at least 2 operands");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int element_count = static_cast<int>(std::get<int64_t>(instr.operands[1].value));
				int result_offset = get_variant_stack_offset(result_vreg);

				// Determine the Variant type based on opcode
				int variant_type;
				switch (instr.opcode) {
					case IROpcode::MAKE_PACKED_BYTE_ARRAY:
						variant_type = Variant::PACKED_BYTE_ARRAY;
						break;
					case IROpcode::MAKE_PACKED_INT32_ARRAY:
						variant_type = Variant::PACKED_INT32_ARRAY;
						break;
					case IROpcode::MAKE_PACKED_INT64_ARRAY:
						variant_type = Variant::PACKED_INT64_ARRAY;
						break;
					case IROpcode::MAKE_PACKED_FLOAT32_ARRAY:
						variant_type = Variant::PACKED_FLOAT32_ARRAY;
						break;
					case IROpcode::MAKE_PACKED_FLOAT64_ARRAY:
						variant_type = Variant::PACKED_FLOAT64_ARRAY;
						break;
					case IROpcode::MAKE_PACKED_STRING_ARRAY:
						variant_type = Variant::PACKED_STRING_ARRAY;
						break;
					case IROpcode::MAKE_PACKED_VECTOR2_ARRAY:
						variant_type = Variant::PACKED_VECTOR2_ARRAY;
						break;
					case IROpcode::MAKE_PACKED_VECTOR3_ARRAY:
						variant_type = Variant::PACKED_VECTOR3_ARRAY;
						break;
					case IROpcode::MAKE_PACKED_COLOR_ARRAY:
						variant_type = Variant::PACKED_COLOR_ARRAY;
						break;
					case IROpcode::MAKE_PACKED_VECTOR4_ARRAY:
						variant_type = Variant::PACKED_VECTOR4_ARRAY;
						break;
					default:
						variant_type = Variant::ARRAY; // Should not happen
						break;
				}

				if (element_count == 0) {
					// Empty packed array: sys_vcreate(&v, TYPE, 0, nullptr)
					// a0 = pointer to destination Variant
					emit_add_offset(REG_A0, REG_SP, result_offset);

					// a1 = Variant type
					emit_li(REG_A1, variant_type);

					// a2 = method (0 for empty)
					emit_li(REG_A2, 0);

					// a3 = nullptr (0)
					emit_li(REG_A3, 0);

					// a7 = ECALL_VCREATE (517)
					emit_li(REG_A7, 517);
					emit_ecall();
				} else {
					// Packed array with elements: Use ECALL_PACKED_ARRAY_OPS (548)
					// Signature: a0=op(type), a1=result_ptr, a2=array_ptr, a3=array_size
					// The syscall converts an Array of Variants to a PackedArray
					int args_space = element_count * variant_size();
					args_space = (args_space + 15) & ~15; // Align to 16 bytes

					// Allocate stack space
					emit_stack_adjust(-args_space);

					// Copy each element variant to the stack (as GuestVariant array)
					for (int i = 0; i < element_count; i++) {
						int elem_vreg = std::get<int>(instr.operands[2 + i].value);
						int elem_offset = get_variant_stack_offset(elem_vreg);
						int dst_offset = i * variant_size();

						// Copy the whole Variant from source to destination
						emit_variant_move(REG_SP, dst_offset, REG_SP, args_space + elem_offset, REG_T0);
					}

					// a0 = op (the packed array type, which also indicates CREATE_FROM_ARRAY)
					emit_li(REG_A0, variant_type);

					// a1 = pointer to destination Variant
					// After SP -= args_space, it's now at result_offset + args_space from NEW SP
					int adjusted_dst_offset = result_offset + args_space;
					emit_add_offset(REG_A1, REG_SP, adjusted_dst_offset);

					// a2 = pointer to element array (sp + 0)
					emit_mv(REG_A2, REG_SP);

					// a3 = element_count
					emit_li(REG_A3, element_count);

					// a7 = ECALL_PACKED_ARRAY_OPS (548)
					emit_li(REG_A7, 548);
					emit_ecall();

					// Restore stack pointer
					emit_add_offset(REG_SP, REG_SP, args_space);
				}

				break;
			}

			case IROpcode::VGET_INLINE: {
				// Format: VGET_INLINE result_reg, obj_reg, member_name, obj_type_hint
				if (instr.operands.size() != 4) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "VGET_INLINE requires 4 operands");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int obj_vreg = std::get<int>(instr.operands[1].value);
				std::string member = std::get<std::string>(instr.operands[2].value);
				int obj_type_hint = static_cast<int>(std::get<int64_t>(instr.operands[3].value));

				int result_offset = get_variant_stack_offset(result_vreg);
				int obj_offset = get_variant_stack_offset(obj_vreg);

				// Determine the offset within the object Variant. Float vectors store
				// real_t components (4 or 8 bytes), integer vectors always int32_t.
				bool is_int_type = false;

				// Map member name to array index
				int component_idx = 0;
				if (member == "x" || member == "r") component_idx = 0;
				else if (member == "y" || member == "g") component_idx = 1;
				else if (member == "z" || member == "b") component_idx = 2;
				else if (member == "w" || member == "a") component_idx = 3;

				// Check if it's an integer vector type using helper function
				IRInstruction::TypeHint hint = static_cast<IRInstruction::TypeHint>(obj_type_hint);
				is_int_type = TypeHintUtils::is_int_vector(hint);

				int member_offset = is_int_type ? int_offset(component_idx) : real_offset(component_idx);

				if (is_int_type) {
					// Load 32-bit integer from object
					emit_lw(REG_T0, REG_SP, obj_offset + member_offset);

					// Create result Variant with INT type (2)
					emit_li(REG_T1, 2);
					emit_sw(REG_T1, REG_SP, result_offset); // m_type = INT

					// Sign-extend to 64-bit and store in v.i
					emit_sext_w(REG_T0, REG_T0);
					emit_sd(REG_T0, REG_SP, result_offset + VARIANT_DATA_OFFSET);
				} else {
					// For real_t components from Vector types: Variant::FLOAT is always a
					// 64-bit double, so widen unless real_t already is one.

					// Load the real_t component into an FP register
					emit_flr(REG_FA0, REG_SP, obj_offset + member_offset);

					// Widen real_t to double
					emit_fcvt_d_r(REG_FA0, REG_FA0);

					// Set result Variant type to FLOAT
					emit_li(REG_T0, Variant::FLOAT);
					emit_sw(REG_T0, REG_SP, result_offset); // m_type = FLOAT

					// Store 8-byte double to v.f field
					emit_fsd(REG_FA0, REG_SP, result_offset + VARIANT_DATA_OFFSET);
				}

				break;
			}

			case IROpcode::VGET: {
				// VGET format: result_reg, obj_reg, string_idx, string_len
				// Uses ECALL_OBJ_PROP_GET (545)
				// Calling convention:
				//   A0 = object address (v.i from object Variant)
				//   A1 = property name pointer
				//   A2 = property name length
				//   A3 = pointer to result Variant

				if (instr.operands.size() != 4) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "VGET requires 4 operands (result_reg, obj_reg, string_idx, string_len)");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int obj_vreg = std::get<int>(instr.operands[1].value);
				int string_idx = static_cast<int>(std::get<int64_t>(instr.operands[2].value));
				int string_len = static_cast<int>(std::get<int64_t>(instr.operands[3].value));

				// Get the string constant
				if (string_idx < 0 || static_cast<size_t>(string_idx) >= m_string_constants->size()) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "String constant index out of range");
				}
				const std::string& str = (*m_string_constants)[string_idx];

				int result_offset = get_variant_stack_offset(result_vreg);
				int obj_offset = get_variant_stack_offset(obj_vreg);

				// ECALL_OBJ_PROP_GET clobbers a0-a3
				std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2, REG_A3};
				auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_fn.current_instr_idx);

				for (const auto& move : moves) {
					emit_mv(move.second, move.first);
				}

				// A0 = object address (load from Variant's v.i field at offset 8)
				// IMPORTANT: Load this BEFORE adjusting the stack, so obj_offset is correct
				emit_ld(REG_A0, REG_SP, obj_offset + 8);

				// Allocate stack space for the string
				int str_space = ((string_len + 1) + 7) & ~7; // Align to 8 bytes, +1 for null terminator

				emit_stack_adjust(-str_space);

				// Store string on stack
				for (size_t i = 0; i < str.length(); i++) {
					emit_li(REG_T0, static_cast<unsigned char>(str[i]));
					emit_sb(REG_T0, REG_SP, i); // SB (store byte)
				}
				// Store null terminator
				emit_sb(REG_ZERO, REG_SP, string_len); // SB

				// A1 = pointer to property name string (sp)
				emit_mv(REG_A1, REG_SP);

				// A2 = string length
				emit_li(REG_A2, string_len);

				// A3 = pointer to result Variant
				// IMPORTANT: Account for str_space since SP was adjusted
				emit_load_stack_offset(REG_A3, result_offset + str_space);

				// A7 = syscall number (545 for ECALL_OBJ_PROP_GET)
				emit_li(REG_A7, 545);
				emit_ecall();

				// Restore stack pointer
				emit_stack_adjust(str_space);

				break;
			}

			case IROpcode::VSET: {
				// VSET format: obj_reg, string_idx, string_len, value_reg
				// Uses ECALL_OBJ_PROP_SET (546)
				// Calling convention:
				//   A0 = object address (v.i from object Variant)
				//   A1 = property name pointer
				//   A2 = property name length
				//   A3 = pointer to value Variant

				if (instr.operands.size() != 4) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "VSET requires 4 operands (obj_reg, string_idx, string_len, value_reg)");
				}

				int obj_vreg = std::get<int>(instr.operands[0].value);
				int string_idx = static_cast<int>(std::get<int64_t>(instr.operands[1].value));
				int string_len = static_cast<int>(std::get<int64_t>(instr.operands[2].value));
				int value_vreg = std::get<int>(instr.operands[3].value);

				// Get the string constant
				if (string_idx < 0 || static_cast<size_t>(string_idx) >= m_string_constants->size()) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "String constant index out of range");
				}
				const std::string& str = (*m_string_constants)[string_idx];

				int obj_offset = get_variant_stack_offset(obj_vreg);
				int value_offset = get_variant_stack_offset(value_vreg);

				// ECALL_OBJ_PROP_SET clobbers a0-a3
				std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2, REG_A3};
				auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_fn.current_instr_idx);

				for (const auto& move : moves) {
					emit_mv(move.second, move.first);
				}

				// A0 = object address (load from Variant's v.i field at offset 8)
				// IMPORTANT: Load this BEFORE adjusting the stack, so obj_offset is correct
				emit_ld(REG_A0, REG_SP, obj_offset + 8);

				// Allocate stack space for the string
				int str_space = ((string_len + 1) + 7) & ~7; // Align to 8 bytes, +1 for null terminator

				emit_stack_adjust(-str_space);

				// Store string on stack
				for (size_t i = 0; i < str.length(); i++) {
					emit_li(REG_T0, static_cast<unsigned char>(str[i]));
					emit_sb(REG_T0, REG_SP, i); // SB (store byte)
				}
				// Store null terminator
				emit_sb(REG_ZERO, REG_SP, string_len); // SB

				// A1 = pointer to property name string (sp)
				emit_mv(REG_A1, REG_SP);

				// A2 = string length
				emit_li(REG_A2, string_len);

				// A3 = pointer to value Variant
				// IMPORTANT: Calculate this AFTER adjusting the stack, and account for str_space
				// emit_load_stack_offset does: addi a3, sp, offset, so we get the correct address directly
				emit_load_stack_offset(REG_A3, value_offset + str_space);

				// A7 = syscall number (546 for ECALL_OBJ_PROP_SET)
				emit_li(REG_A7, 546);
				emit_ecall();

				// Restore stack pointer
				emit_stack_adjust(str_space);

				break;
			}

			// Not implementing these for now
			case IROpcode::MAKE_RECT2:
			case IROpcode::MAKE_RECT2I:
			case IROpcode::MAKE_PLANE:
			case IROpcode::VSET_INLINE:
				throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Opcode not yet implemented in RISC-V codegen");

			case IROpcode::CALL_SYSCALL: {
				// CALL_SYSCALL format: result_reg, syscall_number, arg1, arg2, ...
				// Different syscalls have different calling conventions:
				// ECALL_GET_OBJ (504): result_reg, 504, string_index, string_length
				// ECALL_ARRAY_SIZE (523): result_reg, 523, array_vreg
				// ECALL_ARRAY_AT (522): result_reg, 522, array_vreg, index_vreg

				if (instr.operands.size() < 2) {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "CALL_SYSCALL requires at least 2 operands (result_reg, syscall_num)");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int syscall_num = static_cast<int>(std::get<int64_t>(instr.operands[1].value));

				// Handle different syscalls based on their calling conventions
				if (syscall_num == 504) {
					// ECALL_GET_OBJ: result_reg, 504, string_index, string_length
					if (instr.operands.size() != 4) {
						throw CompilerException(ErrorType::RISCV_codegen_ERROR, "ECALL_GET_OBJ requires 4 operands");
					}

					int string_idx = static_cast<int>(std::get<int64_t>(instr.operands[2].value));
					int string_len = static_cast<int>(std::get<int64_t>(instr.operands[3].value));

					// Get the string constant
					if (string_idx < 0 || static_cast<size_t>(string_idx) >= m_string_constants->size()) {
						throw CompilerException(ErrorType::RISCV_codegen_ERROR, "String constant index out of range");
					}
					const std::string& str = (*m_string_constants)[string_idx];

					int result_offset = get_variant_stack_offset(result_vreg);

					// ECALL_GET_OBJ clobbers a0 and a1
					std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1};
					auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_fn.current_instr_idx);

					for (const auto& move : moves) {
						emit_mv(move.second, move.first);
					}

					// Allocate stack space for the string
					int str_space = ((string_len + 1) + 7) & ~7; // Align to 8 bytes, +1 for null terminator

					emit_stack_adjust(-str_space);

					// Store string on stack
					for (size_t i = 0; i < str.length(); i++) {
						emit_li(REG_T0, static_cast<unsigned char>(str[i]));
						emit_sb(REG_T0, REG_SP, i); // SB (store byte)
					}
					// Store null terminator
					emit_sb(REG_ZERO, REG_SP, string_len); // SB

					// a0 = pointer to class name string (sp) - FIRST ARGUMENT
					emit_mv(REG_A0, REG_SP);

					// a1 = string length - SECOND ARGUMENT
					emit_li(REG_A1, string_len);

					// a7 = syscall number (504 for ECALL_GET_OBJ)
					emit_li(REG_A7, syscall_num);
					emit_ecall();

					// After ecall, a0 contains the object reference (32-bit int)
					// First, restore stack pointer (before storing result)
					emit_stack_adjust(str_space);

					// Store syscall result
					emit_syscall_result(result_vreg, REG_A0, result_offset, 24); // GDOBJECT type = 24

				} else if (syscall_num == 523) {
					// ECALL_ARRAY_SIZE: result_reg, 523, array_vreg
					// Takes: a0 = array variant index (unsigned)
					// Returns: a0 = array size (int)
					if (instr.operands.size() != 3) {
						throw CompilerException(ErrorType::RISCV_codegen_ERROR, "ECALL_ARRAY_SIZE requires 3 operands");
					}

					int array_vreg = static_cast<int>(std::get<int>(instr.operands[2].value));

					// Ensure result variant has a stack slot BEFORE handling clobbering
					// This prevents the allocator from moving it during syscall setup
					int result_offset = get_variant_stack_offset(result_vreg);
					int array_offset = get_variant_stack_offset(array_vreg);

					// ECALL_ARRAY_SIZE clobbers a0
					std::vector<uint8_t> clobbered_regs = {REG_A0};
					auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_fn.current_instr_idx);

					for (const auto& move : moves) {
						emit_mv(move.second, move.first);
					}

					// Load the array variant index from offset 8 of the array Variant
					emit_lw(REG_A0, REG_SP, array_offset + 8); // Load 32-bit variant index

					// a7 = syscall number (523 for ECALL_ARRAY_SIZE)
					emit_li(REG_A7, syscall_num);
					emit_ecall();

					// After ecall, a0 contains the size (64-bit int)
					// Store syscall result
					emit_syscall_result(result_vreg, REG_A0, result_offset, 2); // INT type = 2

				} else if (syscall_num == 522) {
					// ECALL_ARRAY_AT: result_reg, 522, array_vreg, index_vreg
					// Takes: a0 = array variant index (unsigned), a1 = index (int), a2 = result GuestVariant pointer
					// Returns: result variant is filled with the element
					if (instr.operands.size() != 4) {
						throw CompilerException(ErrorType::RISCV_codegen_ERROR, "ECALL_ARRAY_AT requires 4 operands");
					}

					int array_vreg = static_cast<int>(std::get<int>(instr.operands[2].value));
					int index_vreg = static_cast<int>(std::get<int>(instr.operands[3].value));

					// Ensure all variants have stack slots BEFORE handling clobbering
					// This prevents the allocator from moving them during syscall setup
					int result_offset = get_variant_stack_offset(result_vreg);
					int array_offset = get_variant_stack_offset(array_vreg);
					int index_offset = get_variant_stack_offset(index_vreg);

					// ECALL_ARRAY_AT clobbers a0, a1, a2
					std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2};
					auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_fn.current_instr_idx);

					for (const auto& move : moves) {
						emit_mv(move.second, move.first);
					}

					// a0 = array variant index (load from offset 8 of array Variant)
					emit_lw(REG_A0, REG_SP, array_offset + 8); // Load 32-bit variant index

					// a1 = index (load from index_vreg)
					// The index is stored as a Variant, we need to extract the integer value
					// NOTE: Use emit_ld (64-bit) because integer Variants store 64-bit values, not 32-bit!
					emit_ld(REG_A1, REG_SP, index_offset + 8); // Load 64-bit integer from offset 8

					// a2 = pointer to result GuestVariant
					emit_i_type(0x13, REG_A2, 0, REG_SP, result_offset); // addi a2, sp, result_offset

					// a7 = syscall number (522 for ECALL_ARRAY_AT)
					emit_li(REG_A7, syscall_num);
					emit_ecall();

				} else if (syscall_num == 507) {
					// ECALL_GET_NODE: result_reg, 507, addr, [path_vreg]
					// Takes: a0 = base node address (0 for owner), a1 = path string pointer, a2 = path length
					// Returns: a0 = node object reference
					if (instr.operands.size() < 3) {
						throw CompilerException(ErrorType::RISCV_codegen_ERROR, "ECALL_GET_NODE requires at least 3 operands");
					}

					int base_addr = static_cast<int>(std::get<int64_t>(instr.operands[2].value));
					bool has_path = instr.operands.size() >= 4;

					// Ensure result variant has a stack slot BEFORE handling clobbering
					int result_offset = get_variant_stack_offset(result_vreg);

					// ECALL_GET_NODE clobbers a0, a1, a2
					std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2};
					auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_fn.current_instr_idx);

					for (const auto& move : moves) {
						emit_mv(move.second, move.first);
					}

					if (has_path) {
						// get_node(path) - load path from variant register
						int path_vreg = static_cast<int>(std::get<int>(instr.operands[3].value));
						int path_offset = get_variant_stack_offset(path_vreg);

						// Load the String variant from path_vreg
						// String variant structure: type(4) + padding(4) + data(16)
						// For strings, we need to extract the pointer and length

						// a0 = base node address (0 for owner)
						emit_li(REG_A0, base_addr);

						// Load string pointer from offset 8 of the String variant
						emit_ld(REG_A1, REG_SP, path_offset + 8); // Load 64-bit pointer

						// Load string length from offset 16 (assuming it's stored there)
						// For now, we'll use a fixed approach - this may need adjustment
						// String length might be at offset 16 or we may need to query it
						emit_ld(REG_A2, REG_SP, path_offset + 16); // Load 64-bit length
					} else {
						// get_node() - no path argument, get the current node using "."
						// Allocate space for the "." string (2 bytes: '.' + null terminator)
						int dot_str_space = 8; // Align to 8 bytes
						emit_stack_adjust(-dot_str_space);

						// Store "." string on stack
						emit_li(REG_T0, static_cast<uint8_t>('.'));
						emit_sb(REG_T0, REG_SP, 0); // Store '.'
						emit_sb(REG_ZERO, REG_SP, 1); // Store null terminator

						// a0 = base node address (0 for owner)
						emit_li(REG_A0, base_addr);

						// a1 = pointer to "." string
						emit_mv(REG_A1, REG_SP);

						// a2 = string length (1)
						emit_li(REG_A2, 1);
					}

					// a7 = syscall number (507 for ECALL_GET_NODE)
					emit_li(REG_A7, syscall_num);
					emit_ecall();

					// After ecall, a0 contains the node object reference
					// Restore stack if we allocated space for the "."
					if (!has_path) {
						emit_stack_adjust(8); // Restore the stack space for "."
					}

					// Store syscall result
					emit_syscall_result(result_vreg, REG_A0, result_offset, 24); // OBJECT type = 24

				} else {
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Unknown syscall number: " + std::to_string(syscall_num));
				}

				break;
			}

			// No `default:` here on purpose: every opcode in ir_opcodes.def has
			// to be listed, so adding one is a compile error here rather than a
			// program that silently omits it.
		}
	}
}

// Instruction encoding helpers
void RISCVCodeGen::emit_word(uint32_t word) {
	m_code.push_back(word & 0xFF);
	m_code.push_back((word >> 8) & 0xFF);
	m_code.push_back((word >> 16) & 0xFF);
	m_code.push_back((word >> 24) & 0xFF);
}

void RISCVCodeGen::emit_r_type(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, uint8_t rs2, uint8_t funct7) {
	uint32_t instr = opcode | (rd << 7) | (funct3 << 12) | (rs1 << 15) | (rs2 << 20) | (funct7 << 25);
	emit_word(instr);
}

bool RISCVCodeGen::fits_in_signed(int64_t value, int bits) {
	const int64_t limit = int64_t(1) << (bits - 1);
	return value >= -limit && value < limit;
}

void RISCVCodeGen::check_immediate(const std::string& what, int64_t value, int bits) {
	if (fits_in_signed(value, bits)) {
		return;
	}
	throw CompilerException(ErrorType::RISCV_codegen_ERROR,
		what + " immediate " + std::to_string(value) + " does not fit in " +
		std::to_string(bits) + " signed bits (" +
		std::to_string(-(int64_t(1) << (bits - 1))) + " to " +
		std::to_string((int64_t(1) << (bits - 1)) - 1) + ")");
}

void RISCVCodeGen::check_displacement(const std::string& what, int64_t offset, int bits) {
	if ((offset & 1) != 0) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			what + " displacement " + std::to_string(offset) + " is not even");
	}
	if (!fits_in_signed(offset, bits)) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			what + " displacement " + std::to_string(offset) +
			" is out of reach (" + std::to_string(bits) + " signed bits). "
			"The function is too large for a single branch to span.");
	}
}

void RISCVCodeGen::emit_i_type(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, int32_t imm) {
	check_immediate("I-type", imm, I_TYPE_IMM_BITS);
	uint32_t instr = opcode | (rd << 7) | (funct3 << 12) | (rs1 << 15) | ((imm & 0xFFF) << 20);
	emit_word(instr);
}

void RISCVCodeGen::emit_s_type(uint8_t opcode, uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm) {
	check_immediate("S-type (store)", imm, S_TYPE_IMM_BITS);
	uint32_t imm_lo = imm & 0x1F;
	uint32_t imm_hi = (imm >> 5) & 0x7F;
	uint32_t instr = opcode | (imm_lo << 7) | (funct3 << 12) | (rs1 << 15) | (rs2 << 20) | (imm_hi << 25);
	m_code.push_back(instr & 0xFF);
	m_code.push_back((instr >> 8) & 0xFF);
	m_code.push_back((instr >> 16) & 0xFF);
	m_code.push_back((instr >> 24) & 0xFF);
}

void RISCVCodeGen::emit_b_type(uint8_t opcode, uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm) {
	check_displacement("B-type (branch)", imm, B_TYPE_IMM_BITS);
	// B-type immediate encoding (weird layout)
	uint32_t imm12 = (imm >> 12) & 1;
	uint32_t imm10_5 = (imm >> 5) & 0x3F;
	uint32_t imm4_1 = (imm >> 1) & 0xF;
	uint32_t imm11 = (imm >> 11) & 1;

	uint32_t instr = opcode | (imm11 << 7) | (imm4_1 << 8) | (funct3 << 12) | (rs1 << 15) | (rs2 << 20) | (imm10_5 << 25) | (imm12 << 31);
	m_code.push_back(instr & 0xFF);
	m_code.push_back((instr >> 8) & 0xFF);
	m_code.push_back((instr >> 16) & 0xFF);
	m_code.push_back((instr >> 24) & 0xFF);
}

void RISCVCodeGen::emit_u_type(uint8_t opcode, uint8_t rd, uint32_t imm) {
	// A U-type immediate is the upper 20 bits, already shifted into place. Low
	// bits would be silently dropped, so a caller that has them is confused
	// about what this takes.
	if ((imm & 0xFFF) != 0) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"U-type immediate " + std::to_string(imm) + " has bits below bit 12, which the "
			"instruction cannot encode");
	}
	uint32_t instr = opcode | (rd << 7) | (imm & 0xFFFFF000);
	m_code.push_back(instr & 0xFF);
	m_code.push_back((instr >> 8) & 0xFF);
	m_code.push_back((instr >> 16) & 0xFF);
	m_code.push_back((instr >> 24) & 0xFF);
}

void RISCVCodeGen::emit_j_type(uint8_t opcode, uint8_t rd, int32_t imm) {
	check_displacement("J-type (jump)", imm, J_TYPE_IMM_BITS);
	// J-type immediate encoding
	uint32_t imm20 = (imm >> 20) & 1;
	uint32_t imm10_1 = (imm >> 1) & 0x3FF;
	uint32_t imm11 = (imm >> 11) & 1;
	uint32_t imm19_12 = (imm >> 12) & 0xFF;

	uint32_t instr = opcode | (rd << 7) | (imm19_12 << 12) | (imm11 << 20) | (imm10_1 << 21) | (imm20 << 31);
	m_code.push_back(instr & 0xFF);
	m_code.push_back((instr >> 8) & 0xFF);
	m_code.push_back((instr >> 16) & 0xFF);
	m_code.push_back((instr >> 24) & 0xFF);
}

void RISCVCodeGen::emit_r4_type(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, uint8_t rs2, uint8_t rs3, uint8_t funct2)
{
	union {
		struct {
			uint32_t opcode : 7;
			uint32_t rd     : 5;
			uint32_t funct3 : 3;
			uint32_t rs1    : 5;
			uint32_t rs2    : 5;
			uint32_t funct2 : 2;
			uint32_t rs3    : 5;
		} R4type;
		uint32_t value;
	} r4;
	r4.R4type.opcode = opcode;
	r4.R4type.rd = rd;
	r4.R4type.funct3 = funct3;
	r4.R4type.rs1 = rs1;
	r4.R4type.rs2 = rs2;
	r4.R4type.funct2 = funct2;
	r4.R4type.rs3 = rs3;
	emit_word(r4.value);
}

// Higher-level instructions
void RISCVCodeGen::emit_li(uint8_t rd, int64_t imm) {
	// Load immediate - handles 64-bit values
	// For small values, use addi; for larger, build up the value
	if (fits_in_signed(imm, I_TYPE_IMM_BITS)) {
		// Small immediate: addi rd, x0, imm
		emit_i_type(0x13, rd, 0, REG_ZERO, static_cast<int32_t>(imm));
	} else if (imm >= INT32_MIN && imm <= INT32_MAX) {
		// 32-bit immediate: lui + addi
		int32_t imm32 = static_cast<int32_t>(imm);
		int32_t upper = (imm32 + 0x800) >> 12; // Add 0x800 for rounding
		emit_u_type(0x37, rd, upper << 12);

		// The low 12 bits are the two's complement remainder after the rounded
		// upper part, so they have to be sign-extended to be a valid I-type
		// immediate rather than a raw 0..4095 bit pattern.
		int32_t lower = ((imm32 & 0xFFF) ^ 0x800) - 0x800;
		if (lower != 0 || upper == 0) {
			emit_i_type(0x13, rd, 0, rd, lower);
		}
	} else {
		// Full 64-bit immediate: load from constant pool appended to .text
		// This avoids using temporary registers and generates cleaner code
		size_t const_index = add_constant(imm);

		// We'll use a label to refer to the constant pool entry
		// The constant will be located at: code_end + (const_index * 8)
		// We use AUIPC to get PC-relative address, then LD to load the value
		std::string label = ".LC" + std::to_string(const_index);

		// Mark this as a use of the constant label
		// Only mark AUIPC since we'll patch both AUIPC and LD together
		size_t auipc_offset = m_code.size();
		mark_label_use(label, auipc_offset);
		emit_u_type(0x17, rd, 0);  // auipc rd, 0 (will be patched)

		// Emit LD instruction (offset will be patched when AUIPC is resolved)
		emit_ld(rd, rd, 0);  // ld rd, 0(rd) (offset will be patched)
	}
}

void RISCVCodeGen::emit_mv(uint8_t rd, uint8_t rs) {
	// mv rd, rs = addi rd, rs, 0
	emit_i_type(0x13, rd, 0, rs, 0);
}

void RISCVCodeGen::emit_addi(uint8_t rd, uint8_t rs1, int32_t imm) {
	emit_i_type(0x13, rd, 0, rs1, imm);
}

void RISCVCodeGen::emit_add_offset(uint8_t rd, uint8_t base, int32_t offset) {
	if (fits_in_signed(offset, I_TYPE_IMM_BITS)) {
		emit_addi(rd, base, offset);
		return;
	}

	// Too wide for the immediate: materialise it and add. rd can hold it
	// itself unless it is also the base, in which case the base would be
	// destroyed before it is read; the reserved scratch covers that.
	const uint8_t temp = (rd == base) ? REG_WIDE_SCRATCH : rd;
	emit_li(temp, offset);
	emit_add(rd, base, temp);
}

bool RISCVCodeGen::opcode_clobbers_abi_registers(IROpcode op) {
	switch (op) {
		// Expansions that are nothing but loads, stores, arithmetic and
		// branches over the frame. Nothing here reaches the host, so a0-a7 and
		// ra survive across them.
		case IROpcode::LOAD_IMM:
		case IROpcode::LOAD_FLOAT_IMM:
		case IROpcode::LOAD_BOOL:
		case IROpcode::LOAD_GLOBAL:
		case IROpcode::MOVE:
		case IROpcode::LABEL:
		case IROpcode::JUMP:
		case IROpcode::RETURN:
			return false;

		// Everything else either makes a system call or calls another function
		// in the program. Several of these have a faster inline path when the
		// operand types are known -- typed integer arithmetic does not go
		// through VEVAL -- but the answer here is per-opcode and has to hold
		// for the slow path too.
		case IROpcode::LOAD_STRING:
		case IROpcode::STORE_GLOBAL:
		case IROpcode::CONVERT:
		case IROpcode::ADD:
		case IROpcode::SUB:
		case IROpcode::MUL:
		case IROpcode::DIV:
		case IROpcode::MOD:
		case IROpcode::NEG:
		case IROpcode::CMP_EQ:
		case IROpcode::CMP_NEQ:
		case IROpcode::CMP_LT:
		case IROpcode::CMP_LTE:
		case IROpcode::CMP_GT:
		case IROpcode::CMP_GTE:
		case IROpcode::AND:
		case IROpcode::OR:
		case IROpcode::NOT:
		case IROpcode::BIT_AND:
		case IROpcode::BIT_OR:
		case IROpcode::BIT_XOR:
		case IROpcode::BIT_NOT:
		case IROpcode::SHL:
		case IROpcode::SHR:
		// A branch on a Variant of unknown type asks the host to booleanize it:
		// only Godot knows whether a String or an Array is empty.
		case IROpcode::BRANCH_ZERO:
		case IROpcode::BRANCH_NOT_ZERO:
		case IROpcode::BRANCH_EQ:
		case IROpcode::BRANCH_NEQ:
		case IROpcode::BRANCH_LT:
		case IROpcode::BRANCH_LTE:
		case IROpcode::BRANCH_GT:
		case IROpcode::BRANCH_GTE:
		case IROpcode::CALL:
		case IROpcode::CALL_SYSCALL:
		case IROpcode::VCALL:
		case IROpcode::VGET:
		case IROpcode::VSET:
		case IROpcode::PRINT:
		case IROpcode::GLOBAL_CALL:
		case IROpcode::MAKE_VECTOR2:
		case IROpcode::MAKE_VECTOR3:
		case IROpcode::MAKE_VECTOR4:
		case IROpcode::MAKE_VECTOR2I:
		case IROpcode::MAKE_VECTOR3I:
		case IROpcode::MAKE_VECTOR4I:
		case IROpcode::MAKE_COLOR:
		case IROpcode::MAKE_RECT2:
		case IROpcode::MAKE_RECT2I:
		case IROpcode::MAKE_PLANE:
		case IROpcode::MAKE_ARRAY:
		case IROpcode::MAKE_DICTIONARY:
		case IROpcode::MAKE_PACKED_BYTE_ARRAY:
		case IROpcode::MAKE_PACKED_INT32_ARRAY:
		case IROpcode::MAKE_PACKED_INT64_ARRAY:
		case IROpcode::MAKE_PACKED_FLOAT32_ARRAY:
		case IROpcode::MAKE_PACKED_FLOAT64_ARRAY:
		case IROpcode::MAKE_PACKED_STRING_ARRAY:
		case IROpcode::MAKE_PACKED_VECTOR2_ARRAY:
		case IROpcode::MAKE_PACKED_VECTOR3_ARRAY:
		case IROpcode::MAKE_PACKED_COLOR_ARRAY:
		case IROpcode::MAKE_PACKED_VECTOR4_ARRAY:
		case IROpcode::VGET_INLINE:
		case IROpcode::VSET_INLINE:
			return true;
	}
	throw CompilerException(ErrorType::RISCV_codegen_ERROR,
		"Unknown IR opcode in ABI clobber analysis");
}

std::vector<bool> RISCVCodeGen::find_return_forwarding(const IRFunction& func) {
	// A function that ends `MOVE r0, rN; RETURN` or `LOAD_GLOBAL r0, g; RETURN`
	// copies a Variant into r0's stack slot and then copies the same Variant
	// out of that slot into the caller's. The slot is a waypoint nobody reads,
	// so the first copy can be made directly into *a0 and the second dropped --
	// on a Variant that is six loads and six stores instead of twelve.
	//
	// Two things have to hold. The RETURN must be the very next instruction, so
	// the two are in the same basic block and nothing can run between them. And
	// nothing reachable afterwards may read r0, because its slot is left
	// holding whatever it held before: RETURN is a terminator, so the code
	// after it is only reachable through a label, and a backwards branch into a
	// loop that reads r0 would read a stale value.
	std::vector<bool> forward(func.instructions.size(), false);

	std::vector<int> reads;
	for (size_t i = 0; i + 1 < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];
		if (func.instructions[i + 1].opcode != IROpcode::RETURN) {
			continue;
		}
		switch (instr.opcode) {
			case IROpcode::LOAD_IMM:
			case IROpcode::LOAD_FLOAT_IMM:
			case IROpcode::LOAD_BOOL:
			case IROpcode::LOAD_GLOBAL:
			case IROpcode::MOVE:
				break; // Writes its result through one base register, so it can write through a0.
			default:
				continue; // Everything else builds its result in the frame.
		}
		if (ir_destination_register(instr) != IRFunction::RETURN_REGISTER) {
			continue;
		}
		// A self-move writes nothing, so there would be nothing to forward.
		reads.clear();
		ir_collect_read_registers(instr, reads);
		if (std::find(reads.begin(), reads.end(), IRFunction::RETURN_REGISTER) != reads.end()) {
			continue;
		}

		// The scan starts past the RETURN: RETURN's read of the return
		// register is the one this forwards, not one that stops it.
		bool read_later = false;
		for (size_t j = i + 2; j < func.instructions.size() && !read_later; j++) {
			reads.clear();
			ir_collect_read_registers(func.instructions[j], reads);
			read_later = std::find(reads.begin(), reads.end(), IRFunction::RETURN_REGISTER) != reads.end();
		}
		forward[i] = !read_later;
	}

	return forward;
}

void RISCVCodeGen::emit_load_return_pointer() {
	// a0 arrives holding the caller's return Variant and stays there unless
	// something in this function goes through the host, which takes a0 as its
	// first argument register.
	if (m_fn.spills_return_pointer) {
		emit_ld(REG_A0, REG_SP, SAVED_A0_OFFSET);
	}
}

void RISCVCodeGen::emit_address_of_global(uint8_t rd, size_t index) {
	const int64_t byte_offset = static_cast<int64_t>(index) * variant_size();
	if (byte_offset > INT32_MAX) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"Global variable " + std::to_string(index) + " is past the end of a 32-bit address space");
	}
	// The offset goes into the relocation, not into a following `addi`: an
	// `addi` truncates to 12 signed bits, which a program stops fitting in at
	// 85 globals.
	emit_la(rd, GLOBALS_LABEL, static_cast<int32_t>(byte_offset));
}

void RISCVCodeGen::emit_la(uint8_t rd, const std::string& label, int32_t addend) {
	// Load address using AUIPC + ADDI
	// auipc rd, 0  # Load PC-relative upper bits
	// addi rd, rd, offset  # Add lower bits (will be patched)
	//
	// The addend is resolved together with the label, so `label + addend` is
	// reachable for any 32-bit addend. Emitting a separate `addi rd, rd, addend`
	// instead would truncate to a 12-bit signed immediate.

	size_t auipc_offset = m_code.size();
	mark_label_use(label, auipc_offset, addend);
	emit_u_type(0x17, rd, 0);  // auipc rd, 0 (will be patched)

	// Emit ADDI instruction (offset will be patched when AUIPC is resolved)
	emit_i_type(0x13, rd, 0, rd, 0);  // addi rd, rd, 0 (will be patched)
}

void RISCVCodeGen::emit_add(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 0, rs1, rs2, 0);
}

void RISCVCodeGen::emit_sub(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 0, rs1, rs2, 0x20);
}

void RISCVCodeGen::emit_mul(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 0, rs1, rs2, 1); // RV64M extension
}

void RISCVCodeGen::emit_div(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 4, rs1, rs2, 1); // RV64M extension - div
}

void RISCVCodeGen::emit_rem(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 6, rs1, rs2, 1); // RV64M extension - rem
}

void RISCVCodeGen::emit_and(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 7, rs1, rs2, 0);
}

void RISCVCodeGen::emit_or(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 6, rs1, rs2, 0);
}

void RISCVCodeGen::emit_xor(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 4, rs1, rs2, 0);
}

void RISCVCodeGen::emit_sll(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 1, rs1, rs2, 0);
}

void RISCVCodeGen::emit_srl(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 5, rs1, rs2, 0);
}

void RISCVCodeGen::emit_sra(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 5, rs1, rs2, 0x20);
}

void RISCVCodeGen::emit_slt(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 2, rs1, rs2, 0);
}

void RISCVCodeGen::emit_xori(uint8_t rd, uint8_t rs, int32_t imm) {
	emit_i_type(0x13, rd, 4, rs, imm); // XORI
}

void RISCVCodeGen::emit_seqz(uint8_t rd, uint8_t rs) {
	// seqz rd, rs is pseudo-instruction for sltiu rd, rs, 1
	emit_i_type(0x13, rd, 3, rs, 1); // SLTIU rd, rs, 1
}

void RISCVCodeGen::emit_snez(uint8_t rd, uint8_t rs) {
	// snez rd, rs is pseudo-instruction for sltu rd, x0, rs
	emit_r_type(0x33, rd, 3, REG_ZERO, rs, 0); // SLTU rd, x0, rs
}

void RISCVCodeGen::emit_beq(uint8_t rs1, uint8_t rs2, int32_t offset) {
	emit_b_type(0x63, 0, rs1, rs2, offset);
}

void RISCVCodeGen::emit_bne(uint8_t rs1, uint8_t rs2, int32_t offset) {
	emit_b_type(0x63, 1, rs1, rs2, offset);
}

void RISCVCodeGen::emit_blt(uint8_t rs1, uint8_t rs2, int32_t offset) {
	emit_b_type(0x63, 4, rs1, rs2, offset);
}

void RISCVCodeGen::emit_bge(uint8_t rs1, uint8_t rs2, int32_t offset) {
	emit_b_type(0x63, 5, rs1, rs2, offset);
}

void RISCVCodeGen::emit_bltu(uint8_t rs1, uint8_t rs2, int32_t offset) {
	emit_b_type(0x63, 6, rs1, rs2, offset);
}

void RISCVCodeGen::emit_bgeu(uint8_t rs1, uint8_t rs2, int32_t offset) {
	emit_b_type(0x63, 7, rs1, rs2, offset);
}

void RISCVCodeGen::emit_jal(uint8_t rd, int32_t offset) {
	emit_j_type(0x6F, rd, offset);
}

void RISCVCodeGen::emit_jalr(uint8_t rd, uint8_t rs1, int32_t offset) {
	emit_i_type(0x67, rd, 0, rs1, offset);
}

void RISCVCodeGen::emit_ecall() {
	// The prologue skipped spilling the caller's return-value pointer because
	// opcode_clobbers_abi_registers() said no instruction in this function
	// reaches the host. A system call here says otherwise, and a0 is about to
	// be overwritten with a syscall argument. Refusing to encode it turns a
	// wrong row in that switch into a failed compile instead of a function that
	// returns whatever the last system call left behind.
	if (m_fn.in_function && !m_fn.spills_return_pointer) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"System call emitted in a function whose prologue did not save the return-value pointer");
	}
	// ecall instruction: opcode = 0x73, funct3 = 0, rs1 = 0, rd = 0, imm = 0
	emit_i_type(0x73, 0, 0, 0, 0);
}

void RISCVCodeGen::emit_ret() {
	// ret = jalr x0, x1, 0
	emit_jalr(REG_ZERO, REG_RA, 0);
}

// Label management
void RISCVCodeGen::define_label(const std::string& label) {
	m_labels[label] = m_code.size();
}

void RISCVCodeGen::mark_label_use(const std::string& label, size_t code_offset, int32_t addend) {
	m_label_uses.push_back({label, code_offset, addend});
}

void RISCVCodeGen::relax_branches() {
	// B-type: funct3 0=beq 1=bne 4=blt 5=bge 6=bltu 7=bgeu. Flipping the low
	// bit inverts every one of them.
	auto invert_condition = [](uint8_t funct3) -> uint8_t { return funct3 ^ 1; };

	bool changed = true;
	while (changed) {
		changed = false;

		for (size_t use_index = 0; use_index < m_label_uses.size(); use_index++) {
			const LabelUse use = m_label_uses[use_index];
			if (use.code_offset + 4 > m_code.size()) {
				continue;
			}

			uint32_t instr = 0;
			std::memcpy(&instr, &m_code[use.code_offset], 4);
			if ((instr & 0x7F) != 0x63) {
				continue; // Not a conditional branch
			}

			auto label = m_labels.find(use.label);
			if (label == m_labels.end()) {
				continue; // resolve_labels() reports this
			}
			const int64_t displacement =
				static_cast<int64_t>(label->second) - static_cast<int64_t>(use.code_offset) + use.addend;
			if (fits_in_signed(displacement, B_TYPE_IMM_BITS)) {
				continue;
			}

			// Rewrite the branch in place to skip the jump that now follows it,
			// and insert that jump.
			const uint8_t funct3 = (instr >> 12) & 0x7;
			const uint8_t rs1 = (instr >> 15) & 0x1F;
			const uint8_t rs2 = (instr >> 20) & 0x1F;

			const int32_t skip = 8; // over the jal that follows
			const uint32_t imm11 = (skip >> 11) & 1;
			const uint32_t imm4_1 = (skip >> 1) & 0xF;
			const uint32_t imm10_5 = (skip >> 5) & 0x3F;
			const uint32_t imm12 = (skip >> 12) & 1;
			const uint32_t inverted = 0x63 | (imm11 << 7) | (imm4_1 << 8) |
				(invert_condition(funct3) << 12) | (rs1 << 15) | (rs2 << 20) |
				(imm10_5 << 25) | (imm12 << 31);
			std::memcpy(&m_code[use.code_offset], &inverted, 4);

			// jal x0, 0 -- the displacement is patched by resolve_labels().
			const size_t insert_at = use.code_offset + 4;
			const uint32_t jal = 0x6F;
			uint8_t bytes[4];
			std::memcpy(bytes, &jal, 4);
			m_code.insert(m_code.begin() + static_cast<long>(insert_at), bytes, bytes + 4);

			// Everything at or after the inserted instruction moved by 4.
			for (auto& entry : m_labels) {
				if (entry.second >= insert_at) {
					entry.second += 4;
				}
			}
			for (auto& entry : m_functions) {
				if (entry.second >= insert_at) {
					entry.second += 4;
				}
			}
			for (auto& other : m_label_uses) {
				if (other.code_offset >= insert_at) {
					other.code_offset += 4;
				}
			}

			// The branch no longer refers to a label -- its displacement is the
			// fixed +8 written above -- and the jump does.
			m_label_uses[use_index].code_offset = insert_at;

			changed = true;
			break; // Offsets moved; start again.
		}
	}
}

void RISCVCodeGen::resolve_labels() {
	for (const auto& use : m_label_uses) {
		const std::string& label = use.label;
		size_t use_offset = use.code_offset;

		auto it = m_labels.find(label);
		if (it == m_labels.end()) {
			throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Undefined label: " + label);
		}

		size_t target_offset = it->second;
		int32_t offset = static_cast<int32_t>(target_offset - use_offset) + use.addend;

		// Patch the instruction at use_offset with the correct offset
		uint32_t instr;
		memcpy(&instr, &m_code[use_offset], 4);

		uint8_t opcode = instr & 0x7F;

		if (opcode == 0x63) { // B-type (branch)
			// Patching re-encodes by hand rather than going through
			// emit_b_type(), so the range check has to be repeated here. A
			// branch that cannot reach its target would otherwise wrap around
			// and jump somewhere else entirely.
			check_displacement("B-type (branch) to '" + label + "'", offset, B_TYPE_IMM_BITS);

			// Re-encode with correct offset
			uint8_t funct3 = (instr >> 12) & 0x7;
			uint8_t rs1 = (instr >> 15) & 0x1F;
			uint8_t rs2 = (instr >> 20) & 0x1F;

			uint32_t imm12 = (offset >> 12) & 1;
			uint32_t imm10_5 = (offset >> 5) & 0x3F;
			uint32_t imm4_1 = (offset >> 1) & 0xF;
			uint32_t imm11 = (offset >> 11) & 1;

			instr = opcode | (imm11 << 7) | (imm4_1 << 8) | (funct3 << 12) | (rs1 << 15) | (rs2 << 20) | (imm10_5 << 25) | (imm12 << 31);
		} else if (opcode == 0x6F) { // J-type (jal)
			check_displacement("J-type (jump) to '" + label + "'", offset, J_TYPE_IMM_BITS);
			uint8_t rd = (instr >> 7) & 0x1F;

			uint32_t imm20 = (offset >> 20) & 1;
			uint32_t imm10_1 = (offset >> 1) & 0x3FF;
			uint32_t imm11 = (offset >> 11) & 1;
			uint32_t imm19_12 = (offset >> 12) & 0xFF;

			instr = opcode | (rd << 7) | (imm19_12 << 12) | (imm11 << 20) | (imm10_1 << 21) | (imm20 << 31);
		} else if (opcode == 0x17) { // U-type (AUIPC) - for constant pool or address loading
			uint8_t rd = (instr >> 7) & 0x1F;

			// Check if the next instruction is LD (for constant pool) or ADDI (for address)
			uint32_t next_instr;
			memcpy(&next_instr, &m_code[use_offset + 4], 4);
			uint8_t next_opcode = next_instr & 0x7F;

			if (next_opcode == 0x03) { // LD instruction (for constant pool access)
				// Calculate PC-relative offset
				// Split into upper 20 bits (for AUIPC) and lower 12 bits (for LD)
				int32_t upper = (offset + 0x800) >> 12; // Add 0x800 for sign extension
				int32_t lower = offset & 0xFFF;

				// Patch AUIPC with upper 20 bits
				instr = opcode | (rd << 7) | ((upper & 0xFFFFF) << 12);
				memcpy(&m_code[use_offset], &instr, 4);

				// Patch the following LD instruction with lower 12 bits
				// LD is at use_offset + 4
				uint8_t ld_rd = (next_instr >> 7) & 0x1F;
				uint8_t ld_rs1 = (next_instr >> 15) & 0x1F;
				uint8_t ld_funct3 = (next_instr >> 12) & 0x7;
				next_instr = 0x03 | (ld_rd << 7) | (ld_funct3 << 12) | (ld_rs1 << 15) | ((lower & 0xFFF) << 20);
				memcpy(&m_code[use_offset + 4], &next_instr, 4);

				// Skip the next use since we processed both AUIPC and LD together
				continue;
			} else if (next_opcode == 0x13) { // ADDI instruction (for load address)
				// AUIPC + ADDI pattern for load address (emit_la)
				uint8_t addi_funct3 = (next_instr >> 12) & 0x7;
				uint8_t addi_rs1 = (next_instr >> 15) & 0x1F;

				// For AUIPC+ADDI, the offset is the full 32-bit value
				// Split into upper 20 bits (for AUIPC) and lower 12 bits (for ADDI)
				int32_t upper = (offset + 0x800) >> 12; // Add 0x800 for sign extension
				int32_t lower = offset & 0xFFF;

				// Verify ADDI is using the same register as rd (rs1 == rd)
				// This is required for AUIPC+ADDI pattern
				if (addi_rs1 == rd && addi_funct3 == 0) {
					// Patch AUIPC with upper 20 bits
					instr = opcode | (rd << 7) | ((upper & 0xFFFFF) << 12);
					memcpy(&m_code[use_offset], &instr, 4);

					// Patch the following ADDI instruction with lower 12 bits
					uint8_t addi_rd = (next_instr >> 7) & 0x1F;
					next_instr = 0x13 | (addi_rd << 7) | (addi_funct3 << 12) | (addi_rs1 << 15) | ((lower & 0xFFF) << 20);
					memcpy(&m_code[use_offset + 4], &next_instr, 4);

					// Skip the next use since we processed both AUIPC and ADDI together
					continue;
				}
			}
		}

		memcpy(&m_code[use_offset], &instr, 4);
	}
}

int RISCVCodeGen::get_variant_stack_offset(int virtual_reg) {
	if (m_fn.omits_frame) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"Virtual register r" + std::to_string(virtual_reg) + " was asked for in a function generated without a stack frame");
	}
	auto it = m_fn.variant_offsets.find(virtual_reg);
	if (it != m_fn.variant_offsets.end()) {
		return it->second;
	}

	// The frame was sized from IRFunction::max_registers before the body was emitted,
	// so a virtual register that was never pre-assigned has no room in it. Growing the
	// frame here is not possible any more, and handing out an offset past its end would
	// silently corrupt the caller's frame, so fail the compilation instead.
	throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"Virtual register r" + std::to_string(virtual_reg) + " is outside the stack frame (max_registers too low)");
}

int RISCVCodeGen::get_scratch_variant_offset(int index) {
	if (m_fn.omits_frame) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"A scratch Variant was asked for in a function generated without a stack frame");
	}
	if (index < 0 || index >= SCRATCH_VARIANT_SLOTS) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
				"Scratch Variant slot " + std::to_string(index) + " is out of range");
	}
	// Stack layout: [saved ra (8)] [saved fp (8)] [saved a0 (8)] [Variants...] [scratch...]
	return SAVED_REG_SPACE + ((m_fn.scratch_slot_base + index) * variant_size());
}

void RISCVCodeGen::emit_variant_create_int(int stack_offset, int64_t value, uint8_t base_reg) {
	// Create an integer Variant
	// Variant layout: [uint32_t m_type (4 bytes)] [padding (4 bytes)] [int64_t v.i (8 bytes)]

	// Store m_type = INT (2) at stack_offset
	emit_li(REG_T0, 2); // INT type
	emit_store_variant_type(REG_T0, base_reg, stack_offset);

	// Store value
	emit_li(REG_T0, value);
	emit_store_variant_int(REG_T0, base_reg, stack_offset);
}

void RISCVCodeGen::emit_variant_create_float(int stack_offset, double value, uint8_t base_reg) {
	// v.f in a Variant is a 64-bit double whatever real_t is, so the bit
	// pattern goes in whole and there is no rounding to think about.
	int64_t bits;
	memcpy(&bits, &value, sizeof(double));

	emit_li(REG_T0, Variant::FLOAT);
	emit_store_variant_type(REG_T0, base_reg, stack_offset);

	emit_li(REG_T0, bits);
	emit_sd(REG_T0, base_reg, stack_offset + VARIANT_DATA_OFFSET);
}

void RISCVCodeGen::emit_variant_create_bool(int stack_offset, bool value, uint8_t base_reg) {
	// Similar to create_int but with BOOL type (1)
	emit_li(REG_T0, 1); // BOOL type
	emit_store_variant_type(REG_T0, base_reg, stack_offset);

	emit_li(REG_T0, value ? 1 : 0);
	emit_store_variant_bool(REG_T0, base_reg, stack_offset);
}

void RISCVCodeGen::emit_variant_create_string(int stack_offset, int string_idx) {
	// Create a string Variant using VCREATE syscall
	// VCREATE signature: void vcreate(Variant* dst, Variant::Type type, int method, void* data)
	// For strings with method==1: data = struct { const char* str; size_t length; }

	if (!m_string_constants || string_idx < 0 || string_idx >= static_cast<int>(m_string_constants->size())) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Invalid string constant index: " + std::to_string(string_idx));
	}

	const std::string& str = (*m_string_constants)[string_idx];
	int str_len = static_cast<int>(str.length());

	// Handle register clobbering (VCREATE uses a0-a3)
	std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2, REG_A3};
	auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_fn.current_instr_idx);
	for (const auto& move : moves) {
		emit_mv(move.second, move.first);
	}

	// Allocate stack space for: string data + struct { char*, size_t }
	int str_space = ((str_len + 1) + 7) & ~7; // String + null terminator, aligned to 8 bytes
	int struct_space = 16; // Two 8-byte fields
	int total_space = str_space + struct_space;
	total_space = (total_space + 15) & ~15; // Align to 16 bytes

	// Adjust stack pointer
	emit_add_offset(REG_SP, REG_SP, -total_space);

	// Store string data on stack
	for (size_t i = 0; i < str.length(); i++) {
		emit_li(REG_T0, static_cast<unsigned char>(str[i]));
		emit_sb(REG_T0, REG_SP, i); // SB (store byte)
	}
	// Store null terminator
	emit_sb(REG_ZERO, REG_SP, str_len); // SB

	// Create struct at sp + str_space
	// struct.str = sp (pointer to string data)
	emit_mv(REG_T0, REG_SP); // T0 = pointer to the string data at sp
	emit_sd(REG_T0, REG_SP, str_space); // SD (store pointer at sp + str_space)

	// struct.length = str_len
	emit_li(REG_T0, str_len);
	emit_sd(REG_T0, REG_SP, str_space + 8); // SD (store length)

	// Call VCREATE
	// a0 = pointer to destination Variant (stack_offset + total_space)
	int adjusted_dst_offset = stack_offset + total_space;
	emit_add_offset(REG_A0, REG_SP, adjusted_dst_offset);

	// a1 = Variant::STRING
	emit_li(REG_A1, Variant::STRING);

	// a2 = method (1 for const char* + size_t)
	emit_li(REG_A2, 1);

	// a3 = pointer to struct (sp + str_space)
	emit_add_offset(REG_A3, REG_SP, str_space);

	// a7 = ECALL_VCREATE (517)
	emit_li(REG_A7, 517);
	emit_ecall();

	// Restore stack pointer
	emit_add_offset(REG_SP, REG_SP, total_space);
}

void RISCVCodeGen::emit_vcreate_syscall(int variant_type, int method, uint8_t data_ptr_reg, int result_offset) {
	// Generic VCREATE syscall emission
	// VCREATE signature: void vcreate(Variant* dst, Variant::Type type, int method, void* data)
	// This handles register clobbering for a0-a3

	// Handle register clobbering (VCREATE uses a0-a3)
	std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2, REG_A3};
	auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_fn.current_instr_idx);
	for (const auto& move : moves) {
		emit_mv(move.second, move.first);
	}

	// a0 = pointer to destination Variant (result_offset from SP)
	emit_add_offset(REG_A0, REG_SP, result_offset);

	// a1 = Variant::Type
	emit_li(REG_A1, variant_type);

	// a2 = method
	emit_li(REG_A2, method);

	// a3 = data pointer (already in data_ptr_reg, or REG_ZERO for null)
	if (data_ptr_reg != REG_A3) {
		emit_mv(REG_A3, data_ptr_reg);
	}

	// a7 = ECALL_VCREATE (517)
	emit_li(REG_A7, 517);
	emit_ecall();
}

void RISCVCodeGen::emit_variant_create_empty_array(int stack_offset) {
	// Create empty Array using VCREATE syscall
	// sys_vcreate(&v, ARRAY, 0, nullptr)
	emit_vcreate_syscall(Variant::ARRAY, 0, REG_ZERO, stack_offset);
}

void RISCVCodeGen::emit_variant_create_empty_dictionary(int stack_offset) {
	// Create empty Dictionary using VCREATE syscall
	// sys_vcreate(&v, DICTIONARY, 0, nullptr)
	emit_vcreate_syscall(Variant::DICTIONARY, 0, REG_ZERO, stack_offset);
}

void RISCVCodeGen::emit_variant_move(uint8_t dst_base, int32_t dst_offset, uint8_t src_base, int32_t src_offset, uint8_t tmp_reg) {
	// A Variant is 3 doublewords with float real_t, 5 with double real_t
	for (int i = 0; i < m_layout.variant_words(); i++) {
		emit_ld(tmp_reg, src_base, src_offset + i * 8);
		emit_sd(tmp_reg, dst_base, dst_offset + i * 8);
	}
}

void RISCVCodeGen::emit_variant_eval_unary(int result_offset, int operand_offset, int op) {
	// Godot registers its unary operators (OP_NOT, OP_NEGATE, OP_POSITIVE,
	// OP_BIT_NEGATE) with NIL as the right-hand type, and Variant::evaluate()
	// indexes operator_evaluator_table[op][a_type][b_type] directly. Passing the
	// operand as both sides - the obvious reading of "a unary op has no second
	// operand" - lands on an unregistered [op][T][T] entry, so evaluate() reports
	// the operation invalid and returns NIL. Every `not x` then came out false.
	const int nil_offset = get_scratch_variant_offset(1);
	emit_li(REG_T0, Variant::NIL);
	emit_store_variant_type(REG_T0, REG_SP, nil_offset);
	emit_variant_eval(result_offset, operand_offset, nil_offset, op);
}

void RISCVCodeGen::emit_variant_eval(int result_offset, int lhs_offset, int rhs_offset, int op) {
	// Call sys_veval(op, &lhs, &rhs, &result)
	// Signature: bool sys_veval(int op, const Variant* a, const Variant* b, Variant* result)

	// VEVAL clobbers a0-a3, so handle register clobbering first
	std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2, REG_A3};
	auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_fn.current_instr_idx);

	// Emit moves to save live values from clobbered registers
	for (const auto& move : moves) {
		emit_mv(move.second, move.first); // Move from clobbered reg to new reg
	}

	// Load operator into a0
	emit_li(REG_A0, op);

	// Load address of lhs Variant into a1: addi a1, sp, lhs_offset
	emit_add_offset(REG_A1, REG_SP, lhs_offset);

	// Load address of rhs Variant into a2: addi a2, sp, rhs_offset
	emit_add_offset(REG_A2, REG_SP, rhs_offset);

	// Load address of result Variant into a3: addi a3, sp, result_offset
	emit_add_offset(REG_A3, REG_SP, result_offset);

	// Make the ecall to sys_veval (ECALL_VEVAL = 502)
	emit_li(REG_A7, 502); // ECALL_VEVAL
	emit_ecall();
}

void RISCVCodeGen::emit_typed_int_binary_op(int result_offset, int lhs_offset, int rhs_offset, IROpcode op) {
	// Optimized path for type-hinted integer arithmetic
	// Instead of calling VEVAL syscall, we:
	// 1. Load int64 values from Variants directly (from v.i field at offset +8)
	// 2. Perform native RISC-V arithmetic in registers
	// 3. Store result back as an INT Variant
	//
	// This is safe because:
	// - Type hints are verified by the parser
	// - Variant int64 field is well-defined at VARIANT_DATA_OFFSET
	// - We maintain Variant structure for ABI compatibility

	// Load lhs int64 value: ld t0, lhs_offset+8(sp)
	emit_load_variant_int(REG_T0, REG_SP, lhs_offset);

	// Load rhs int64 value: ld t1, rhs_offset+8(sp)
	emit_load_variant_int(REG_T1, REG_SP, rhs_offset);

	// Perform the operation in REG_T2
	switch (op) {
		case IROpcode::ADD:
			emit_add(REG_T2, REG_T0, REG_T1);
			break;
		case IROpcode::SUB:
			emit_sub(REG_T2, REG_T0, REG_T1);
			break;
		case IROpcode::MUL:
			emit_mul(REG_T2, REG_T0, REG_T1);
			break;
		case IROpcode::DIV:
			// Note: RISC-V div by zero produces -1 (all bits set) for positive dividend
			// This matches Variant behavior which should be checked at runtime
			emit_div(REG_T2, REG_T0, REG_T1);
			break;
		case IROpcode::MOD:
			emit_rem(REG_T2, REG_T0, REG_T1);
			break;
		case IROpcode::BIT_AND:
			emit_and(REG_T2, REG_T0, REG_T1);
			break;
		case IROpcode::BIT_OR:
			emit_or(REG_T2, REG_T0, REG_T1);
			break;
		case IROpcode::BIT_XOR:
			emit_xor(REG_T2, REG_T0, REG_T1);
			break;
		case IROpcode::SHL:
			// SLL/SRA use only the low 6 bits of the shift amount on RV64,
			// which matches Godot's Variant shift behaviour
			emit_sll(REG_T2, REG_T0, REG_T1);
			break;
		case IROpcode::SHR:
			// GDScript's >> on integers is an arithmetic shift
			emit_sra(REG_T2, REG_T0, REG_T1);
			break;
		default:
			throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Unsupported typed int binary op");
	}

	// Store result as INT Variant
	// Set type to INT (2)
	emit_li(REG_T0, Variant::INT);
	emit_store_variant_type(REG_T0, REG_SP, result_offset);

	// Store computed value to v.i field
	emit_store_variant_int(REG_T2, REG_SP, result_offset);
}

void RISCVCodeGen::emit_typed_int_comparison(int result_offset, int lhs_offset, int rhs_offset, IROpcode cmp_op) {
	// Optimized path for type-hinted integer comparisons
	// Common in loops: for i: int in range(N)
	//
	// Process:
	// 1. Load int64 values from Variants
	// 2. Perform RISC-V comparison
	// 3. Store result as BOOL Variant (0 or 1)

	// Load lhs and rhs int64 values
	emit_load_variant_int(REG_T0, REG_SP, lhs_offset);
	emit_load_variant_int(REG_T1, REG_SP, rhs_offset);

	// Perform comparison and set REG_T2 to 0 or 1
	switch (cmp_op) {
		case IROpcode::CMP_EQ:
			// xor t2, t0, t1; seqz t2, t2  (set if equal to zero)
			emit_xor(REG_T2, REG_T0, REG_T1);
			emit_seqz(REG_T2, REG_T2);
			break;

		case IROpcode::CMP_NEQ:
			// xor t2, t0, t1; snez t2, t2  (set if not equal to zero)
			emit_xor(REG_T2, REG_T0, REG_T1);
			emit_snez(REG_T2, REG_T2);
			break;

		case IROpcode::CMP_LT:
			// slt t2, t0, t1  (set if t0 < t1, signed)
			emit_slt(REG_T2, REG_T0, REG_T1);
			break;

		case IROpcode::CMP_LTE:
			// t0 <= t1  is equivalent to  !(t1 < t0)
			// slt t2, t1, t0; xori t2, t2, 1
			emit_slt(REG_T2, REG_T1, REG_T0);
			emit_xori(REG_T2, REG_T2, 1);
			break;

		case IROpcode::CMP_GT:
			// t0 > t1  is equivalent to  t1 < t0
			emit_slt(REG_T2, REG_T1, REG_T0);
			break;

		case IROpcode::CMP_GTE:
			// t0 >= t1  is equivalent to  !(t0 < t1)
			// slt t2, t0, t1; xori t2, t2, 1
			emit_slt(REG_T2, REG_T0, REG_T1);
			emit_xori(REG_T2, REG_T2, 1);
			break;

		default:
			throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Unsupported typed int comparison");
	}

	// Store result as BOOL Variant
	emit_li(REG_T0, Variant::BOOL);
	emit_store_variant_type(REG_T0, REG_SP, result_offset);
	emit_store_variant_bool(REG_T2, REG_SP, result_offset);
}

void RISCVCodeGen::emit_typed_float_binary_op(int result_offset, int lhs_offset, int rhs_offset, IROpcode op) {
	// Optimized path for type-hinted float (double) arithmetic
	// IMPORTANT: Variant v.f is ALWAYS 64-bit double, not real_t
	//
	// Process:
	// 1. Load double values from Variants (at offset +8)
	// 2. Perform native RISC-V double-precision FP arithmetic
	// 3. Store result back as a FLOAT Variant
	//
	// This avoids VEVAL syscall overhead for typed float operations

	// Load lhs double value: fld fa0, lhs_offset+8(sp)
	emit_fld(REG_FA0, REG_SP, lhs_offset + VARIANT_DATA_OFFSET);

	// Load rhs double value: fld fa1, rhs_offset+8(sp)
	emit_fld(REG_FA1, REG_SP, rhs_offset + VARIANT_DATA_OFFSET);

	// Perform the double-precision FP operation in REG_FA2
	switch (op) {
		case IROpcode::ADD:
			emit_fadd_d(REG_FA2, REG_FA0, REG_FA1);
			break;
		case IROpcode::SUB:
			emit_fsub_d(REG_FA2, REG_FA0, REG_FA1);
			break;
		case IROpcode::MUL:
			emit_fmul_d(REG_FA2, REG_FA0, REG_FA1);
			break;
		case IROpcode::DIV:
			emit_fdiv_d(REG_FA2, REG_FA0, REG_FA1);
			break;
		default:
			throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Unsupported typed float binary op");
	}

	// Store result as FLOAT Variant
	// Set type to FLOAT (3)
	emit_li(REG_T0, Variant::FLOAT);
	emit_store_variant_type(REG_T0, REG_SP, result_offset);

	// Store computed double value to v.f field
	emit_fsd(REG_FA2, REG_SP, result_offset + VARIANT_DATA_OFFSET);
}

void RISCVCodeGen::emit_typed_vector_binary_op(int result_offset, int lhs_offset, int rhs_offset, IROpcode op, IRInstruction::TypeHint type_hint) {
	// Optimized path for type-hinted vector arithmetic
	// Vectors are stored inline in Variant's data union:
	// - VECTOR2/3/4: real_t v4[4] (32-bit float by default, 64-bit double in
	//   double-precision builds)
	// - VECTOR2I/3I/4I: int32_t v4i[4] (always 32-bit)
	//
	// Process:
	// 1. Determine element count (2, 3, or 4) and type (int32 vs real_t)
	// 2. Loop over components, performing element-wise arithmetic
	// 3. Store result back with proper Variant type
	//
	// This is game math - Vector3 * Vector3 means component-wise multiplication

	// Determine vector properties
	int elem_count = 0;
	bool is_int = false;

	switch (type_hint) {
		case Variant::VECTOR2:
			elem_count = 2;
			is_int = false;
			break;
		case Variant::VECTOR2I:
			elem_count = 2;
			is_int = true;
			break;
		case Variant::VECTOR3:
			elem_count = 3;
			is_int = false;
			break;
		case Variant::VECTOR3I:
			elem_count = 3;
			is_int = true;
			break;
		case Variant::VECTOR4:
		case Variant::COLOR:  // Color is also 4 real_t components
			elem_count = 4;
			is_int = false;
			break;
		case Variant::VECTOR4I:
			elem_count = 4;
			is_int = true;
			break;
		default:
			throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Invalid vector type hint");
	}

	// Perform element-wise operations
	for (int i = 0; i < elem_count; i++) {
		int component_offset = is_int ? int_offset(i) : real_offset(i);

		if (is_int) {
			// Integer vector: use lw/sw and integer ALU
			emit_lw(REG_T0, REG_SP, lhs_offset + component_offset);
			emit_lw(REG_T1, REG_SP, rhs_offset + component_offset);

			// Perform integer operation
			switch (op) {
				case IROpcode::ADD:
					emit_add(REG_T2, REG_T0, REG_T1);
					break;
				case IROpcode::SUB:
					emit_sub(REG_T2, REG_T0, REG_T1);
					break;
				case IROpcode::MUL:
					emit_mul(REG_T2, REG_T0, REG_T1);
					break;
				case IROpcode::DIV:
					emit_div(REG_T2, REG_T0, REG_T1);
					break;
				case IROpcode::MOD:
					emit_rem(REG_T2, REG_T0, REG_T1);
					break;
				default:
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Unsupported vector int operation");
			}

			// Store result component
			emit_sw(REG_T2, REG_SP, result_offset + component_offset);
		} else {
			// Float vector: load/store and compute at real_t width
			emit_flr(REG_FA0, REG_SP, lhs_offset + component_offset);
			emit_flr(REG_FA1, REG_SP, rhs_offset + component_offset);

			// Perform the real_t-width FP operation
			switch (op) {
				case IROpcode::ADD:
					emit_fadd_r(REG_FA2, REG_FA0, REG_FA1);
					break;
				case IROpcode::SUB:
					emit_fsub_r(REG_FA2, REG_FA0, REG_FA1);
					break;
				case IROpcode::MUL:
					emit_fmul_r(REG_FA2, REG_FA0, REG_FA1);
					break;
				case IROpcode::DIV:
					emit_fdiv_r(REG_FA2, REG_FA0, REG_FA1);
					break;
				default:
					throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Unsupported vector float operation");
			}

			// Store result component
			emit_fsr(REG_FA2, REG_SP, result_offset + component_offset);
		}
	}

	// Set result Variant type
	emit_li(REG_T0, type_hint);
	emit_store_variant_type(REG_T0, REG_SP, result_offset);
}

// Variant field access helpers
void RISCVCodeGen::emit_load_variant_type(uint8_t rd, uint8_t base_reg, int32_t variant_offset) {
	// Load m_type field (4 bytes at variant_offset + 0)
	emit_lw(rd, base_reg, variant_offset + VARIANT_TYPE_OFFSET);
}

void RISCVCodeGen::emit_store_variant_type(uint8_t rs, uint8_t base_reg, int32_t variant_offset) {
	// Store m_type field (4 bytes at variant_offset + 0)
	emit_sw(rs, base_reg, variant_offset + VARIANT_TYPE_OFFSET);
}

void RISCVCodeGen::emit_variant_truthy(uint8_t rd, int variant_offset, int32_t type_hint) {
	// Compute Godot's Variant::booleanize() for the Variant at variant_offset(sp)
	// and leave 0 or 1 in rd.
	//
	// This used to be a single `lbu` of the payload's low byte, which is only
	// correct for BOOL: it makes the integer 256 false, and makes any scoped
	// index whose low byte happens to be zero false as well.
	switch (type_hint) {
		case Variant::NIL:
			emit_li(rd, 0);
			return;
		case Variant::BOOL:
			emit_lbu(rd, REG_SP, variant_offset + VARIANT_DATA_OFFSET);
			return;
		case Variant::INT:
			emit_ld(rd, REG_SP, variant_offset + VARIANT_DATA_OFFSET);
			emit_snez(rd, rd);
			return;
		case Variant::FLOAT:
			// Variant::FLOAT is always a 64-bit double. Shifting the sign bit out
			// makes -0.0 compare equal to +0.0 and leaves every NaN non-zero,
			// which is what comparing against 0.0 would do.
			emit_ld(rd, REG_SP, variant_offset + VARIANT_DATA_OFFSET);
			emit_i_type(0x13, rd, 1, rd, 1); // slli rd, rd, 1
			emit_snez(rd, rd);
			return;
		default:
			break;
	}

	// The type is not known at compile time. Strings, arrays, dictionaries and
	// packed arrays booleanize by emptiness and objects by validity, none of
	// which the guest can see, so the host decides. OP_NOT yields
	// !booleanize(x); inverting it gives the truth value.
	//
	// The call is emitted unconditionally rather than behind a type dispatch:
	// emit_variant_eval() asks the register allocator to move live values out of
	// the syscall's clobbered registers, and those moves must not sit on a path
	// that is sometimes skipped.
	const int scratch_offset = get_scratch_variant_offset();
	emit_variant_eval_unary(scratch_offset, variant_offset, 23); // OP_NOT
	emit_lbu(rd, REG_SP, scratch_offset + VARIANT_DATA_OFFSET);
	emit_seqz(rd, rd);
}

void RISCVCodeGen::emit_load_variant_bool(uint8_t rd, uint8_t base_reg, int32_t variant_offset) {
	// Load boolean value (1 byte at variant_offset + 8)
	// Use unsigned load to ensure proper zero-extension
	emit_lbu(rd, base_reg, variant_offset + VARIANT_DATA_OFFSET);
}

void RISCVCodeGen::emit_store_variant_bool(uint8_t rs, uint8_t base_reg, int32_t variant_offset) {
	// Store boolean value (1 byte at variant_offset + 8)
	// Note: storing 8 bytes is fine too since bool is in a union, but byte is more precise
	emit_sb(rs, base_reg, variant_offset + VARIANT_DATA_OFFSET);
}

void RISCVCodeGen::emit_load_variant_int(uint8_t rd, uint8_t base_reg, int32_t variant_offset) {
	// Load int64 value (8 bytes at variant_offset + 8)
	emit_ld(rd, base_reg, variant_offset + VARIANT_DATA_OFFSET);
}

void RISCVCodeGen::emit_store_variant_int(uint8_t rs, uint8_t base_reg, int32_t variant_offset) {
	// Store int64 value (8 bytes at variant_offset + 8)
	emit_sd(rs, base_reg, variant_offset + VARIANT_DATA_OFFSET);
}

// Load/Store helpers with automatic large offset handling.
//
// Every load and every store shares these two, so the decision about what fits
// in a 12-bit immediate is made once. Fourteen inline copies of it is thirteen
// chances for one to check `< 2048` and forget `>= -2048`.
void RISCVCodeGen::emit_load_with_offset(uint8_t opcode, uint8_t funct3, uint8_t rd, uint8_t rs1, int32_t offset) {
	if (fits_in_signed(offset, I_TYPE_IMM_BITS)) {
		emit_i_type(opcode, rd, funct3, rs1, offset);
		return;
	}
	// The address goes in the reserved scratch rather than in a temporary the
	// surrounding expansion may be using.
	emit_add_offset(REG_WIDE_SCRATCH, rs1, offset);
	emit_i_type(opcode, rd, funct3, REG_WIDE_SCRATCH, 0);
}

void RISCVCodeGen::emit_store_with_offset(uint8_t opcode, uint8_t funct3, uint8_t rs2, uint8_t rs1, int32_t offset) {
	if (fits_in_signed(offset, S_TYPE_IMM_BITS)) {
		emit_s_type(opcode, funct3, rs1, rs2, offset);
		return;
	}
	// rs2 is the value being stored. Computing the address into a temporary
	// that happened to be rs2 -- which is what `sd t2, 2048(sp)` did -- stores
	// the address instead of the value.
	emit_add_offset(REG_WIDE_SCRATCH, rs1, offset);
	emit_s_type(opcode, funct3, REG_WIDE_SCRATCH, rs2, 0);
}

void RISCVCodeGen::emit_ld(uint8_t rd, uint8_t rs1, int32_t offset) {
	emit_load_with_offset(0x03, 3, rd, rs1, offset);
}

void RISCVCodeGen::emit_lw(uint8_t rd, uint8_t rs1, int32_t offset) {
	emit_load_with_offset(0x03, 2, rd, rs1, offset);
}

void RISCVCodeGen::emit_lwu(uint8_t rd, uint8_t rs1, int32_t offset) {
	// LWU (Load Word Unsigned) - RV64I specific, zero-extends to 64 bits
	emit_load_with_offset(0x03, 4, rd, rs1, offset);  // funct3=4 for LWU
}

void RISCVCodeGen::emit_lh(uint8_t rd, uint8_t rs1, int32_t offset) {
	emit_load_with_offset(0x03, 1, rd, rs1, offset);
}

void RISCVCodeGen::emit_lb(uint8_t rd, uint8_t rs1, int32_t offset) {
	emit_load_with_offset(0x03, 0, rd, rs1, offset);
}

void RISCVCodeGen::emit_lbu(uint8_t rd, uint8_t rs1, int32_t offset) {
	emit_load_with_offset(0x03, 4, rd, rs1, offset);
}

void RISCVCodeGen::emit_sd(uint8_t rs2, uint8_t rs1, int32_t offset) {
	emit_store_with_offset(0x23, 3, rs2, rs1, offset);
}

void RISCVCodeGen::emit_sw(uint8_t rs2, uint8_t rs1, int32_t offset) {
	emit_store_with_offset(0x23, 2, rs2, rs1, offset);
}

void RISCVCodeGen::emit_sh(uint8_t rs2, uint8_t rs1, int32_t offset) {
	emit_store_with_offset(0x23, 1, rs2, rs1, offset);
}

void RISCVCodeGen::emit_sb(uint8_t rs2, uint8_t rs1, int32_t offset) {
	emit_store_with_offset(0x23, 0, rs2, rs1, offset);
}

// Floating-point load/store (RV64D extension)
void RISCVCodeGen::emit_fld(uint8_t rd, uint8_t rs1, int32_t offset) {
	// FLD: opcode=0x07, funct3=3
	emit_load_with_offset(0x07, 3, rd, rs1, offset);
}

void RISCVCodeGen::emit_fsd(uint8_t rs2, uint8_t rs1, int32_t offset) {
	// FSD: opcode=0x27, funct3=3
	emit_store_with_offset(0x27, 3, rs2, rs1, offset);
}

void RISCVCodeGen::emit_flw(uint8_t rd, uint8_t rs1, int32_t offset) {
	// FLW: opcode=0x07, funct3=2 (RV32F/RV64F)
	emit_load_with_offset(0x07, 2, rd, rs1, offset);
}

void RISCVCodeGen::emit_fsw(uint8_t rs2, uint8_t rs1, int32_t offset) {
	// FSW: opcode=0x27, funct3=2 (RV32F/RV64F)
	emit_store_with_offset(0x27, 2, rs2, rs1, offset);
}

void RISCVCodeGen::emit_fcvt_d_s(uint8_t rd, uint8_t rs1) {
	// FCVT.D.S: Convert float (32-bit) to double (64-bit)
	emit_r4_type(0b1010011, rd, 0, rs1, 0, 0b01000, 1);
}

void RISCVCodeGen::emit_fcvt_s_d(uint8_t rd, uint8_t rs1) {
	// FCVT.S.D: Convert double (64-bit) to float (32-bit)
	emit_r4_type(0b1010011, rd, 0, rs1, 1, 0b01000, 0);
}

void RISCVCodeGen::emit_fcvt_d_l(uint8_t rd, uint8_t rs1) {
	// FCVT.D.L: Convert signed 64-bit integer to double
	emit_r4_type(0b1010011, rd, 0, rs1, 2, 0b11010, 1);
}

void RISCVCodeGen::emit_fadd_d(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	// FADD.D: Double-precision FP add
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x01);
}

void RISCVCodeGen::emit_fsub_d(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	// FSUB.D: Double-precision FP subtract
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x05);
}

void RISCVCodeGen::emit_fmul_d(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	// FMUL.D: Double-precision FP multiply
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x09);
}

void RISCVCodeGen::emit_fdiv_d(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	// FDIV.D: Double-precision FP divide
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x0D);
}

void RISCVCodeGen::emit_fmv_d(uint8_t rd, uint8_t rs) {
	// FMV.D: Double-precision FP move
	// FSGNJ.D with rs1=rs2 implements move
	emit_r4_type(0x53, rd, 0x0, rs, rs, 0b00100, 1);
}

void RISCVCodeGen::emit_fsqrt_d(uint8_t rd, uint8_t rs1) {
	// FSQRT.D: funct7=0b0101101, rs2 is unused and must be zero
	emit_r_type(0x53, rd, 0, rs1, 0, 0b0101101);
}

void RISCVCodeGen::emit_fabs_d(uint8_t rd, uint8_t rs1) {
	// FABS.D is FSGNJX.D rd, rs, rs: the sign becomes rs's sign XOR rs's sign,
	// which is positive, and the rest of the value is untouched. Exact, and
	// defined for every input including the NaNs.
	emit_r_type(0x53, rd, 0b010, rs1, rs1, 0b0010001);
}

void RISCVCodeGen::emit_flt_d(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	// FLT.D: 1 or 0 into an *integer* register. Quiet on ordered operands and
	// false whenever either is a NaN, which is what C++ `a < b` does.
	emit_r_type(0x53, rd, 0b001, rs1, rs2, 0b1010001);
}

void RISCVCodeGen::emit_feq_d(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	// FEQ.D: 1 or 0 into an *integer* register. Quiet, and false whenever
	// either operand is a NaN, which is what C++ `a == b` does.
	emit_r_type(0x53, rd, 0b010, rs1, rs2, 0b1010001);
}

void RISCVCodeGen::emit_fcvt_l_d(uint8_t rd, uint8_t rs1) {
	// FCVT.L.D, rounding toward zero (rm=001): the C++ (int64_t) cast.
	emit_r_type(0x53, rd, 0b001, rs1, 0b00010, 0b1100001);
}

void RISCVCodeGen::emit_fadd_s(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	// FADD.S: Single-precision FP add
	// funct7=0x00 for single-precision (vs 0x01 for double)
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x00);
}

void RISCVCodeGen::emit_fsub_s(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	// FSUB.S: Single-precision FP subtract
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x04);
}

void RISCVCodeGen::emit_fmul_s(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	// FMUL.S: Single-precision FP multiply
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x08);
}

void RISCVCodeGen::emit_fdiv_s(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	// FDIV.S: Single-precision FP divide
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x0C);
}

void RISCVCodeGen::emit_fmv_s(uint8_t rd, uint8_t rs) {
	// FMV.S: Single-precision FP move
	// FSGNJ.S with rs1=rs2 implements move
	emit_r4_type(0x53, rd, 0x0, rs, rs, 0b00100, 0);
}

// -= real_t-width floating point =-
// One set of call sites, two instruction encodings. Everything that touches a
// real_t component of a Variant (Vector2/3/4, Rect2, Plane, Quaternion, Color)
// goes through these instead of picking flw/fld or fadd.s/fadd.d directly.

void RISCVCodeGen::emit_flr(uint8_t rd, uint8_t rs1, int32_t offset) {
	if (m_layout.double_precision) {
		emit_fld(rd, rs1, offset);
	} else {
		emit_flw(rd, rs1, offset);
	}
}

void RISCVCodeGen::emit_fsr(uint8_t rs2, uint8_t rs1, int32_t offset) {
	if (m_layout.double_precision) {
		emit_fsd(rs2, rs1, offset);
	} else {
		emit_fsw(rs2, rs1, offset);
	}
}

void RISCVCodeGen::emit_fcvt_d_r(uint8_t rd, uint8_t rs1) {
	// Widen real_t to the double that Variant::FLOAT always holds. When real_t
	// already is a double this is at most a register move.
	if (m_layout.double_precision) {
		if (rd != rs1) {
			emit_fmv_d(rd, rs1);
		}
	} else {
		emit_fcvt_d_s(rd, rs1);
	}
}

void RISCVCodeGen::emit_fcvt_r_d(uint8_t rd, uint8_t rs1) {
	// Narrow a double down to real_t. No-op when real_t is a double.
	if (m_layout.double_precision) {
		if (rd != rs1) {
			emit_fmv_d(rd, rs1);
		}
	} else {
		emit_fcvt_s_d(rd, rs1);
	}
}

void RISCVCodeGen::emit_fadd_r(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	if (m_layout.double_precision) {
		emit_fadd_d(rd, rs1, rs2);
	} else {
		emit_fadd_s(rd, rs1, rs2);
	}
}

void RISCVCodeGen::emit_fsub_r(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	if (m_layout.double_precision) {
		emit_fsub_d(rd, rs1, rs2);
	} else {
		emit_fsub_s(rd, rs1, rs2);
	}
}

void RISCVCodeGen::emit_fmul_r(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	if (m_layout.double_precision) {
		emit_fmul_d(rd, rs1, rs2);
	} else {
		emit_fmul_s(rd, rs1, rs2);
	}
}

void RISCVCodeGen::emit_fdiv_r(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	if (m_layout.double_precision) {
		emit_fdiv_d(rd, rs1, rs2);
	} else {
		emit_fdiv_s(rd, rs1, rs2);
	}
}

// Sign-extend word to doubleword (addiw rd, rs, 0)
void RISCVCodeGen::emit_srai(uint8_t rd, uint8_t rs, uint8_t shamt) {
	// SRAI on RV64: funct6=010000 and a six-bit shift amount, so the immediate
	// field is (0b010000 << 6) | shamt.
	emit_i_type(0x13, rd, 5, rs, (0b010000 << 6) | (shamt & 0x3F));
}

void RISCVCodeGen::emit_sext_w(uint8_t rd, uint8_t rs) {
	// ADDIW: opcode=0x1b, funct3=0
	emit_i_type(0x1b, rd, 0, rs, 0);
}

void RISCVCodeGen::emit_syscall_result(int result_vreg, uint8_t result_reg, int result_offset, int variant_type) {
	// Always store to stack as Variant
	emit_li(REG_T0, variant_type);
	emit_sw(REG_T0, REG_SP, result_offset); // Store 4-byte type
	emit_sd(result_reg, REG_SP, result_offset + 8); // Store 8-byte integer at offset 8
}

void RISCVCodeGen::emit_stack_adjust(int32_t amount) {
	emit_add_offset(REG_SP, REG_SP, amount);
}

void RISCVCodeGen::emit_load_stack_offset(uint8_t rd, int32_t offset) {
	emit_add_offset(rd, REG_SP, offset);
}

bool RISCVCodeGen::is_complex_variant_type(int variant_type) {
	// Simple/inline types that DON'T need permanent storage:
	// NIL, BOOL, INT, FLOAT, Vector2/3/4, Vector2/3/4i, Color, Rect2/2i, Plane
	// Everything else (String, Array, Dictionary, Object, packed arrays, etc.) needs permanent storage
	switch (variant_type) {
		case Variant::NIL:
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:
		case Variant::VECTOR2:
		case Variant::VECTOR2I:
		case Variant::VECTOR3:
		case Variant::VECTOR3I:
		case Variant::VECTOR4:
		case Variant::VECTOR4I:
		case Variant::RECT2:
		case Variant::RECT2I:
		case Variant::PLANE:
		case Variant::COLOR:
			return false; // These are simple inline types
		default:
			return true; // Everything else is complex and needs permanent storage
	}
}

} // namespace gdscript
