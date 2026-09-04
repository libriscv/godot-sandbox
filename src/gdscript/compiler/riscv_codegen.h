#pragma once
#include "debug_layout.h"
#include "globals.h"
#include "ir.h"
#include "line_table.h"
#include "profiling_layout.h"
#include "register_allocator.h"
#include "trait_cache_layout.h"
#include "variant_layout.h"
#include <array>
#include <set>
#include <string>
#include <unordered_set>
#include <cstdint>

namespace gdscript {

// Every vmcall returns to the address the host resolved from this symbol. The
// image opens with it so that return lands in the program's own execute segment
// - a native return under bintr/asmjit - instead of libriscv's injected
// trampoline page, which costs a dispatch and a segment switch. Same two
// instructions as that trampoline: STOP, then a jump back onto it, so resuming
// a stopped machine stops again instead of falling into the entry code that
// follows. The program entry begins after both.
inline constexpr const char *FAST_EXIT_SYMBOL = "fast_exit";
inline constexpr size_t FAST_EXIT_SIZE = 8;

class RISCVCodeGen {
public:
	explicit RISCVCodeGen(const VariantLayout& layout = native_variant_layout(),
		bool profiling = false, ProfilingClock profiling_clock = ProfilingClock::TIME,
		bool debug_info = false, const std::vector<uint32_t>& breakpoint_lines = {},
		bool debug_step_points = false);

	std::vector<uint8_t> generate(const IRProgram& program);

	static constexpr size_t TRAIT_CACHE_ENTRIES = TraitCacheLayout::ENTRIES;

	const VariantLayout& get_layout() const { return m_layout; }
	const std::unordered_map<std::string, size_t>& get_function_offsets() const { return m_functions; }
	const RegisterAllocator& get_allocator() const { return m_allocator; }
	const std::vector<int64_t>& get_constant_pool() const { return m_constant_pool; }
	const std::vector<IRGlobalVar>& get_globals() const { return m_globals; }
	size_t get_global_data_size() const { return m_global_data_size; }
	uint64_t get_global_address() const { return m_global_address; }
	size_t get_global_area_size() const { return m_data_global_count * variant_size(); }

	uint64_t get_profiling_address() const { return m_profiling_address; }
	size_t get_profiling_size() const { return m_profiling_size; }

	uint64_t get_debug_address() const { return m_debug_address; }
	size_t get_debug_size() const { return m_debug_size; }

	// One cache per trait, published so the host can invalidate them after a
	// script change makes an object's answer stale.
	uint64_t get_trait_cache_address() const { return m_trait_cache_address; }
	size_t get_trait_cache_count() const { return m_trait_cache_count; }
	static constexpr size_t trait_cache_bytes() { return TraitCacheLayout::AREA_SIZE; }

	uint64_t get_instance_blob_address() const { return m_instance_blob_address; }
	size_t get_instance_blob_size() const { return m_instance_blob_size; }
	size_t get_instance_init_offset() const { return m_instance_init_offset; }
	size_t get_instance_member_count() const { return m_instance_count; }

	// Metadata; produced by every generate(), costs no instructions.
	const LineTable& get_line_table() const { return m_line_table; }
	const std::vector<DebugVariableRecord>& get_debug_variables() const { return m_debug_variables; }

	// Subset of requested breakpoints that got emitted.
	const std::vector<uint32_t>& get_installed_breakpoints() const { return m_installed_breakpoints; }

	// Out-of-range immediates are trapped, not masked: masking silently emits a different instruction.
	static constexpr int I_TYPE_IMM_BITS = 12;
	static constexpr int S_TYPE_IMM_BITS = 12;
	static constexpr int B_TYPE_IMM_BITS = 13;
	static constexpr int J_TYPE_IMM_BITS = 21;

	static bool fits_in_signed(int64_t value, int bits);
	static void check_immediate(const std::string& what, int64_t value, int bits);
	static void check_displacement(const std::string& what, int64_t offset, int bits);

	// Reserved for wide-offset address materialization. Using t0-t2 clobbered live values.
	static constexpr uint8_t REG_WIDE_SCRATCH = 31; // t6

	// '.' prefix prevents collision with GDScript identifiers.
	static constexpr const char* GLOBAL_INIT_LABEL = ".init_globals";

private:
	struct Function {
		std::string name;
		size_t offset;
	};

	void gen_function(const IRFunction& func);

	// Three phases sharing state through m_fn: frame layout, prologue, per-instruction.
	void plan_frame(const IRFunction& func);
	void emit_prologue(const IRFunction& func, const std::vector<int>* parameter_destinations = nullptr);
	// Zero all slot type tags; promote_frame_handles reads the whole array.
	void emit_zero_variant_slots();
	void gen_scope_mark(const IRInstruction& instr);
	void gen_scope_release(const IRInstruction& instr);
	int scope_slot_offset(int scope_id) const;
	bool scope_is_elided(int scope_id) const;
	void plan_scopes(const IRFunction& func);
	void plan_program_call_properties(const IRProgram& program);
	// Frame slots holding nothing anyone will read again at each SCOPE_RELEASE.
	void plan_release_clears(const IRFunction& func);
	// Fixed-type scalar values use a stable callee-saved register for the whole
	// function. Their Variant slots are shadows, materialized only at host/ABI
	// visibility points. Move-only temporaries may share the destination's slot.
	void plan_scalar_residency(const IRFunction& func);
	// Untyped numeric loop sites cache a successful runtime FLOAT/FLOAT guard in
	// one callee-saved register. The generic path remains for every other type.
	void plan_numeric_loop_modes(const IRFunction& func);
	void emit_save_resident_registers();
	void emit_restore_resident_registers();
	void emit_initialize_resident_values();
	void sync_resident_value(int vreg);
	void sync_all_resident_values();
	void sync_instruction_resident_reads(const IRInstruction& instr);
	void reload_resident_value(int vreg);
	bool instruction_reads_residents_directly(const IRInstruction& instr) const;
	bool key_is_fixed_int(int vreg) const;
	bool operand_is_read_from_register(const IRInstruction& instr, size_t index) const;
	void emit_int_key(int key_vreg, int key_offset);
	int resident_int_register(int vreg) const;
	int resident_float_register(int vreg) const;
	bool scope_body_may_allocate(const IRFunction& func, size_t mark_index) const;
	bool instruction_may_ecall(const IRInstruction& instr) const;
	// Narrower: an ecall whose answer is a register value leaves no scoped
	// variant behind, so a loop body made only of those needs no release.
	bool instruction_may_allocate_scoped(const IRInstruction& instr) const;
	static bool global_call_may_ecall(GlobalFn fn);
	void gen_instruction(const IRInstruction& instr);
	void gen_call(const IRInstruction& instr);
	void gen_call_hosted(const IRInstruction& instr);
	void gen_vset(const IRInstruction& instr);
	void gen_fused_branch(const IRInstruction& instr);
	void gen_comparison(const IRInstruction& instr);
	void gen_make_packed_array(const IRInstruction& instr);
	void gen_binary_op(const IRInstruction& instr);
	void gen_print(const IRInstruction& instr);
	void gen_throw(const IRInstruction& instr);
	void gen_switch(const IRInstruction& instr);
	void gen_await(const IRInstruction& instr);
	void gen_vget_inline(const IRInstruction& instr);
	void gen_vset_inline(const IRInstruction& instr);
	void gen_vget(const IRInstruction& instr);
	void gen_store_global(const IRInstruction& instr);
	void gen_make_array(const IRInstruction& instr);
	void gen_make_dictionary(const IRInstruction& instr);
	void gen_vcall(const IRInstruction& instr);
	void gen_construct(const IRInstruction& instr);
	void gen_call_syscall(const IRInstruction& instr);
	void gen_trait_test(const IRInstruction& instr);
	// Per-syscall expansions; each has its own calling convention.
	void gen_syscall_get_obj(const IRInstruction& instr, int result_vreg);
	void gen_syscall_node_create(const IRInstruction& instr, int result_vreg);
	void gen_syscall_class_bind(const IRInstruction& instr, int result_vreg);
	void gen_syscall_array_size(const IRInstruction& instr, int result_vreg);
	void gen_syscall_string_size(const IRInstruction& instr, int result_vreg);
	void gen_syscall_array_at(const IRInstruction& instr, int result_vreg);
	void gen_syscall_string_at(const IRInstruction& instr, int result_vreg);
	void gen_syscall_variant_get(const IRInstruction& instr, int result_vreg);
	void gen_syscall_string_batch(const IRInstruction& instr, int result_vreg);
	void gen_syscall_string_codepoint_batch(const IRInstruction& instr, int result_vreg);
	void gen_syscall_array_batch(const IRInstruction& instr, int result_vreg);
	void gen_syscall_dictionary_ops(const IRInstruction& instr, int result_vreg);
	void gen_dict_const(const IRInstruction& instr);
	void gen_struct_check(const IRInstruction& instr);
	void gen_make_dictionary_keyed(const IRInstruction& instr);
	void gen_get_node(const IRInstruction& instr);
	void gen_load_resource(const IRInstruction& instr);
	void gen_make_callable(const IRInstruction& instr);
	void gen_load_resource_var(const IRInstruction& instr);

	// Querying commits to return forwarding for this vreg.
	std::pair<uint8_t, int> value_destination(int vreg);

	void emit_word(uint32_t word);
	void emit_r_type(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, uint8_t rs2, uint8_t funct7);
	void emit_i_type(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, int32_t imm);
	void emit_s_type(uint8_t opcode, uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm);
	void emit_b_type(uint8_t opcode, uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm);
	void emit_u_type(uint8_t opcode, uint8_t rd, uint32_t imm);
	void emit_j_type(uint8_t opcode, uint8_t rd, int32_t imm);
	void emit_r4_type(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, uint8_t rs2, uint8_t rs3, uint8_t funct2);

	void emit_li(uint8_t rd, int64_t imm);
	void emit_la(uint8_t rd, const std::string& label, int32_t addend = 0);
	void emit_mv(uint8_t rd, uint8_t rs);
	void emit_addi(uint8_t rd, uint8_t rs1, int32_t imm);

	// addi when offset fits 12-bit signed, li+add otherwise. Uses REG_WIDE_SCRATCH when rd == base.
	void emit_add_offset(uint8_t rd, uint8_t base, int32_t offset);

	// Index folded into relocation; a post-la addi truncates at 85 globals.
	void emit_address_of_global(uint8_t rd, size_t index);
	void emit_address_of_global_area(uint8_t rd);
	void emit_address_of_init_scratch(uint8_t rd);
	void emit_instance_init(const IRProgram& program);
	bool emits_instance_init(const IRProgram& program) const;
	void emit_folded_initializers(const IRProgram& program, bool members);

	void emit_load_return_pointer();

	// No default in switch: new opcodes must answer. "no" only where expansion is pure frame arithmetic.
	static bool opcode_clobbers_abi_registers(IROpcode op);

	static std::vector<bool> find_return_forwarding(const IRFunction& func);
	static std::vector<bool> find_live_parameters(const IRFunction& func);

	// Program-wide counter for unique SWITCH table labels.
	size_t m_switch_tables = 0;

	static constexpr const char* GLOBALS_LABEL = ".globals";
	static constexpr const char* INSTANCE_LABEL = ".instance";
	static constexpr const char* INSTANCE_BLOB_LABEL = ".instance_blob";
	static constexpr const char* INSTANCE_INIT_LABEL = ".instance_init";
	static constexpr const char* MEMBER_INIT_LABEL = ".init_members";
	static constexpr const char* PROFILING_LABEL = ".profiling";
	static constexpr const char* DEBUG_LABEL = ".debug";

	// Uses t0-t5 only; dead at entry (args in a0-a7) and exit (retval written).
	void emit_profiling_entry();
	void emit_profiling_exit();

	// Shadow-stack push/pop. Entry runs before ra is spilled.
	void emit_debug_entry();
	void emit_debug_exit();

	// Saves/restores the two regs it uses; safe between arbitrary IR instructions.
	void emit_breakpoint(int32_t line, bool installed = true, bool user_stop = true,
			bool source_stop = false);

	// Coroutine resume entry: restores frame, dispatches on state index. No ELF symbol.
	void emit_coroutine_resume_entry(const IRFunction& func);
	// Unwind frame and return; the host hands back the awaitable, not the return value.
	void emit_suspend_epilogue();
	// Zba jump table indexed by index_reg; falls to past_label on out-of-range.
	void emit_dense_jump_table(uint8_t index_reg, const std::vector<std::string>& labels,
		const std::string& past_label);
	// Materialize resident scalars, then invalidate legacy allocator mappings.
	void spill_all_registers();

	// Appends a line-table row. `force` emits line 0 (function entry only).
	void record_line(int32_t line, bool force = false);
	// CSR number is unsigned 12-bit; emit_i_type rejects it as signed.
	void emit_csrr(uint8_t rd, uint32_t csr);
	void emit_add(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_sub(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_mul(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_div(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_rem(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_and(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_or(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_xor(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_xori(uint8_t rd, uint8_t rs, int32_t imm);
	void emit_andi(uint8_t rd, uint8_t rs, int32_t imm);
	void emit_ori(uint8_t rd, uint8_t rs, int32_t imm);
	void emit_sll(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_srl(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_sra(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_slt(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_slti(uint8_t rd, uint8_t rs, int32_t imm);
	void emit_seqz(uint8_t rd, uint8_t rs);   // sltiu rd, rs, 1
	void emit_snez(uint8_t rd, uint8_t rs);   // sltu rd, x0, rs
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

	// Auto-handle wide offsets via REG_WIDE_SCRATCH.
	void emit_load_with_offset(uint8_t opcode, uint8_t funct3, uint8_t rd, uint8_t rs1, int32_t offset);
	void emit_store_with_offset(uint8_t opcode, uint8_t funct3, uint8_t rs2, uint8_t rs1, int32_t offset);

	void emit_ld(uint8_t rd, uint8_t rs1, int32_t offset);
	void emit_lw(uint8_t rd, uint8_t rs1, int32_t offset);
	void emit_lwu(uint8_t rd, uint8_t rs1, int32_t offset);
	void emit_lh(uint8_t rd, uint8_t rs1, int32_t offset);
	void emit_lb(uint8_t rd, uint8_t rs1, int32_t offset);
	void emit_lbu(uint8_t rd, uint8_t rs1, int32_t offset);
	void emit_sd(uint8_t rs2, uint8_t rs1, int32_t offset);
	void emit_sw(uint8_t rs2, uint8_t rs1, int32_t offset);
	void emit_sh(uint8_t rs2, uint8_t rs1, int32_t offset);
	void emit_sb(uint8_t rs2, uint8_t rs1, int32_t offset);

	void emit_fld(uint8_t rd, uint8_t rs1, int32_t offset);
	void emit_fsd(uint8_t rs2, uint8_t rs1, int32_t offset);
	void emit_flw(uint8_t rd, uint8_t rs1, int32_t offset);
	void emit_fsw(uint8_t rs2, uint8_t rs1, int32_t offset);
	void emit_fcvt_d_s(uint8_t rd, uint8_t rs1);
	void emit_fcvt_s_d(uint8_t rd, uint8_t rs1);
	void emit_fcvt_d_l(uint8_t rd, uint8_t rs1);
	void emit_fmv_s(uint8_t rd, uint8_t rs);

	// real_t-width: selects 32-bit or 64-bit FP instruction per VariantLayout.
	void emit_flr(uint8_t rd, uint8_t rs1, int32_t offset);
	void emit_fsr(uint8_t rs2, uint8_t rs1, int32_t offset);
	void emit_fcvt_d_r(uint8_t rd, uint8_t rs1);
	void emit_fcvt_r_d(uint8_t rd, uint8_t rs1);
	void emit_fadd_r(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_fsub_r(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_fmul_r(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_fdiv_r(uint8_t rd, uint8_t rs1, uint8_t rs2);

	void emit_fadd_d(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_fsub_d(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_fmul_d(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_fdiv_d(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_fmv_d(uint8_t rd, uint8_t rs);
	void emit_fmv_d_x(uint8_t rd, uint8_t rs);
	void emit_fsqrt_d(uint8_t rd, uint8_t rs1);
	void emit_fabs_d(uint8_t rd, uint8_t rs1);              // fsgnjx.d rd, rs, rs
	void emit_flt_d(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_feq_d(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_fcvt_l_d(uint8_t rd, uint8_t rs1);

	void emit_fadd_s(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_fsub_s(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_fmul_s(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_fdiv_s(uint8_t rd, uint8_t rs1, uint8_t rs2);

	void emit_sext_w(uint8_t rd, uint8_t rs);               // addiw rd, rs, 0
	void emit_srai(uint8_t rd, uint8_t rs, uint8_t shamt);
	void emit_slli(uint8_t rd, uint8_t rs, uint8_t shamt);
	void emit_sh2add(uint8_t rd, uint8_t rs1, uint8_t rs2); // Zba

	void emit_call(const std::string& func_name);
	void emit_jump(const std::string& label);

	void define_label(const std::string& label);
	// IR label operand: already interned, no name lookup.
	void define_label(const IRValue& label);
	// Addend folded into AUIPC+ADDI; a separate addi truncates outside 12-bit range.
	void mark_label_use(const std::string& label, size_t code_offset, int32_t addend = 0);
	void mark_label_use(const IRValue& label, size_t code_offset, int32_t addend = 0);

	// Out-of-range B-type branches become b<inv>+8 / jal x0,target (+-1MB). Runs to fixpoint.
	void relax_branches();

	void resolve_labels();

	void emit_load_variant_type(uint8_t rd, uint8_t base_reg, int32_t variant_offset);
	void emit_store_variant_type(uint8_t rs, uint8_t base_reg, int32_t variant_offset);
	// Variant::booleanize(). type_hint == TypeHint_NONE skips the fast path.
	void emit_variant_truthy(uint8_t rd, int variant_offset, int32_t type_hint, uint8_t base_reg = REG_SP);

	// Unary Variant::evaluate(). Scratch slot 1 holds the NIL rhs Godot expects.
	void emit_variant_eval_unary(int result_offset, int operand_offset, int op);
	void emit_variant_eval_unary(int result_offset, uint8_t operand_base, int operand_offset, int op);
	void emit_load_variant_bool(uint8_t rd, uint8_t base_reg, int32_t variant_offset);
	void emit_store_variant_bool(uint8_t rs, uint8_t base_reg, int32_t variant_offset);
	void emit_load_variant_int(uint8_t rd, uint8_t base_reg, int32_t variant_offset);
	void emit_store_variant_int(uint8_t rs, uint8_t base_reg, int32_t variant_offset);

	// base_reg defaults to sp; return values use a0.
	void emit_variant_create_int(int stack_offset, int64_t value, uint8_t base_reg = REG_SP);
	void emit_variant_create_bool(int stack_offset, bool value, uint8_t base_reg = REG_SP);
	void emit_variant_create_float(int stack_offset, double value, uint8_t base_reg = REG_SP);
	// variant_type: STRING, STRING_NAME (&""), or NODE_PATH (^"").
	void emit_variant_create_string(int stack_offset, int string_idx, int variant_type = Variant::STRING);

	// Both endpoints must be 8-byte aligned.
	void emit_variant_move(uint8_t dst_base, int32_t dst_offset, uint8_t src_base, int32_t src_offset, uint8_t tmp_reg);
	void emit_scalar_variant_move(uint8_t dst_base, int32_t dst_offset, uint8_t src_base,
		int32_t src_offset, uint8_t tmp_reg);
	// handle_clobbering=false when caller already spilled a0-a3 (required on branch paths).
	void emit_variant_eval(int result_offset, int lhs_offset, int rhs_offset, int op,
	                       bool handle_clobbering = true);

	// Branches to slow_label unless both type tags are INT (and >= 0 when require_non_negative).
	// A side whose tag is already known (INT or FLOAT) is not tested again.
	void emit_branch_unless_both_int(int lhs_offset, int rhs_offset, const std::string& slow_label,
	                                 bool require_non_negative,
	                                 int lhs_known = IRInstruction::TypeHint_NONE,
	                                 int rhs_known = IRInstruction::TypeHint_NONE);

	static bool has_int_fast_path(IROpcode op);

	static bool has_float_fast_path(IROpcode op);
	void emit_branch_unless_float_pair(int lhs_offset, int rhs_offset, const std::string& slow_label,
	                                   int lhs_known = IRInstruction::TypeHint_NONE,
	                                   int rhs_known = IRInstruction::TypeHint_NONE);
	void emit_numeric_to_double(uint8_t fd, int variant_offset,
	                            int known = IRInstruction::TypeHint_NONE);
	void emit_float_pair_binary_op(int result_offset, int lhs_offset, int rhs_offset, IROpcode op,
	                               int lhs_known = IRInstruction::TypeHint_NONE,
	                               int rhs_known = IRInstruction::TypeHint_NONE);
	// INT or FLOAT when the block already proved the vreg's tag, else TypeHint_NONE.
	int numeric_known_tag(int vreg) const;
	void emit_float_pair_comparison(int result_offset, int lhs_offset, int rhs_offset, IROpcode cmp_op);
	void emit_float_pair_fused_branch(IROpcode op, int lhs_offset, int rhs_offset, const std::string& label);
	void emit_typed_float_comparison(int result_vreg, int result_offset,
		int lhs_vreg, int lhs_offset, int rhs_vreg, int rhs_offset, IROpcode cmp_op);
	void emit_typed_float_fused_branch(IROpcode op, int lhs_vreg, int lhs_offset,
		int rhs_vreg, int rhs_offset, const std::string& label);
	void emit_double_compare(uint8_t rd, IROpcode cmp_op, uint8_t lhs, uint8_t rhs);

	// Integer-typed BRANCH_EQ..BRANCH_GTE on int64 payloads.
	void emit_int_fused_branch(IROpcode op, int lhs_vreg, int lhs_offset,
		int rhs_vreg, int rhs_offset, const std::string& label);
	void emit_int_fused_branch_imm(IROpcode op, int value_vreg, int value_offset,
		int64_t imm, bool constant_on_left, const std::string& label);

	// ECALL_ARRAY_AT; index_nonnegative elides the negative-index wrap, while a
	// negative constant read is tagged for the host to wrap in the same syscall.
	void emit_array_element_access(bool is_set, int array_offset, int index_offset, int value_offset,
		bool index_nonnegative = false, bool negative_constant_get = false,
		int array_vreg = -1, int index_vreg = -1);

	// Caller sets up data_ptr_reg (or REG_ZERO for nullptr) before calling.
	void emit_vcreate_syscall(int variant_type, int method, uint8_t data_ptr_reg, int result_offset);

	void emit_variant_create_empty_array(int stack_offset);
	void emit_variant_create_empty_dictionary(int stack_offset);

	// Native int arithmetic; vreg != -1 may resolve to REG_T2 via chaining.
	void emit_typed_int_binary_op(int result_vreg, int result_offset, int lhs_offset, int rhs_offset, IROpcode op,
		int lhs_vreg = -1, int rhs_vreg = -1);
	void emit_typed_int_comparison(int result_vreg, int result_offset, int lhs_vreg,
		int lhs_offset, int rhs_vreg, int rhs_offset, IROpcode cmp_op);
	void emit_typed_int_comparison_imm(int result_vreg, int result_offset, int value_vreg,
		int value_offset, int64_t imm, bool constant_on_left, IROpcode cmp_op);
	void emit_typed_float_binary_op(int result_vreg, int result_offset, int lhs_vreg,
		int lhs_offset, int rhs_vreg, int rhs_offset, IROpcode op);
	void emit_typed_vector_binary_op(int result_vreg, int result_offset, int lhs_offset, int rhs_offset, IROpcode op, IRInstruction::TypeHint type_hint);
	void emit_typed_vector_scalar_op(int result_vreg, int result_offset, int vector_offset, int scalar_offset,
		bool scalar_on_left, IROpcode op, IRInstruction::TypeHint vector_type,
		IRInstruction::TypeHint scalar_type);

	// GLOBAL_CALL emission (riscv_globals.cpp); table in globals.h.
	void emit_global_call(const IRInstruction& instr);
	// `typed` == args are already the form's expected Variant type; skips run-time type test.
	void emit_global_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
		int result_offset, bool typed);
	void emit_global_int_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
		int result_offset, bool typed);
	void emit_global_float_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
		int result_offset, bool typed);
	void emit_global_syscall_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
		int result_offset, bool typed);
	void emit_global_int_syscall_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
		int result_offset, bool typed);
	void emit_global_host_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
		int result_offset);

	// `known` true only where type is established by hint or emit_args_all_int() dispatch.
	void emit_variant_to_double(uint8_t fd, int variant_offset, bool known_float);
	void emit_variant_to_int(uint8_t rd, int variant_offset, bool known_int);

	// rd = 1 iff all arg_offsets hold INT Variants (run-time type dispatch for globals).
	void emit_args_all_int(uint8_t rd, const std::vector<int>& arg_offsets);

	void emit_global_double_result(int result_offset, uint8_t fs, GlobalResult result);
	void emit_global_int_result(int result_offset, uint8_t rs, GlobalResult result);

	int get_variant_stack_offset(int virtual_reg);

	// Immediate folding; folds_to_immediate() is the single decision point.
	void plan_constants(const IRFunction& func);
	bool constant_int(int vreg, int64_t& value) const;
	bool folds_to_immediate(const IRInstruction& instr, size_t operand_index) const;
	static bool int_op_takes_immediate(IROpcode op, int64_t value);
	static bool int_op_is_commutative(IROpcode op);
	static bool int_compare_takes_immediate(IROpcode op, int64_t value,
		bool constant_on_left);
	void emit_int_compare_imm(uint8_t result, uint8_t value, int64_t imm,
		IROpcode op, bool constant_on_left);
	void emit_typed_int_binary_op_imm(int result_vreg, int result_offset, int lhs_offset, int64_t imm, IROpcode op,
		int lhs_vreg = -1);
	void plan_int_chaining(const IRFunction& func);
	void plan_nonnegative(const IRFunction& func);
	void plan_global_handles(const IRFunction& func);
	static std::vector<std::vector<size_t>> build_successors(const IRFunction& func);
	// Scoped index into rd from the frame slot or global data area.
	void emit_container_handle(uint8_t rd, int vreg, int offset);
	std::pair<uint8_t, int> variant_source(int vreg, int offset, uint8_t scratch);
	static bool is_typed_int_binary(const IRInstruction& instr);
	// Typed payload cache. Slots remain canonical; these caller-saved registers
	// merely avoid reloading values inside one basic block.
	void clear_block_value_state();
	void invalidate_cached_vreg(int vreg);
	void note_known_tag(int vreg, int tag);
	void emit_known_variant_type(int vreg, int offset, int tag);
	void cache_int_result(int vreg, uint8_t source);
	void cache_float_result(int vreg, uint8_t source);
	uint8_t emit_int_operand(uint8_t rd, int vreg, int offset);
	uint8_t emit_float_operand(uint8_t fd, int vreg, int offset);
	void emit_typed_int_result(int result_vreg, int result_offset, uint8_t source);

	// Per-instruction temporaries; do not survive past the emitting instruction.
	int get_scratch_variant_offset(int index = 0);

	// Spill allocator-held values in clobbered_regs before ecall setup.
	void spill_around_syscall(const std::vector<uint8_t>& clobbered_regs);

	void emit_syscall_result(int result_vreg, uint8_t result_reg, int result_offset, int variant_type);
	void emit_stack_adjust(int32_t amount);
	void emit_load_stack_offset(uint8_t rd, int32_t offset);
	static bool is_complex_variant_type(int variant_type);

	std::vector<uint8_t> m_code;

	// Label offsets by id, not by name. The id space is seeded from the IR's own
	// table, so an IR label operand already carries its id; backend-synthetic
	// names (.LC0, .LSTR0, function symbols) intern past the end of it.
	// resolve_labels() and relax_branches() then index instead of hashing.
	static constexpr size_t NO_LABEL = SIZE_MAX;
	IRStringTable m_label_names;
	std::vector<size_t> m_label_offsets;
	uint32_t label_id(const std::string& name);
	void set_label(uint32_t id, size_t offset);
	struct LabelUse {
		uint32_t label;
		size_t code_offset;
		int32_t addend;
	};
	std::vector<LabelUse> m_label_uses;
	std::unordered_map<std::string, size_t> m_functions;
	std::unordered_set<std::string> m_scoped_clean_functions;
	std::unordered_set<std::string> m_trusted_internal_entries;

	RegisterAllocator m_allocator;

	// Reset per function via FunctionStateGuard; keyed by vreg (restarts at 0 each function).
	struct FunctionState {
		std::unordered_map<int, int> variant_offsets;
		size_t num_params = 0;
		int stack_frame_size = 0;
		int next_variant_slot = 0;
		int scratch_slot_base = 0;
		int current_instr_idx = 0;

		// value_destination() reads this for the current instruction.
		bool forward_return = false;

		// False outside gen_function() (entry stub / export table have no frame).
		bool in_function = false;
		// Member initializers execute in a temporary host call state; complex
		// results stored there must be promoted before that state is reset.
		bool is_member_initializer = false;

		// Set by plan_frame() before first instruction; ecall/CALL expansion asserts consistency.
		bool saves_return_address = false;
		bool spills_return_pointer = false;

		// No sp-relative access => prologue/epilogue omitted entirely.
		bool omits_frame = false;

		// Set after writing through a0; RETURN emits only the epilogue. Reset per RETURN.
		bool return_value_written = false;

		// Per-instruction: write to return reg forwarded straight into *a0.
		std::vector<bool> forward_to_return;

		// Per-parameter: incoming Variant read before its register is overwritten.
		std::vector<bool> live_params;

		// vreg -> value for singly-defined int LOAD_IMMs.
		std::unordered_map<int, int64_t> const_ints;

		// Per-instruction: all uses folded; the Variant is never built.
		std::vector<bool> unmaterialized_imm;

		// Per-instruction: result stays in REG_T2 for the next instruction.
		std::vector<bool> int_kept_in_reg;

		// Destination vreg kept in REG_T2 this instruction, or -1.
		int keep_int_in_reg = -1;

		// vreg left in REG_T2 by the previous instruction, or -1.
		int chained_vreg = -1;
		int next_chained_vreg = -1;

		// Proven non-negative vregs; subscripts skip the wrap.
		std::unordered_set<int> nonnegative;

		// vreg -> global index; reads go to the data area, no frame copy.
		std::unordered_map<int, size_t> global_handles;

		bool is_coroutine = false;
		bool has_backedge = false;
		// Bytes occupied by ra/a0 and the callee-saved scalar registers.
		int saved_reg_space = SAVED_FIXED_SPACE;
		// Frame size in bytes from saved_reg_space; checked at resume.
		int variant_space = 0;

		// Above Variant slots so release never reads a mark as a handle.
		int scope_slot_base = 0;
		int scope_slot_count = 0;
		std::vector<bool> elided_scopes;
		std::vector<int> scope_slots;
		// A loop scope whose only allocations returned inline values can skip its
		// expensive host-side release.  One saved register tracks that fact.
		std::vector<int8_t> scope_dirty_regs;
		std::unordered_map<size_t, std::vector<uint8_t>> scope_dirty_updates;
		std::unordered_map<size_t, std::vector<uint8_t>> scope_dirty_sets;
		// Instruction index of a SCOPE_RELEASE -> frame slots that are dead there.
		std::unordered_map<int, std::vector<int>> release_clears;
		// emit_ecall() asserts false; catches predicate drift.
		bool ecall_refused = false;
		// Per-suspension labels, state-ordered; resume dispatches on the index.
		std::vector<std::string> await_states;
		std::string resume_label;

		std::vector<int> known_tags;
		// Static scalar type and stable physical register, indexed by vreg.
		std::vector<int> fixed_scalar_types;
		std::vector<int8_t> resident_int_regs;
		std::vector<int8_t> resident_float_regs;
		// MOVE-coalesced vreg representative. Representatives share a slot and preg.
		std::vector<int> scalar_aliases;
		std::vector<uint8_t> used_int_resident_regs;
		std::vector<uint8_t> used_float_resident_regs;
		struct NumericLoopMode {
			uint8_t preg = 0;
			int carried_vreg = -1;
			size_t move_index = SIZE_MAX;
		};
		std::unordered_map<size_t, NumericLoopMode> numeric_loop_modes;
		std::unordered_map<size_t, uint8_t> numeric_loop_moves;
		std::unordered_map<size_t, uint8_t> numeric_loop_releases;
		// Set by a direct resident definition; otherwise the post-instruction hook
		// reloads the result written to its shadow slot.
		bool resident_result_written = false;
		std::vector<int8_t> int_cache_slots;
		std::vector<int8_t> float_cache_slots;
		// Opaque IR batch token -> first of sixteen dedicated Variant slots.
		// These cannot be ordinary vregs: register compaction is allowed to
		// renumber and discard the otherwise-unreferenced consecutive slots.
		std::unordered_map<int64_t, int> array_batch_offsets;
		std::unordered_map<size_t, std::vector<int64_t>> array_batch_releases;
		std::unordered_map<int64_t, int> codepoint_batch_offsets;
		std::array<int, 3> int_cache_owners {{ -1, -1, -1 }};
		std::array<int, 3> float_cache_owners {{ -1, -1, -1 }};
		uint8_t next_int_cache = 0;
		uint8_t next_float_cache = 0;
	};

	FunctionState m_fn;

	struct FunctionStateGuard {
		RISCVCodeGen& gen;
		explicit FunctionStateGuard(RISCVCodeGen& g) : gen(g) { gen.m_fn = FunctionState {}; }
		~FunctionStateGuard() { gen.m_fn = FunctionState {}; }
		FunctionStateGuard(const FunctionStateGuard&) = delete;
		FunctionStateGuard& operator=(const FunctionStateGuard&) = delete;
	};

	static constexpr int SCRATCH_VARIANT_SLOTS = 2;

	VariantLayout m_layout;

	static constexpr int VARIANT_TYPE_OFFSET = VariantLayout::TYPE_OFFSET;
	static constexpr int VARIANT_DATA_OFFSET = VariantLayout::DATA_OFFSET;

	int variant_size() const { return m_layout.variant_size(); }
	int real_size() const { return m_layout.real_size(); }
	int real_offset(int index) const { return m_layout.real_offset(index); }
	static constexpr int int_offset(int index) { return VariantLayout::int_offset(index); }

	// All accesses are sp-relative and frame size is fixed after planning.
	static constexpr int SAVED_RA_OFFSET = 0;
	static constexpr int SAVED_A0_OFFSET = 8;
	static constexpr int SAVED_FIXED_SPACE = 16;

	const std::vector<std::string>* m_string_constants = nullptr;
	const IRStringTable* m_strings = nullptr;
	const std::vector<ClassSignature>* m_trait_signatures = nullptr;
	bool m_trait_structural_fallback = true;
	size_t m_trait_cache_size = 0;
	size_t m_trait_cache_count = 0;
	uint64_t m_trait_cache_address = 0;
	// Resolves an operand's interned name.
	const std::string& text(const IRValue& value) const;

	std::vector<int64_t> m_constant_pool;
	std::unordered_map<int64_t, size_t> m_constant_pool_map;

	size_t add_constant(int64_t value);
	std::string gen_local_label(const std::string& prefix);

	// INT/FLOAT Variant -> real_t. normalize_by_255 for Color integer components.
	void emit_variant_component_to_real(int comp_offset, int result_offset, int store_offset);
	void emit_variant_component_to_int(int comp_offset, int result_offset, int store_offset);
	void gen_coerce(int dst_vreg, int src_vreg, int target);

	int m_label_counter = 0;

	std::vector<IRGlobalVar> m_globals;
	size_t m_global_count = 0;
	size_t m_global_data_size = 0;
	uint64_t m_global_address = 0;

	std::vector<size_t> m_global_slots;
	size_t m_data_global_count = 0;
	size_t m_instance_count = 0;
	uint64_t m_instance_blob_address = 0;
	size_t m_instance_blob_size = 0;
	size_t m_instance_init_offset = 0;

	bool m_profiling = false;
	ProfilingClock m_profiling_clock = ProfilingClock::TIME;
	uint64_t m_profiling_address = 0;
	size_t m_profiling_size = 0;
	size_t m_profiling_count = 0;
	// -1 outside a profiled function (.init_globals is uninstrumented).
	int m_profiling_index = -1;

	bool m_debug = false;
	uint64_t m_debug_address = 0;
	size_t m_debug_size = 0;
	int m_debug_index = -1; // -1 outside instrumented function

	bool m_debug_step_points = false;
	std::set<uint32_t> m_breakpoints; // requested breakpoint lines
	int32_t m_break_line = 0; // current line; reset per function
	bool m_break_pending = false;
	bool m_emitting_breakpoint = false; // suppresses a0-spill check in emit_ecall
	std::vector<uint32_t> m_installed_breakpoints; // subset actually emitted

	LineTable m_line_table;
	std::vector<DebugVariableRecord> m_debug_variables;
	struct PendingDebugVariable {
		DebugVariableRecord record;
		std::string begin_label;
		std::string end_label;
	};
	std::vector<PendingDebugVariable> m_pending_debug_variables;

	std::vector<std::pair<std::string, std::string>> m_rodata_strings;
	std::unordered_map<std::string, std::string> m_rodata_string_labels;
	std::string rodata_string(const std::string& text);

	static constexpr uint8_t REG_ZERO = 0;
	static constexpr uint8_t REG_RA = 1;
	static constexpr uint8_t REG_SP = 2;
	static constexpr uint8_t REG_GP = 3;
	static constexpr uint8_t REG_TP = 4;
	static constexpr uint8_t REG_T0 = 5;
	static constexpr uint8_t REG_T1 = 6;
	static constexpr uint8_t REG_T2 = 7;
	static constexpr uint8_t REG_T3 = 28;
	static constexpr uint8_t REG_T4 = 29;
	static constexpr uint8_t REG_T5 = 30;
	static constexpr uint8_t REG_T6 = 31;

	static constexpr uint8_t REG_FP = 8;
	static constexpr uint8_t REG_S1 = 9;
	static constexpr uint8_t REG_A0 = 10;
	static constexpr uint8_t REG_A1 = 11;
	static constexpr uint8_t REG_A2 = 12;
	static constexpr uint8_t REG_A3 = 13;
	static constexpr uint8_t REG_A4 = 14;
	static constexpr uint8_t REG_A5 = 15;
	static constexpr uint8_t REG_A6 = 16;
	static constexpr uint8_t REG_A7 = 17;

	// REG_FA* are f0-f7 (ft0-ft7, scratch). Syscalls need the ABI fa0-fa7 = f10-f17.
	static constexpr uint8_t REG_FA0 = 0;
	static constexpr uint8_t REG_FA1 = 1;
	static constexpr uint8_t REG_FA2 = 2;
	static constexpr uint8_t REG_FA3 = 3;
	static constexpr uint8_t REG_FA4 = 4;
	static constexpr uint8_t REG_FA5 = 5;
	static constexpr uint8_t REG_FA6 = 6;
	static constexpr uint8_t REG_FA7 = 7;

	static constexpr uint8_t REG_ABI_FA0 = 10; // f10, the real ABI fa0
};

} // namespace gdscript
