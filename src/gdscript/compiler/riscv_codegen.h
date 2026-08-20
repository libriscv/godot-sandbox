#pragma once
#include "globals.h"
#include "ir.h"
#include "register_allocator.h"
#include "variant_layout.h"
#include <string>
#include <unordered_set>
#include <cstdint>

namespace gdscript {

// RISC-V instruction encoder
class RISCVCodeGen {
public:
	// The layout decides how wide real_t is, and therefore how big a Variant is.
	// Defaults to the layout this compiler was built for.
	explicit RISCVCodeGen(const VariantLayout& layout = native_variant_layout());

	// Generate RISC-V machine code from IR
	std::vector<uint8_t> generate(const IRProgram& program);

	// The Variant layout this code generator emits for
	const VariantLayout& get_layout() const { return m_layout; }

	// Get function offsets (name -> offset in code)
	const std::unordered_map<std::string, size_t>& get_function_offsets() const { return m_functions; }

	// Get register allocator (for testing)
	const RegisterAllocator& get_allocator() const { return m_allocator; }

	// Get constant pool (for ELF builder to create .rodata section)
	const std::vector<int64_t>& get_constant_pool() const { return m_constant_pool; }

	// Get global variables information
	const std::vector<IRGlobalVar>& get_globals() const { return m_globals; }
	size_t get_global_data_size() const { return m_global_data_size; }

	// -= Immediate ranges =-
	//
	// Every instruction immediate is a fixed-width signed field. Masking a value
	// that does not fit -- which these encoders used to do -- silently emits a
	// different instruction: `addi rd, rs, 4776` became `addi rd, rs, 680`, and
	// three call sites computing global addresses that way produced wrong code
	// once a program had enough globals. An immediate out of range is a compiler
	// bug, so the encoders say so instead of encoding something else.
	static constexpr int I_TYPE_IMM_BITS = 12;   // addi, loads, jalr
	static constexpr int S_TYPE_IMM_BITS = 12;   // stores
	static constexpr int B_TYPE_IMM_BITS = 13;   // conditional branches, always even
	static constexpr int J_TYPE_IMM_BITS = 21;   // jal, always even

	// Whether `value` fits in a `bits`-wide two's complement field.
	static bool fits_in_signed(int64_t value, int bits);
	// Throws unless it does, naming the instruction and the value.
	static void check_immediate(const std::string& what, int64_t value, int bits);
	// The same, for a branch or jump displacement, which is also always even.
	static void check_displacement(const std::string& what, int64_t offset, int bits);

	// The register a load or a store computes a wide address in.
	//
	// A stack frame larger than 2047 bytes cannot reach its upper slots with a
	// 12-bit immediate, so those accesses have to materialize the address in a
	// register first. Doing that in t0-t2, which is what every inline copy of
	// the wide path used to do, silently destroys whatever the surrounding
	// instruction expansion had there -- including, for `sd t2, 2048(sp)`, the
	// value being stored, which then wrote a stack address into the Variant.
	//
	// This register is reserved: RegisterAllocator does not hand it out and no
	// expansion uses it, so computing an address in it is always safe.
	static constexpr uint8_t REG_WIDE_SCRATCH = 31; // x31 (t6)

	// Label of the synthetic function that evaluates non-constant global
	// initializers. Not a user-visible function name, so it cannot collide with
	// one: GDScript identifiers cannot contain a '.'.
	static constexpr const char* GLOBAL_INIT_LABEL = ".init_globals";

private:
	struct Function {
		std::string name;
		size_t offset; // Offset in code section
	};

	// Generate code for a single function
	void gen_function(const IRFunction& func);

	// Instruction emission
	void emit_word(uint32_t word);  // Emit raw 32-bit word
	void emit_r_type(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, uint8_t rs2, uint8_t funct7);
	void emit_i_type(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, int32_t imm);
	void emit_s_type(uint8_t opcode, uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm);
	void emit_b_type(uint8_t opcode, uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm);
	void emit_u_type(uint8_t opcode, uint8_t rd, uint32_t imm);
	void emit_j_type(uint8_t opcode, uint8_t rd, int32_t imm);
	void emit_r4_type(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, uint8_t rs2, uint8_t rs3, uint8_t funct2);

	// Higher-level RISC-V instructions
	void emit_li(uint8_t rd, int64_t imm);      // Load immediate
	void emit_la(uint8_t rd, const std::string& label, int32_t addend = 0); // Load address (pseudo: auipc + addi)
	void emit_mv(uint8_t rd, uint8_t rs);       // Move
	void emit_addi(uint8_t rd, uint8_t rs1, int32_t imm); // Add immediate

	// rd = base + offset, whatever the offset is: `addi` when it fits in the
	// 12-bit immediate, `li` + `add` when it does not. Every place that used to
	// write that branch inline checked `offset < 2048` without checking
	// `offset >= -2048`, which is half a range check.
	//
	// When rd and base are the same register the offset needs somewhere to
	// live, and that somewhere is REG_WIDE_SCRATCH, which nothing else uses.
	void emit_add_offset(uint8_t rd, uint8_t base, int32_t offset);

	// Address of global variable `index`, i.e. ".globals + index * stride".
	// The index is folded into the relocation rather than added afterwards,
	// because an `addi` after the address truncates at 85 globals.
	void emit_address_of_global(uint8_t rd, size_t index);

	// The label the global data area is defined at.
	static constexpr const char* GLOBALS_LABEL = ".globals";
	void emit_add(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_sub(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_mul(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_div(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_rem(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_and(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_or(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_xor(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_xori(uint8_t rd, uint8_t rs, int32_t imm);  // XOR immediate
	void emit_sll(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_srl(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_sra(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_slt(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_seqz(uint8_t rd, uint8_t rs);   // Set if equal to zero (pseudo: sltiu rd, rs, 1)
	void emit_snez(uint8_t rd, uint8_t rs);   // Set if not equal to zero (pseudo: sltu rd, x0, rs)
	void emit_beq(uint8_t rs1, uint8_t rs2, int32_t offset);
	void emit_bne(uint8_t rs1, uint8_t rs2, int32_t offset);
	void emit_blt(uint8_t rs1, uint8_t rs2, int32_t offset);
	void emit_bge(uint8_t rs1, uint8_t rs2, int32_t offset);
	void emit_bltu(uint8_t rs1, uint8_t rs2, int32_t offset);
	void emit_bgeu(uint8_t rs1, uint8_t rs2, int32_t offset);
	void emit_jal(uint8_t rd, int32_t offset);
	void emit_jalr(uint8_t rd, uint8_t rs1, int32_t offset);
	void emit_ecall();
	void emit_ret();

	// One range decision for every load and every store: the 12-bit immediate
	// when the offset fits, a computed base register when it does not.
	void emit_load_with_offset(uint8_t opcode, uint8_t funct3, uint8_t rd, uint8_t rs1, int32_t offset);
	void emit_store_with_offset(uint8_t opcode, uint8_t funct3, uint8_t rs2, uint8_t rs1, int32_t offset);

	// Load/Store instructions (with automatic large offset handling)
	void emit_ld(uint8_t rd, uint8_t rs1, int32_t offset);   // Load doubleword (64-bit)
	void emit_lw(uint8_t rd, uint8_t rs1, int32_t offset);   // Load word (32-bit)
	void emit_lwu(uint8_t rd, uint8_t rs1, int32_t offset);  // Load word unsigned (32-bit, zero-extended to 64-bit)
	void emit_lh(uint8_t rd, uint8_t rs1, int32_t offset);   // Load halfword (16-bit)
	void emit_lb(uint8_t rd, uint8_t rs1, int32_t offset);   // Load byte (8-bit signed)
	void emit_lbu(uint8_t rd, uint8_t rs1, int32_t offset);  // Load byte unsigned (8-bit)
	void emit_sd(uint8_t rs2, uint8_t rs1, int32_t offset);  // Store doubleword (64-bit)
	void emit_sw(uint8_t rs2, uint8_t rs1, int32_t offset);  // Store word (32-bit)
	void emit_sh(uint8_t rs2, uint8_t rs1, int32_t offset);  // Store halfword (16-bit)
	void emit_sb(uint8_t rs2, uint8_t rs1, int32_t offset);  // Store byte (8-bit)

	// Floating-point load/store (RV64D extension)
	void emit_fld(uint8_t rd, uint8_t rs1, int32_t offset);  // Load double (64-bit FP)
	void emit_fsd(uint8_t rs2, uint8_t rs1, int32_t offset); // Store double (64-bit FP)
	void emit_flw(uint8_t rd, uint8_t rs1, int32_t offset);  // Load float (32-bit FP)
	void emit_fsw(uint8_t rs2, uint8_t rs1, int32_t offset); // Store float (32-bit FP)
	void emit_fcvt_d_s(uint8_t rd, uint8_t rs1);             // Convert float to double
	void emit_fcvt_s_d(uint8_t rd, uint8_t rs1);             // Convert double to float
	void emit_fcvt_d_l(uint8_t rd, uint8_t rs1);             // Convert signed 64-bit int to double
	void emit_fmv_s(uint8_t rd, uint8_t rs);                 // Single-precision FP move

	// real_t-width floating point. These pick the 32-bit or the 64-bit instruction
	// depending on the target's real_t, so the vector code paths stay single-sourced.
	void emit_flr(uint8_t rd, uint8_t rs1, int32_t offset);  // Load real_t (flw / fld)
	void emit_fsr(uint8_t rs2, uint8_t rs1, int32_t offset); // Store real_t (fsw / fsd)
	void emit_fcvt_d_r(uint8_t rd, uint8_t rs1);             // real_t -> double (widen, or move)
	void emit_fcvt_r_d(uint8_t rd, uint8_t rs1);             // double -> real_t (narrow, or move)
	void emit_fadd_r(uint8_t rd, uint8_t rs1, uint8_t rs2);  // real_t FP add
	void emit_fsub_r(uint8_t rd, uint8_t rs1, uint8_t rs2);  // real_t FP sub
	void emit_fmul_r(uint8_t rd, uint8_t rs1, uint8_t rs2);  // real_t FP mul
	void emit_fdiv_r(uint8_t rd, uint8_t rs1, uint8_t rs2);  // real_t FP div

	// FP arithmetic instructions (RV64D extension - double precision)
	void emit_fadd_d(uint8_t rd, uint8_t rs1, uint8_t rs2);  // Double-precision FP add
	void emit_fsub_d(uint8_t rd, uint8_t rs1, uint8_t rs2);  // Double-precision FP sub
	void emit_fmul_d(uint8_t rd, uint8_t rs1, uint8_t rs2);  // Double-precision FP mul
	void emit_fdiv_d(uint8_t rd, uint8_t rs1, uint8_t rs2);  // Double-precision FP div
	void emit_fmv_d(uint8_t rd, uint8_t rs);                // Double-precision FP move
	void emit_fsqrt_d(uint8_t rd, uint8_t rs1);              // Double-precision square root
	void emit_fabs_d(uint8_t rd, uint8_t rs1);               // |x| (fsgnjx.d rd, rs, rs)
	void emit_flt_d(uint8_t rd, uint8_t rs1, uint8_t rs2);   // rd = (rs1 < rs2), into an integer register
	void emit_fcvt_l_d(uint8_t rd, uint8_t rs1);             // double -> signed 64-bit int, truncating

	// FP arithmetic instructions (RV32F extension - single precision)
	void emit_fadd_s(uint8_t rd, uint8_t rs1, uint8_t rs2);  // Single-precision FP add
	void emit_fsub_s(uint8_t rd, uint8_t rs1, uint8_t rs2);  // Single-precision FP sub
	void emit_fmul_s(uint8_t rd, uint8_t rs1, uint8_t rs2);  // Single-precision FP mul
	void emit_fdiv_s(uint8_t rd, uint8_t rs1, uint8_t rs2);  // Single-precision FP div

	// Additional integer instructions
	void emit_sext_w(uint8_t rd, uint8_t rs);  // Sign-extend word to doubleword (addiw rd, rs, 0)
	void emit_srai(uint8_t rd, uint8_t rs, uint8_t shamt); // Arithmetic shift right by a constant

	// Pseudo-instructions
	void emit_call(const std::string& func_name);
	void emit_jump(const std::string& label);

	// Label management
	void define_label(const std::string& label);
	// `addend` is folded into the resolved address, which lets a single AUIPC+ADDI
	// pair reach `label + addend` for any 32-bit addend. Computing the same address
	// as `la` followed by `addi` silently truncates once the addend leaves the
	// 12-bit immediate range, which happens as soon as a program has enough globals.
	void mark_label_use(const std::string& label, size_t code_offset, int32_t addend = 0);

	// Rewrite conditional branches that cannot reach their target.
	//
	// A B-type branch reaches +-4KB, which a function with a large enough body
	// outgrows. Masking the displacement -- which is what the encoder used to
	// do -- produced a branch to somewhere else entirely; refusing to encode it
	// is correct but leaves a program that cannot be compiled at all. So a
	// branch that cannot reach is turned into the standard pair
	//
	//     b<inverted> rs1, rs2, +8
	//     jal x0, target
	//
	// which reaches +-1MB. Inserting an instruction moves everything after it,
	// so this runs to a fixpoint: growing the code can put another branch out
	// of reach.
	void relax_branches();

	void resolve_labels();

	// Variant field access helpers
	// Load/store Variant fields with proper types and offsets
	void emit_load_variant_type(uint8_t rd, uint8_t base_reg, int32_t variant_offset);
	void emit_store_variant_type(uint8_t rs, uint8_t base_reg, int32_t variant_offset);
	// Godot's Variant::booleanize() for the Variant at variant_offset(sp), into rd.
	// type_hint is the statically known Variant type, or IRInstruction::TypeHint_NONE.
	void emit_variant_truthy(uint8_t rd, int variant_offset, int32_t type_hint);

	// Variant::evaluate() for a unary operator. Uses scratch slot 1 for the NIL
	// right-hand operand Godot's operator table expects.
	void emit_variant_eval_unary(int result_offset, int operand_offset, int op);
	void emit_load_variant_bool(uint8_t rd, uint8_t base_reg, int32_t variant_offset);
	void emit_store_variant_bool(uint8_t rs, uint8_t base_reg, int32_t variant_offset);
	void emit_load_variant_int(uint8_t rd, uint8_t base_reg, int32_t variant_offset);
	void emit_store_variant_int(uint8_t rs, uint8_t base_reg, int32_t variant_offset);

	// Variant management
	void emit_variant_create_int(int stack_offset, int64_t value);
	void emit_variant_create_bool(int stack_offset, bool value);
	void emit_variant_create_string(int stack_offset, int string_idx);
	void emit_variant_copy(int dst_offset, int src_offset);

	// Copy a whole Variant (variant_size() bytes) between two [base + offset] locations,
	// shuttling it through tmp_reg. Both endpoints must be 8-byte aligned.
	void emit_variant_move(uint8_t dst_base, int32_t dst_offset, uint8_t src_base, int32_t src_offset, uint8_t tmp_reg);
	void emit_variant_eval(int result_offset, int lhs_offset, int rhs_offset, int op);

	// VCREATE syscall abstraction
	// Generic VCREATE emission that handles register clobbering and stack management
	// variant_type: Variant::Type enum value (e.g., Variant::STRING, Variant::ARRAY)
	// method: Constructor method number (type-specific)
	// data_ptr_reg: Register containing pointer to data (or REG_ZERO for nullptr)
	// result_offset: Stack offset where to create the Variant
	// Note: Caller is responsible for setting up data_ptr_reg before calling
	void emit_vcreate_syscall(int variant_type, int method, uint8_t data_ptr_reg, int result_offset);

	// High-level variant constructors using VCREATE
	// These handle all the setup including data preparation
	void emit_variant_create_empty_array(int stack_offset);
	void emit_variant_create_empty_dictionary(int stack_offset);

	// Typed operations (optimized paths when type hints are available)
	// These emit native RISC-V instructions instead of syscalls
	void emit_typed_int_binary_op(int result_offset, int lhs_offset, int rhs_offset, IROpcode op);
	void emit_typed_int_comparison(int result_offset, int lhs_offset, int rhs_offset, IROpcode cmp_op);
	void emit_typed_float_binary_op(int result_offset, int lhs_offset, int rhs_offset, IROpcode op);
	void emit_typed_vector_binary_op(int result_offset, int lhs_offset, int rhs_offset, IROpcode op, IRInstruction::TypeHint type_hint);

	// -= GDScript's global functions =-
	//
	// GLOBAL_CALL, in riscv_globals.cpp. What each global means, how many
	// arguments it takes and which of these shapes performs it is globals.h's
	// table; this is only the emission.
	void emit_global_call(const IRInstruction& instr);
	// One form -- a row of the table with a concrete kind. `typed` says the
	// arguments are already Variants of the type the form works in, which is
	// what lets the loads skip the run-time type test.
	void emit_global_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
		int result_offset, bool typed);
	void emit_global_int_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
		int result_offset, bool typed);
	void emit_global_float_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
		int result_offset, bool typed);
	void emit_global_syscall_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
		int result_offset, bool typed);
	void emit_global_host_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
		int result_offset);

	// The Variant at `variant_offset(sp)` as a double in `fd`, or as a 64-bit
	// integer in `rd`. `known` skips the run-time type test, and is only ever
	// true where the type has been established -- by a type hint, or by the
	// dispatch emit_args_all_int() performs.
	void emit_variant_to_double(uint8_t fd, int variant_offset, bool known_float);
	void emit_variant_to_int(uint8_t rd, int variant_offset, bool known_int);

	// rd = 1 when every one of `arg_offsets` holds an INT Variant. This is the
	// run-time half of GDScript's rule that abs(2) is 2 and abs(2.0) is 2.0.
	void emit_args_all_int(uint8_t rd, const std::vector<int>& arg_offsets);

	// Write the double in `fs` into the result Variant, as whatever the global
	// returns: the double itself, its truncation to an integer, or a boolean.
	void emit_global_double_result(int result_offset, uint8_t fs, GlobalResult result);

	// Get stack offset for a virtual register (in bytes)
	int get_variant_stack_offset(int virtual_reg);

	// Get the stack offset of a scratch Variant slot (in bytes).
	// Scratch slots hold the short-lived Variants that a single IR instruction has to
	// materialize -- an immediate operand, or the result of a comparison that is
	// consumed by the branch right after it. They are reserved as part of the stack
	// frame, and reused by every instruction, so they must never be expected to survive
	// past the instruction that wrote them.
	int get_scratch_variant_offset(int index = 0);

	// Syscall result handling helpers
	// Store syscall result from register to Variant, with optional register allocation
	// result_vreg: virtual register for the result
	// result_reg: physical register containing the result (e.g., REG_A0 after syscall)
	// result_offset: stack offset for the Variant
	// variant_type: Variant type value (e.g., 2 for INT, 24 for GDOBJECT)
	void emit_syscall_result(int result_vreg, uint8_t result_reg, int result_offset, int variant_type);

	// Stack manipulation helpers
	// Adjust stack pointer by a constant amount (positive or negative)
	// Handles both small immediates (< 2048) and large values
	void emit_stack_adjust(int32_t amount);

	// Load stack pointer offset into a register
	// If offset < 2048, uses addi; otherwise loads into temp register and adds
	void emit_load_stack_offset(uint8_t rd, int32_t offset);

	// Check if a Variant type needs permanent storage (refcounted/complex types)
	static bool is_complex_variant_type(int variant_type);

	// Output buffer
	std::vector<uint8_t> m_code;
	std::unordered_map<std::string, size_t> m_labels;
	struct LabelUse {
		std::string label;
		size_t code_offset;
		int32_t addend;
	};
	std::vector<LabelUse> m_label_uses;
	std::unordered_map<std::string, size_t> m_functions;

	// Register allocator
	RegisterAllocator m_allocator;

	// -= Per-function state =-
	//
	// Stack offsets are keyed by virtual register number, and virtual register
	// numbers restart at 0 in every function, so all of this means nothing once
	// the function ends. Clearing each piece by hand at the top of
	// gen_function() is the shape that let a stale register type survive a
	// function boundary in the IR code generator; grouping it means a field
	// added here is reset with the rest, and the guard below takes it away
	// again when the function ends so it cannot be read between functions.
	//
	// Unlike CodeGenerator::FunctionContext this is not passed down: every one
	// of the hundred-odd encoder helpers reaches for the frame size or a slot
	// offset, and threading a parameter through all of them would buy no
	// safety the guard does not already give.
	struct FunctionState {
		// For VARIANT values: virtual_reg -> stack offset
		std::unordered_map<int, int> variant_offsets;
		size_t num_params = 0;      // Number of parameters in current function
		int stack_frame_size = 0;   // Total stack frame size in bytes
		int next_variant_slot = 0;  // Next Variant slot to allocate
		int scratch_slot_base = 0;  // First Variant slot of the scratch area
		int current_instr_idx = 0;  // Current instruction index for register allocation
	};

	FunctionState m_fn;

	// Installs a fresh FunctionState for the length of one function, and takes
	// it away again afterwards.
	struct FunctionStateGuard {
		RISCVCodeGen& gen;
		explicit FunctionStateGuard(RISCVCodeGen& g) : gen(g) { gen.m_fn = FunctionState {}; }
		~FunctionStateGuard() { gen.m_fn = FunctionState {}; }
		FunctionStateGuard(const FunctionStateGuard&) = delete;
		FunctionStateGuard& operator=(const FunctionStateGuard&) = delete;
	};

	// Number of scratch Variant slots reserved in every stack frame. No instruction
	// expansion needs more than one live at a time; the second is headroom.
	static constexpr int SCRATCH_VARIANT_SLOTS = 2;

	// Variant structure layout
	// Variant layout: [uint32_t m_type (4)] [padding (4)] [union data (4 * sizeof(real_t))]
	VariantLayout m_layout;

	static constexpr int VARIANT_TYPE_OFFSET = VariantLayout::TYPE_OFFSET; // m_type field (4 bytes)
	static constexpr int VARIANT_DATA_OFFSET = VariantLayout::DATA_OFFSET; // union data (bool/int64/etc)

	// Layout-dependent sizes and offsets, all routed through m_layout
	int variant_size() const { return m_layout.variant_size(); }
	int real_size() const { return m_layout.real_size(); }
	int real_offset(int index) const { return m_layout.real_offset(index); }
	static constexpr int int_offset(int index) { return VariantLayout::int_offset(index); }

	// Space reserved at the bottom of every frame for saved ra, fp and a0
	static constexpr int SAVED_REG_SPACE = 24;

	// String constants from IR
	const std::vector<std::string>* m_string_constants = nullptr;

	// Constant pool for large immediates (64-bit values that can't be encoded in instructions)
	std::vector<int64_t> m_constant_pool;
	std::unordered_map<int64_t, size_t> m_constant_pool_map; // value -> index

	// Add a constant to the pool and return its index
	size_t add_constant(int64_t value);

	// Generate a unique local label for internal use
	std::string gen_local_label(const std::string& prefix);

	// Convert a Variant component (at comp_offset) to real_t and store to result_offset + store_offset
	// Handles both INT and FLOAT Variants
	// If normalize_by_255 is true, divides integers by 255.0 (for Color components)
	void emit_variant_component_to_real(int comp_offset, int result_offset, int store_offset, bool normalize_by_255 = false);

	// Label counter for generating unique local labels
	int m_label_counter = 0;

	// Global variables
	std::vector<IRGlobalVar> m_globals;
	size_t m_global_count = 0;
	size_t m_global_data_size = 0;

	// Property name strings for @export globals
	// These are stored as: vector of {string_data, label_name}
	std::vector<std::pair<std::string, std::string>> m_property_name_strings;

	// RISC-V RV64I register definitions
	static constexpr uint8_t REG_ZERO = 0;  // x0 - always zero
	static constexpr uint8_t REG_RA = 1;    // x1 - return address
	static constexpr uint8_t REG_SP = 2;    // x2 - stack pointer
	static constexpr uint8_t REG_GP = 3;    // x3 - global pointer
	static constexpr uint8_t REG_TP = 4;    // x4 - thread pointer
	static constexpr uint8_t REG_T0 = 5;    // x5-x7 - temporaries
	static constexpr uint8_t REG_T1 = 6;
	static constexpr uint8_t REG_T2 = 7;
	// x8-x9 are s0-s1 (saved registers)
	// x10-x17 are a0-a7 (argument registers)
	// x18-x27 are s2-s11 (saved registers)
	static constexpr uint8_t REG_T3 = 28;   // x28-x31 - temporaries
	static constexpr uint8_t REG_T4 = 29;
	static constexpr uint8_t REG_T5 = 30;
	static constexpr uint8_t REG_T6 = 31;

	static constexpr uint8_t REG_FP = 8;    // x8 - frame pointer
	static constexpr uint8_t REG_S1 = 9;    // x9 - saved register
	static constexpr uint8_t REG_A0 = 10;   // x10-x17 - arguments/return values
	static constexpr uint8_t REG_A1 = 11;
	static constexpr uint8_t REG_A2 = 12;
	static constexpr uint8_t REG_A3 = 13;
	static constexpr uint8_t REG_A4 = 14;
	static constexpr uint8_t REG_A5 = 15;
	static constexpr uint8_t REG_A6 = 16;
	static constexpr uint8_t REG_A7 = 17;

	static constexpr uint8_t REG_FA0 = 0;  // f0-f1 - floating-point return values
	static constexpr uint8_t REG_FA1 = 1;  // f0-f7 - floating-point arguments
	static constexpr uint8_t REG_FA2 = 2;
	static constexpr uint8_t REG_FA3 = 3;
	static constexpr uint8_t REG_FA4 = 4;
	static constexpr uint8_t REG_FA5 = 5;
	static constexpr uint8_t REG_FA6 = 6;
	static constexpr uint8_t REG_FA7 = 7;

	// -= The ABI's floating-point argument registers =-
	//
	// The REG_FA* constants above are f0-f7, which the calling convention calls
	// ft0-ft7: scratch, and what every inline expansion in this backend uses. A
	// syscall that takes floating-point arguments needs the real fa0-fa7, which
	// are f10-f17. Passing REG_FA0 to one of those puts the argument in ft0 and
	// the host reads whatever ft... f10 happened to hold.
	static constexpr uint8_t REG_ABI_FA0 = 10;
};

} // namespace gdscript
