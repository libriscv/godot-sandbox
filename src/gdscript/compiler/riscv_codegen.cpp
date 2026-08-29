#include "riscv_codegen.h"
#include <unordered_set>
#include "compiler_exception.h"
#include "variant_types.h"
#include "builtin_members.h"
#include "instance_layout.h"
#include "syscall_numbers.h"
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <climits>

namespace gdscript {

static constexpr int32_t USAGE_SCRIPT_VARIABLE = 4096;

static bool is_scalar_variant_type(int type) {
	return type == Variant::NIL || type == Variant::BOOL ||
		type == Variant::INT || type == Variant::FLOAT;
}

static bool consecutive_vreg_operands(const IRInstruction& instr, size_t first,
	size_t count, int forbidden_vreg)
{
	if (count == 0 || first + count > instr.operands.size() ||
		instr.operands[first].type != IRValue::Type::REGISTER)
	{
		return false;
	}
	const int base = instr.operands[first].reg_index();
	for (size_t i = 0; i < count; i++) {
		if (instr.operands[first + i].type != IRValue::Type::REGISTER ||
			instr.operands[first + i].reg_index() != base + int(i) ||
			instr.operands[first + i].reg_index() == forbidden_vreg)
		{
			return false;
		}
	}
	return true;
}

RISCVCodeGen::RISCVCodeGen(const VariantLayout& layout, bool profiling, ProfilingClock profiling_clock,
		bool debug_info, const std::vector<uint32_t>& breakpoint_lines, bool debug_step_points) :
		m_layout(layout), m_profiling(profiling), m_profiling_clock(profiling_clock),
		m_debug(debug_info), m_debug_step_points(debug_step_points),
		m_breakpoints(breakpoint_lines.begin(), breakpoint_lines.end()) {
	// Line 0 = unstamped; a breakpoint on it would fire everywhere.
	m_breakpoints.erase(0);
}

size_t RISCVCodeGen::add_constant(int64_t value) {
	auto it = m_constant_pool_map.find(value);
	if (it != m_constant_pool_map.end()) {
		return it->second;
	}

	size_t index = m_constant_pool.size();
	m_constant_pool.push_back(value);
	m_constant_pool_map[value] = index;
	return index;
}

std::string RISCVCodeGen::rodata_string(const std::string& text) {
	auto it = m_rodata_string_labels.find(text);
	if (it != m_rodata_string_labels.end()) {
		return it->second;
	}
	std::string label = ".LSTR" + std::to_string(m_rodata_strings.size());
	m_rodata_strings.push_back({ text, label });
	m_rodata_string_labels.emplace(text, label);
	return label;
}

std::string RISCVCodeGen::gen_local_label(const std::string& prefix) {
	return prefix + std::to_string(m_label_counter++);
}

void RISCVCodeGen::emit_variant_component_to_real(int comp_offset, int result_offset, int store_offset) {
	// INT or FLOAT Variant -> real_t.
	std::string label_float = gen_local_label(".float");
	std::string label_cont = gen_local_label(".cont");

	emit_lwu(REG_T0, REG_SP, comp_offset);
	emit_addi(REG_T1, REG_T0, -2); // 0 = INT, nonzero = FLOAT
	mark_label_use(label_float, m_code.size());
	emit_bne(REG_T1, REG_ZERO, 0);

	// INT path
	emit_ld(REG_T0, REG_SP, comp_offset + 8);
	emit_fcvt_d_l(REG_FA0, REG_T0);
	emit_fcvt_r_d(REG_FA0, REG_FA0);
	mark_label_use(label_cont, m_code.size());
	emit_jal(REG_ZERO, 0);

	// FLOAT path: always 64-bit double, narrow to real_t.
	define_label(label_float);
	emit_fld(REG_FA0, REG_SP, comp_offset + VARIANT_DATA_OFFSET);
	emit_fcvt_r_d(REG_FA0, REG_FA0);
	define_label(label_cont);

	emit_fsr(REG_FA0, REG_SP, result_offset + store_offset);
}

void RISCVCodeGen::emit_variant_component_to_int(int comp_offset, int result_offset, int store_offset) {
	// INT or FLOAT Variant -> int32_t component (truncates, matching Godot).
	std::string label_cont = gen_local_label(".icont");

	emit_lwu(REG_T0, REG_SP, comp_offset);
	emit_load_variant_int(REG_T1, REG_SP, comp_offset);
	emit_addi(REG_T0, REG_T0, -2); // INT(2)->0, FLOAT(3)->1
	mark_label_use(label_cont, m_code.size());
	emit_beq(REG_T0, REG_ZERO, 0);

	// FLOAT path: 64-bit double, fcvt truncates toward zero.
	emit_fld(REG_FA0, REG_SP, comp_offset + VARIANT_DATA_OFFSET);
	emit_fcvt_l_d(REG_T1, REG_FA0);
	define_label(label_cont);

	emit_sw(REG_T1, REG_SP, result_offset + store_offset);
}

void RISCVCodeGen::gen_coerce(int dst_vreg, int src_vreg, int target) {
	const int dst_offset = get_variant_stack_offset(dst_vreg);
	const int src_offset = get_variant_stack_offset(src_vreg);

	const std::string bool_path = gen_local_label(".coerce_bool");
	const std::string float_path = gen_local_label(".coerce_float");
	const std::string store = gen_local_label(".coerce_store");

	emit_lwu(REG_T0, REG_SP, src_offset + VARIANT_TYPE_OFFSET);
	emit_addi(REG_T1, REG_T0, -Variant::BOOL);
	mark_label_use(bool_path, m_code.size());
	emit_beq(REG_T1, REG_ZERO, 0);
	emit_addi(REG_T1, REG_T0, -Variant::FLOAT);
	mark_label_use(float_path, m_code.size());
	emit_beq(REG_T1, REG_ZERO, 0);

	emit_load_variant_int(REG_T0, REG_SP, src_offset);
	if (target == Variant::FLOAT) {
		emit_fcvt_d_l(REG_FA0, REG_T0);
	} else if (target == Variant::BOOL) {
		emit_snez(REG_T0, REG_T0);
	}
	mark_label_use(store, m_code.size());
	emit_jal(REG_ZERO, 0);

	define_label(bool_path);
	emit_lbu(REG_T0, REG_SP, src_offset + VARIANT_DATA_OFFSET);
	if (target == Variant::FLOAT) {
		emit_fcvt_d_l(REG_FA0, REG_T0);
	} else if (target == Variant::BOOL) {
		emit_snez(REG_T0, REG_T0);
	}
	mark_label_use(store, m_code.size());
	emit_jal(REG_ZERO, 0);

	define_label(float_path);
	emit_fld(REG_FA0, REG_SP, src_offset + VARIANT_DATA_OFFSET);
	if (target == Variant::INT) {
		emit_fcvt_l_d(REG_T0, REG_FA0);
	} else if (target == Variant::BOOL) {
		emit_fcvt_d_l(REG_FA1, REG_ZERO);
		emit_feq_d(REG_T0, REG_FA0, REG_FA1);
		emit_xori(REG_T0, REG_T0, 1);
	}

	define_label(store);
	emit_li(REG_T1, target);
	emit_sw(REG_T1, REG_SP, dst_offset + VARIANT_TYPE_OFFSET);
	if (target == Variant::FLOAT) {
		emit_fsd(REG_FA0, REG_SP, dst_offset + VARIANT_DATA_OFFSET);
	} else if (target == Variant::INT) {
		emit_sd(REG_T0, REG_SP, dst_offset + VARIANT_DATA_OFFSET);
	} else {
		emit_sb(REG_T0, REG_SP, dst_offset + VARIANT_DATA_OFFSET);
	}
}

void RISCVCodeGen::emit_folded_initializers(const IRProgram& program, bool members) {
	for (size_t i = 0; i < program.globals.size(); i++) {
		const auto& global = program.globals[i];

		if (global.init_type == IRGlobalVar::InitType::NONE ||
		    global.init_type == IRGlobalVar::InitType::RUNTIME) {
			continue;
		}
		if (global.is_member() != members) {
			continue;
		}

		emit_address_of_global(REG_T0, i);

		if (global.init_type == IRGlobalVar::InitType::INT) {
			emit_li(REG_T1, Variant::INT);
			emit_sw(REG_T1, REG_T0, 0);

			int64_t value = std::get<int64_t>(global.init_value);
			emit_li(REG_T1, value);
			emit_sd(REG_T1, REG_T0, 8);
		} else if (global.init_type == IRGlobalVar::InitType::FLOAT) {
			emit_li(REG_T1, Variant::FLOAT);
			emit_sw(REG_T1, REG_T0, 0);

			double value = std::get<double>(global.init_value);
			int64_t bits;
			memcpy(&bits, &value, sizeof(double));
			emit_li(REG_T1, bits);
			emit_sd(REG_T1, REG_T0, 8);
		} else if (global.init_type == IRGlobalVar::InitType::BOOL) {
			emit_li(REG_T1, Variant::BOOL);
			emit_sw(REG_T1, REG_T0, 0);

			bool value = std::get<bool>(global.init_value);
			emit_li(REG_T1, value ? 1 : 0);
			emit_sd(REG_T1, REG_T0, 8);
		} else if (global.init_type == IRGlobalVar::InitType::STRING) {
			std::string str_value = std::get<std::string>(global.init_value);
			int str_len = static_cast<int>(str_value.length());

			int str_space = ((str_len + 1) + 7) & ~7;
			int struct_space = 16; // { char*, size_t }
			int total_space = (str_space + struct_space + 15) & ~15;

			emit_add_offset(REG_SP, REG_SP, -total_space);

			for (size_t j = 0; j < str_value.length(); j++) {
				emit_li(REG_T2, static_cast<unsigned char>(str_value[j]));
				emit_sb(REG_T2, REG_SP, j);
			}
			emit_sb(REG_ZERO, REG_SP, str_len);

			emit_mv(REG_T2, REG_SP);
			emit_sd(REG_T2, REG_SP, str_space);
			emit_li(REG_T2, str_len);
			emit_sd(REG_T2, REG_SP, str_space + 8);
			emit_add_offset(REG_T1, REG_SP, str_space);

			// t0 is an absolute address from emit_address_of_global; VCREATE inline
			emit_mv(REG_A0, REG_T0);
			emit_li(REG_A1, Variant::STRING);
			emit_li(REG_A2, 1); // method 1: { char*, size_t }
			emit_mv(REG_A3, REG_T1);
			emit_li(REG_A7, ECALL_VCREATE);
			emit_ecall();

			emit_add_offset(REG_SP, REG_SP, total_space);
		} else if (global.init_type == IRGlobalVar::InitType::EMPTY_ARRAY) {
			emit_mv(REG_A0, REG_T0);
			emit_li(REG_A1, Variant::ARRAY);
			emit_li(REG_A2, 0);
			emit_li(REG_A3, 0);
			emit_li(REG_A7, ECALL_VCREATE);
			emit_ecall();
		} else if (global.init_type == IRGlobalVar::InitType::EMPTY_DICT) {
			emit_mv(REG_A0, REG_T0);
			emit_li(REG_A1, Variant::DICTIONARY);
			emit_li(REG_A2, 0);
			emit_li(REG_A3, 0);
			emit_li(REG_A7, ECALL_VCREATE);
			emit_ecall();
		} else if (global.init_type == IRGlobalVar::InitType::NULL_VAL) {
			emit_li(REG_T1, Variant::NIL);
			emit_sw(REG_T1, REG_T0, 0);
		} else {
			throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Global variable '" + global.name + "': Unknown initialization type.");
		}

	}
}

std::vector<uint8_t> RISCVCodeGen::generate(const IRProgram& program) {
	m_code.clear();
	m_label_names = program.strings;
	m_label_offsets.assign(m_label_names.size(), NO_LABEL);
	m_label_uses.clear();
	m_functions.clear();
	m_scoped_clean_functions.clear();
	m_trusted_internal_entries.clear();
	m_fn = FunctionState {};
	m_constant_pool.clear();
	m_constant_pool_map.clear();
	m_rodata_strings.clear();
	m_rodata_string_labels.clear();
	m_string_constants = &program.string_constants;
	m_strings = &program.strings;
	m_trait_signatures = &program.trait_signatures;
	m_trait_structural_fallback = program.trait_structural_fallback;
	m_trait_cache_count = program.trait_signatures.size();
	m_trait_cache_size = m_trait_cache_count * TraitCacheLayout::AREA_SIZE;
	m_trait_cache_address = 0;
	plan_program_call_properties(program);

	m_global_count = program.globals.size();
	m_globals = program.globals;
	m_global_address = 0;

	m_global_slots.assign(m_global_count, 0);
	m_data_global_count = 0;
	m_instance_count = 0;
	for (size_t i = 0; i < m_global_count; i++) {
		m_global_slots[i] = m_globals[i].is_member() ? m_instance_count++ : m_data_global_count++;
	}
	m_instance_blob_address = 0;
	m_instance_blob_size = 0;
	m_instance_init_offset = 0;
	m_profiling_count = program.functions.size();
	m_profiling_address = 0;
	m_profiling_size = 0;
	m_profiling_index = -1;
	m_debug_address = 0;
	m_debug_size = 0;
	m_debug_index = -1;
	m_line_table.entries.clear();
	m_installed_breakpoints.clear();
	m_debug_variables.clear();
	m_pending_debug_variables.clear();

	if (m_instance_count > 0) {
		emit_la(REG_TP, INSTANCE_LABEL);
	}

	emit_folded_initializers(program, false);

	// Runs before properties are registered, so @export gets its declared value.
	// a0 must point at real storage: the scratch slot past .globals.
	if (program.has_global_init) {
		emit_address_of_init_scratch(REG_A0);
		mark_label_use(GLOBAL_INIT_LABEL, m_code.size());
		emit_jal(REG_RA, 0);
	}

	if (emits_instance_init(program)) {
		emit_mv(REG_A0, REG_TP);
		mark_label_use(INSTANCE_INIT_LABEL, m_code.size());
		emit_jal(REG_RA, 0);
	}

	for (size_t i = 0; i < program.globals.size(); i++) {
		const auto& global = program.globals[i];
		if (!global.publishes_to_host()) {
			continue;
		}
		const bool script_variable = !global.is_property;

		const std::string name_label = ".LPROP" + std::to_string(i);
		m_rodata_strings.push_back({global.name, name_label});

		emit_li(REG_A0, 0);
		emit_la(REG_A1, name_label);
		emit_li(REG_A2, static_cast<int64_t>(global.name.length()));
		// NIL = unconstrained Variant
		const int32_t variant_type = global.value_type != IRInstruction::TypeHint_NONE
			? global.value_type
			: static_cast<int32_t>(Variant::NIL);
		emit_li(REG_A3, variant_type);
		if (global.setter_function.empty()) {
			emit_li(REG_A4, 0);
		} else {
			emit_la(REG_A4, global.setter_function);
		}
		if (global.getter_function.empty()) {
			emit_li(REG_A5, 0);
		} else {
			emit_la(REG_A5, global.getter_function);
		}
		emit_address_of_global(REG_A6, i);
		emit_li(REG_A7, ECALL_SANDBOX_ADD);
		emit_ecall();
		if (!global.export_hint.is_default()) {
			const std::string hint_label = ".LHINT" + std::to_string(i);
			m_rodata_strings.push_back({global.export_hint.hint_string, hint_label});

			emit_li(REG_A0, SANDBOX_ADD_PROPERTY_HINT);
			emit_la(REG_A1, name_label);
			emit_li(REG_A2, static_cast<int64_t>(global.name.length()));
			emit_li(REG_A3, global.export_hint.hint);
			emit_la(REG_A4, hint_label);
			emit_li(REG_A5, static_cast<int64_t>(global.export_hint.hint_string.length()));
			emit_li(REG_A6, global.export_hint.usage);
			emit_li(REG_A7, ECALL_SANDBOX_ADD);
			emit_ecall();
		} else if (script_variable) {
			emit_li(REG_A0, SANDBOX_ADD_PROPERTY_HINT);
			emit_la(REG_A1, name_label);
			emit_li(REG_A2, static_cast<int64_t>(global.name.length()));
			emit_li(REG_A3, 0);
			emit_la(REG_A4, name_label);
			emit_li(REG_A5, 0);
			emit_li(REG_A6, USAGE_SCRIPT_VARIABLE);
			emit_li(REG_A7, ECALL_SANDBOX_ADD);
			emit_ecall();
		}
	}

	// STOP: SYSTEM with imm = 0x7ff
	emit_i_type(0x73, 0, 0, 0, 0x7ff);

	// Not exported; no signature. Forced line-0 row caps the previous function.
	if (program.has_global_init) {
		set_label(label_id(GLOBAL_INIT_LABEL), m_code.size());
		m_profiling_index = -1;
		m_debug_index = -1;
		record_line(0, true);
		gen_function(program.global_init);
	}

	if (emits_instance_init(program)) {
		emit_instance_init(program);
	}

	if (program.has_member_init) {
		set_label(label_id(MEMBER_INIT_LABEL), m_code.size());
		m_profiling_index = -1;
		m_debug_index = -1;
		record_line(0, true);
		gen_function(program.member_init);
	}

	for (size_t i = 0; i < program.functions.size(); i++) {
		const auto& func = program.functions[i];
		m_functions[func.name] = m_code.size();
		set_label(label_id(func.name), m_code.size());
		m_profiling_index = m_profiling ? int(i) : -1;
		m_debug_index = m_debug ? int(i) : -1;
		// Prologue line = declaration, not the first statement.
		record_line(i < program.signatures.size() ? program.signatures[i].line : 0, true);
		gen_function(func);
	}
	m_profiling_index = -1;
	m_debug_index = -1;

	// Must run before constant pool / data is appended: relaxation inserts instructions.
	relax_branches();

	size_t const_pool_base = m_code.size();
	for (size_t i = 0; i < m_constant_pool.size(); i++) {
		std::string label = ".LC" + std::to_string(i);
		set_label(label_id(label), const_pool_base + (i * 8));
	}

	for (int64_t constant : m_constant_pool) {
		for (int i = 0; i < 8; i++) {
			m_code.push_back(static_cast<uint8_t>((constant >> (i * 8)) & 0xFF));
		}
	}

	for (const auto& [str, label] : m_rodata_strings) {
		set_label(label_id(label), m_code.size());
		for (char c : str) {
			m_code.push_back(static_cast<uint8_t>(c));
		}
		m_code.push_back(0);
	}
	while (m_code.size() % 8 != 0) {
		m_code.push_back(0);
	}

	const bool wants_scratch = program.has_global_init || program.has_member_init;
	const size_t data_slots = m_data_global_count + (wants_scratch ? 1 : 0);
	const size_t global_slots = data_slots + m_instance_count;
	const size_t globals_bytes = global_slots > 0 ? global_slots * variant_size() : 0;
	m_instance_blob_size = m_instance_count > 0 ? size_t(InstanceLayout::BLOB_SIZE) : 0;
	m_profiling_size = m_profiling ? size_t(ProfilingLayout::area_size(uint32_t(m_profiling_count))) : 0;
	m_debug_size = m_debug ? size_t(DebugLayout::area_size()) : 0;
	m_global_data_size = globals_bytes + m_instance_blob_size + m_profiling_size +
		m_debug_size + m_trait_cache_size;

	if (m_global_data_size > 0) {
		while (m_code.size() % 8 != 0) {
			m_code.push_back(0);
		}

		// .data vaddr = BASE_ADDR + text_size, page-aligned.
		// Label stores vaddr - BASE_ADDR because resolve_labels() adds BASE_ADDR.
		size_t text_size = m_code.size();
		size_t data_vaddr = 0x10000 + text_size;
		data_vaddr = (data_vaddr + 0xFFF) & ~0xFFF;

		if (global_slots > 0) {
			m_global_address = data_vaddr;
			set_label(label_id(GLOBALS_LABEL), data_vaddr - 0x10000);
			set_label(label_id(INSTANCE_LABEL), data_vaddr - 0x10000 + data_slots * variant_size());

			// Payload = INT32_MIN, not 0: VASSIGN's "adopt source" path requires
			// INT32_MIN as the sentinel. 0 is a valid scoped-variant index.
			for (size_t i = 0; i < global_slots; i++) {
				for (int j = 0; j < variant_size(); j++) {
					m_code.push_back(0);
				}
				const int64_t empty_index = static_cast<int64_t>(INT32_MIN);
				const size_t payload = m_code.size() - variant_size() + VARIANT_DATA_OFFSET;
				for (int j = 0; j < 8; j++) {
					m_code[payload + j] = static_cast<uint8_t>((empty_index >> (j * 8)) & 0xFF);
				}
			}
		}

		if (m_instance_blob_size > 0) {
			m_instance_blob_address = data_vaddr + globals_bytes;
			set_label(label_id(INSTANCE_BLOB_LABEL), m_instance_blob_address - 0x10000);

			const size_t base = m_code.size();
			m_code.resize(base + m_instance_blob_size, 0);

			const auto put32 = [&](int32_t offset, uint32_t value) {
				for (int j = 0; j < 4; j++) {
					m_code[base + offset + j] = static_cast<uint8_t>((value >> (j * 8)) & 0xFF);
				}
			};
			const auto put64 = [&](int32_t offset, uint64_t value) {
				for (int j = 0; j < 8; j++) {
					m_code[base + offset + j] = static_cast<uint8_t>((value >> (j * 8)) & 0xFF);
				}
			};
			put32(InstanceLayout::MAGIC_OFF, InstanceLayout::MAGIC);
			put32(InstanceLayout::VERSION_OFF, InstanceLayout::LAYOUT_VERSION);
			put64(InstanceLayout::DEFAULT_BASE_OFF, data_vaddr + data_slots * variant_size());
			put32(InstanceLayout::RECORD_SIZE_OFF, uint32_t(m_instance_count * variant_size()));
			put32(InstanceLayout::MEMBER_COUNT_OFF, uint32_t(m_instance_count));
		}

		if (m_profiling) {
			m_profiling_address = data_vaddr + globals_bytes + m_instance_blob_size;
			set_label(label_id(PROFILING_LABEL), m_profiling_address - 0x10000);

			const size_t base = m_code.size();
			m_code.resize(base + m_profiling_size, 0);

			const auto put32 = [&](int32_t offset, uint32_t value) {
				for (int j = 0; j < 4; j++) {
					m_code[base + offset + j] = static_cast<uint8_t>((value >> (j * 8)) & 0xFF);
				}
			};
			put32(ProfilingLayout::MAGIC_OFF, ProfilingLayout::MAGIC);
			put32(ProfilingLayout::VERSION_OFF, ProfilingLayout::LAYOUT_VERSION);
			put32(ProfilingLayout::FUNCTION_COUNT_OFF, uint32_t(m_profiling_count));
			put32(ProfilingLayout::RECORD_SIZE_OFF, uint32_t(ProfilingLayout::RECORD_SIZE));
			put32(ProfilingLayout::MAX_DEPTH_OFF, ProfilingLayout::MAX_DEPTH);
			put32(ProfilingLayout::CLOCK_OFF, uint32_t(m_profiling_clock));
		}

		if (m_debug) {
			m_debug_address = data_vaddr + globals_bytes + m_instance_blob_size + m_profiling_size;
			set_label(label_id(DEBUG_LABEL), m_debug_address - 0x10000);

			const size_t base = m_code.size();
			m_code.resize(base + m_debug_size, 0);

			const auto put32 = [&](int32_t offset, uint32_t value) {
				for (int j = 0; j < 4; j++) {
					m_code[base + offset + j] = static_cast<uint8_t>((value >> (j * 8)) & 0xFF);
				}
			};
			put32(DebugLayout::MAGIC_OFF, DebugLayout::MAGIC);
			put32(DebugLayout::VERSION_OFF, DebugLayout::LAYOUT_VERSION);
			put32(DebugLayout::FUNCTION_COUNT_OFF, uint32_t(program.functions.size()));
			put32(DebugLayout::FRAME_SIZE_OFF, uint32_t(DebugLayout::FRAME_SIZE));
			put32(DebugLayout::MAX_DEPTH_OFF, DebugLayout::MAX_DEPTH);

			// Globals and members are live for the whole program. Offsets are
			// relative to the exported global area or the frame's instance base.
			for (size_t i = 0; i < m_globals.size(); i++) {
				if (m_globals[i].is_const) continue;
				DebugVariableRecord record;
				record.function_index = UINT32_MAX;
				record.name = m_globals[i].name;
				record.type = m_globals[i].value_type == IRInstruction::TypeHint_NONE
						? -1 : int32_t(m_globals[i].value_type);
				record.storage = m_globals[i].is_member() ? DebugStorage::MEMBER : DebugStorage::GLOBAL;
				record.offset = int32_t(m_global_slots[i] * variant_size());
				record.pc_begin = 0x10000;
				record.pc_end = data_vaddr;
				m_debug_variables.push_back(std::move(record));
			}
		}

		if (m_trait_cache_size > 0) {
			const size_t caches_address = data_vaddr + globals_bytes + m_instance_blob_size +
				m_profiling_size + m_debug_size;
			m_trait_cache_address = caches_address;
			for (size_t i = 0; i < program.trait_signatures.size(); i++) {
				set_label(label_id(".trait_cache_" + std::to_string(i)),
					caches_address - 0x10000 + i * TraitCacheLayout::AREA_SIZE);
			}
			m_code.resize(m_code.size() + m_trait_cache_size, 0);
		}
	}

	resolve_labels();
	for (PendingDebugVariable &pending : m_pending_debug_variables) {
		const size_t begin = m_label_offsets[label_id(pending.begin_label)];
		const size_t end = m_label_offsets[label_id(pending.end_label)];
		pending.record.pc_begin = 0x10000 + begin;
		pending.record.pc_end = 0x10000 + std::max(begin, end);
		m_debug_variables.push_back(std::move(pending.record));
	}

	return m_code;
}

// Sizes the frame from the instructions, not the declaration.
void RISCVCodeGen::plan_frame(const IRFunction& func) {
	m_fn.num_params = func.parameters.size();
	m_allocator.init(func);
	m_fn.in_function = true;
	m_fn.known_tags.assign(size_t(std::max(func.max_registers, 0)), IRInstruction::TypeHint_NONE);
	m_fn.int_cache_slots.assign(m_fn.known_tags.size(), int8_t(-1));
	m_fn.float_cache_slots.assign(m_fn.known_tags.size(), int8_t(-1));

	// Leaf functions keep ra; functions with no syscall keep the return pointer in a0.
	for (const auto& instr : func.instructions) {
		if (opcode_clobbers_abi_registers(instr.opcode)) {
			m_fn.spills_return_pointer = true;
		}
		if (instr.opcode == IROpcode::CALL) {
			m_fn.saves_return_address = true;
		}
	}
	m_fn.is_coroutine = func.is_coroutine;
	if (m_fn.is_coroutine) {
		// Suspension clobbers a0; resume needs ra. Frame mandatory.
		m_fn.spills_return_pointer = true;
		m_fn.saves_return_address = true;
		m_fn.resume_label = ".resume$" + func.name;
		m_fn.await_states.clear();
	}
	m_fn.forward_to_return = find_return_forwarding(func);
	m_fn.live_params = find_live_parameters(func);
	if (m_fn.is_coroutine || m_debug) {
		// Parameters are caller pointers; force all live so they are in the frame before suspension.
		m_fn.live_params.assign(func.parameters.size(), true);
	}
	plan_constants(func);
	plan_int_chaining(func);
	plan_nonnegative(func);
	plan_global_handles(func);
	const bool copies_a_parameter =
		std::find(m_fn.live_params.begin(), m_fn.live_params.end(), true) != m_fn.live_params.end();

	// A function that saves nothing, copies no parameter and writes everything
	// through a0 needs no frame. Determined by the prologue's writes, not the
	// declaration. get_variant_stack_offset() refuses after this, so a missing
	// frame is a compile error rather than a silent caller-frame corruption.
	m_fn.omits_frame = !m_fn.saves_return_address && !m_fn.spills_return_pointer &&
		!copies_a_parameter && !m_fn.is_coroutine;
	for (size_t i = 0; m_fn.omits_frame && i < func.instructions.size(); i++) {
		switch (func.instructions[i].opcode) {
			case IROpcode::LABEL:
			case IROpcode::JUMP:
				break;
			case IROpcode::RETURN:
				m_fn.omits_frame = func.max_registers == 0 ||
					(i > 0 && m_fn.forward_to_return[i - 1]);
				break;
			case IROpcode::LOAD_IMM:
			case IROpcode::LOAD_FLOAT_IMM:
			case IROpcode::LOAD_BOOL:
			case IROpcode::LOAD_NIL:
			case IROpcode::LOAD_GLOBAL:
				m_fn.omits_frame = m_fn.forward_to_return[i];
				break;
			default:
				m_fn.omits_frame = false;
				break;
		}
	}

	plan_scopes(func);
	plan_release_clears(func);
	if (m_fn.scope_slot_count > 0) {
		m_fn.omits_frame = false;
	}

	const int saved_reg_space = SAVED_REG_SPACE;

	// Offsets are by vreg number, not visit order, so optimizer reordering is safe.
	int max_variants = func.max_registers;
	// Scratch slots for temporaries (immediate operands, comparison results).
	int variant_space = (max_variants + SCRATCH_VARIANT_SLOTS) * variant_size();

	for (int vreg = 0; vreg < max_variants; vreg++) {
		int offset = saved_reg_space + (vreg * variant_size());
		m_fn.variant_offsets[vreg] = offset;
	}
	m_fn.scratch_slot_base = max_variants;
	m_fn.next_variant_slot = max_variants + SCRATCH_VARIANT_SLOTS;
	m_fn.variant_space = variant_space;

	m_fn.scope_slot_base = saved_reg_space + variant_space;

	m_fn.stack_frame_size = m_fn.omits_frame
		? 0
		: saved_reg_space + variant_space + m_fn.scope_slot_count * 8;

	m_fn.stack_frame_size = (m_fn.stack_frame_size + 15) & ~15; // RISC-V ABI: 16-byte aligned

}

void RISCVCodeGen::emit_prologue(const IRFunction& func,
	const std::vector<int>* parameter_destinations) {
	// Before frame setup; uses no stack so frameless functions work.
	if (m_profiling_index >= 0) {
		emit_profiling_entry();
	}

	if (m_fn.stack_frame_size > 0) {
		emit_add_offset(REG_SP, REG_SP, -m_fn.stack_frame_size);
	}

	// After frame setup (sp valid for locals), before ra is spilled.
	if (m_debug_index >= 0) {
		emit_debug_entry();
	}

	if (m_fn.saves_return_address) {
		emit_sd(REG_RA, REG_SP, SAVED_RA_OFFSET);
	}

	if (m_fn.spills_return_pointer) {
		emit_sd(REG_A0, REG_SP, SAVED_A0_OFFSET);
	}

	if (m_fn.is_coroutine || m_fn.scope_slot_count > 0) {
		emit_zero_variant_slots();
	}

	// Parameters arrive as pointers to Variants: first in a1-a7, then in
	// pointer-sized slots at the caller's sp. The frame was allocated above, so
	// a stacked pointer is reached past this function's whole frame.
	if (m_fn.num_params > IRFunction::MAX_PARAMETERS) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"Function '" + func.name + "' has " + std::to_string(m_fn.num_params) +
			" parameters, but the SafeGDScript ABI supports at most " +
			std::to_string(IRFunction::MAX_PARAMETERS));
	}
	for (size_t i = 0; i < m_fn.num_params; i++) {
		if (!m_fn.live_params[i]) {
			continue; // Dead on entry; slot stays reserved for later writes.
		}
		int param_vreg = parameter_destinations != nullptr
			? parameter_destinations->at(i)
			: static_cast<int>(i);
		int dst_offset = get_variant_stack_offset(param_vreg);
		int parameter_type = IRInstruction::TypeHint_NONE;
		for (const IRFunction::DebugLocal& local : func.debug_locals) {
			if (local.parameter && local.name == func.parameters[i]) {
				parameter_type = local.type_hint;
				break;
			}
		}
		if (i < IRFunction::REGISTER_PARAMETERS) {
			uint8_t arg_reg = REG_A1 + static_cast<uint8_t>(i);
			if (is_scalar_variant_type(parameter_type)) {
				emit_scalar_variant_move(REG_SP, dst_offset, arg_reg, 0, REG_T0);
			} else {
				emit_variant_move(REG_SP, dst_offset, arg_reg, 0, REG_T0);
			}
		} else {
			const int32_t incoming_offset = m_fn.stack_frame_size + int32_t(
				(i - IRFunction::REGISTER_PARAMETERS) * CallABI::STACK_SLOT_SIZE);
			emit_ld(REG_T1, REG_SP, incoming_offset);
			if (is_scalar_variant_type(parameter_type)) {
				emit_scalar_variant_move(REG_SP, dst_offset, REG_T1, 0, REG_T0);
			} else {
				emit_variant_move(REG_SP, dst_offset, REG_T1, 0, REG_T0);
			}
		}
		if (is_scalar_variant_type(parameter_type)) note_known_tag(param_vreg, parameter_type);
	}
}

void RISCVCodeGen::emit_zero_variant_slots() {
	if (m_fn.stack_frame_size == 0) {
		return;
	}
	const int vsize = variant_size();
	// sp-relative while within 12-bit range; t0-based past that.
	int base = 0;
	bool based = false;
	auto store_zero = [&](int offset, bool wide) {
		if (!based && offset <= 2047) {
			if (wide) {
				emit_sd(REG_ZERO, REG_SP, offset);
			} else {
				emit_sw(REG_ZERO, REG_SP, offset);
			}
			return;
		}
		if (!based || offset - base > 2047) {
			base = offset;
			emit_add_offset(REG_T0, REG_SP, base);
			based = true;
		}
		if (wide) {
			emit_sd(REG_ZERO, REG_T0, offset - base);
		} else {
			emit_sw(REG_ZERO, REG_T0, offset - base);
		}
	};
	for (int i = 0; i < m_fn.next_variant_slot; i++) {
		store_zero(SAVED_REG_SPACE + i * vsize, false);
	}
	for (int i = 0; i < m_fn.scope_slot_count; i++) {
		store_zero(m_fn.scope_slot_base + i * 8, true);
	}
}

// Destination for a result: the vreg's frame slot, or *a0 when forwarding to RETURN.
// Asking commits to forwarding; only the expansion about to write may ask.
std::pair<uint8_t, int> RISCVCodeGen::value_destination(int vreg) {
	if (m_fn.forward_return) {
		emit_load_return_pointer();
		m_fn.return_value_written = true;
		return { REG_A0, 0 };
	}
	return { REG_SP, get_variant_stack_offset(vreg) };
}

void RISCVCodeGen::gen_syscall_get_obj(const IRInstruction& instr, int result_vreg) {
	if (instr.operands.size() != 4) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "ECALL_GET_OBJ requires 4 operands");
	}

	int string_idx = static_cast<int>(instr.operands[2].immediate());
	int string_len = static_cast<int>(instr.operands[3].immediate());

	if (string_idx < 0 || static_cast<size_t>(string_idx) >= m_string_constants->size()) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "String constant index out of range");
	}
	const std::string& str = (*m_string_constants)[string_idx];

	int result_offset = get_variant_stack_offset(result_vreg);

	spill_around_syscall({REG_A0, REG_A1});

	emit_la(REG_A0, rodata_string(str));
	emit_li(REG_A1, string_len);
	emit_li(REG_A7, ECALL_GET_OBJ);
	emit_ecall();

	emit_syscall_result(result_vreg, REG_A0, result_offset, 24); // OBJECT
}

void RISCVCodeGen::gen_syscall_node_create(const IRInstruction& instr, int result_vreg) {
	if (instr.operands.size() != 4) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "ECALL_NODE_CREATE requires 4 operands");
	}

	const int string_idx = static_cast<int>(instr.operands[2].immediate());
	const int string_len = static_cast<int>(instr.operands[3].immediate());

	if (string_idx < 0 || static_cast<size_t>(string_idx) >= m_string_constants->size()) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "String constant index out of range");
	}
	const std::string& str = (*m_string_constants)[string_idx];

	const int result_offset = get_variant_stack_offset(result_vreg);

	spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3, REG_A4});

	emit_li(REG_A0, static_cast<int64_t>(Node_Create_Shortlist::CREATE_CLASSDB));
	emit_la(REG_A1, rodata_string(str));
	emit_li(REG_A2, string_len);
	emit_li(REG_A3, 0);
	emit_li(REG_A4, 0);
	emit_li(REG_A7, ECALL_NODE_CREATE);
	emit_ecall();

	emit_syscall_result(result_vreg, REG_A0, result_offset, 24); // OBJECT
}

// ECALL_CLASS_BIND: a0 = the instance Dictionary's scoped index, a1/a2 = class
// name. Answers nothing; the result slot is only defined so a later spill of it
// reads a Variant rather than whatever the frame held.
void RISCVCodeGen::gen_syscall_class_bind(const IRInstruction& instr, int result_vreg) {
	if (instr.operands.size() != 5) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "ECALL_CLASS_BIND requires 5 operands");
	}

	const int string_idx = static_cast<int>(instr.operands[2].immediate());
	const int string_len = static_cast<int>(instr.operands[3].immediate());
	const int dict_vreg = instr.operands[4].reg_index();

	if (string_idx < 0 || static_cast<size_t>(string_idx) >= m_string_constants->size()) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "String constant index out of range");
	}
	const std::string& str = (*m_string_constants)[string_idx];

	const int result_offset = get_variant_stack_offset(result_vreg);
	const int dict_offset = get_variant_stack_offset(dict_vreg);

	spill_around_syscall({REG_A0, REG_A1, REG_A2});

	emit_container_handle(REG_A0, dict_vreg, dict_offset);
	emit_la(REG_A1, rodata_string(str));
	emit_li(REG_A2, string_len);
	emit_li(REG_A7, ECALL_CLASS_BIND);
	emit_ecall();

	emit_li(REG_T0, Variant::NIL);
	emit_store_variant_type(REG_T0, REG_SP, result_offset);
}

void RISCVCodeGen::gen_syscall_array_size(const IRInstruction& instr, int result_vreg) {
	if (instr.operands.size() != 3) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "ECALL_ARRAY_SIZE requires 3 operands");
	}

	int array_vreg = static_cast<int>(instr.operands[2].reg_index());

	// Slot allocation before spill_around_syscall to prevent allocator drift.
	int result_offset = get_variant_stack_offset(result_vreg);
	int array_offset = get_variant_stack_offset(array_vreg);

	spill_around_syscall({REG_A0});

	emit_container_handle(REG_A0, array_vreg, array_offset);
	emit_li(REG_A7, ECALL_ARRAY_SIZE);
	emit_ecall();

	emit_syscall_result(result_vreg, REG_A0, result_offset, 2); // INT
}

// ECALL_STRING_SIZE: handle in a0, length returned in a0.
void RISCVCodeGen::gen_syscall_string_size(const IRInstruction& instr, int result_vreg) {
	if (instr.operands.size() != 3) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "ECALL_STRING_SIZE requires 3 operands");
	}

	int string_vreg = static_cast<int>(instr.operands[2].reg_index());

	// Slot allocation before spill_around_syscall to prevent allocator drift.
	int result_offset = get_variant_stack_offset(result_vreg);
	int string_offset = get_variant_stack_offset(string_vreg);

	spill_around_syscall({REG_A0});

	emit_container_handle(REG_A0, string_vreg, string_offset);
	emit_li(REG_A7, ECALL_STRING_SIZE);
	emit_ecall();

	emit_syscall_result(result_vreg, REG_A0, result_offset, 2); // INT
}

void RISCVCodeGen::gen_syscall_array_at(const IRInstruction& instr, int result_vreg) {
	if (instr.operands.size() != 4) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "ECALL_ARRAY_AT requires 4 operands");
	}

	int array_vreg = static_cast<int>(instr.operands[2].reg_index());
	int index_vreg = static_cast<int>(instr.operands[3].reg_index());

	int result_offset = get_variant_stack_offset(result_vreg);
	int array_offset = get_variant_stack_offset(array_vreg);
	int index_offset = get_variant_stack_offset(index_vreg);

	spill_around_syscall({REG_A0, REG_A1, REG_A2});

	emit_container_handle(REG_A0, array_vreg, array_offset);
	emit_ld(REG_A1, REG_SP, index_offset + 8); // int64, not int32
	emit_load_stack_offset(REG_A2, result_offset);
	emit_li(REG_A7, ECALL_ARRAY_AT);
	emit_ecall();
}

void RISCVCodeGen::gen_syscall_string_at(const IRInstruction& instr, int result_vreg) {
	if (instr.operands.size() != 4) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "ECALL_STRING_AT requires 4 operands");
	}

	int string_vreg = static_cast<int>(instr.operands[2].reg_index());
	int index_vreg = static_cast<int>(instr.operands[3].reg_index());

	int result_offset = get_variant_stack_offset(result_vreg);
	int string_offset = get_variant_stack_offset(string_vreg);
	int index_offset = get_variant_stack_offset(index_vreg);

	spill_around_syscall({REG_A0, REG_A1});

	emit_container_handle(REG_A0, string_vreg, string_offset);
	emit_ld(REG_A1, REG_SP, index_offset + 8); // int64, not int32
	emit_li(REG_A7, ECALL_STRING_AT);
	emit_ecall();

	emit_syscall_result(result_vreg, REG_A0, result_offset, Variant::STRING);
}

// ECALL_VARIANT_GET: a0 = subject GuestVariant*, a1 = key GuestVariant*,
// a2 = result GuestVariant*. The host delegates the complete `[]` decision to
// Godot, preserving non-integer keys and built-in indexed semantics.
void RISCVCodeGen::gen_syscall_variant_get(const IRInstruction& instr, int result_vreg) {
	if (instr.operands.size() != 4) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
				"ECALL_VARIANT_GET requires 4 operands");
	}

	const int subject_vreg = instr.operands[2].reg_index();
	const int key_vreg = instr.operands[3].reg_index();
	const int result_offset = get_variant_stack_offset(result_vreg);
	const int subject_offset = get_variant_stack_offset(subject_vreg);
	const int key_offset = get_variant_stack_offset(key_vreg);

	spill_around_syscall({REG_A0, REG_A1, REG_A2});

	emit_load_stack_offset(REG_A0, subject_offset);
	emit_load_stack_offset(REG_A1, key_offset);
	emit_load_stack_offset(REG_A2, result_offset);
	emit_li(REG_A7, ECALL_VARIANT_GET);
	emit_ecall();
}

// a0 = string, a1 = first character, a2 = how many at most (an immediate: the
// batch size is fixed at the call site). The answer is one register, packing the
// first scoped index and the count, so nothing is written through a pointer.
void RISCVCodeGen::gen_syscall_string_batch(const IRInstruction& instr, int result_vreg) {
	if (instr.operands.size() != 5) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "ECALL_STRING_BATCH requires 5 operands");
	}

	int string_vreg = static_cast<int>(instr.operands[2].reg_index());
	int index_vreg = static_cast<int>(instr.operands[3].reg_index());
	const int64_t max_count = instr.operands[4].immediate();

	int result_offset = get_variant_stack_offset(result_vreg);
	int string_offset = get_variant_stack_offset(string_vreg);
	int index_offset = get_variant_stack_offset(index_vreg);

	spill_around_syscall({ REG_A0, REG_A1, REG_A2 });

	emit_container_handle(REG_A0, string_vreg, string_offset);
	emit_ld(REG_A1, REG_SP, index_offset + 8); // int64, not int32
	emit_li(REG_A2, max_count);
	emit_li(REG_A7, ECALL_STRING_BATCH);
	emit_ecall();

	emit_syscall_result(result_vreg, REG_A0, result_offset, Variant::INT);
}

// Keyed ops pass key in a2, result in a3; keyless (GET_KEYS, GET_VALUES) take result in a2.
// HAS and GET_SIZE return in a0 instead of through a pointer.
void RISCVCodeGen::gen_syscall_dictionary_ops(const IRInstruction& instr, int result_vreg) {
	if (instr.operands.size() != 4 && instr.operands.size() != 5) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "ECALL_DICTIONARY_OPS requires 4 or 5 operands");
	}

	constexpr int64_t DICT_OP_HAS = 3;
	constexpr int64_t DICT_OP_GET_SIZE = 6;

	const int64_t dict_op = instr.operands[2].immediate();
	int dict_vreg = static_cast<int>(instr.operands[3].reg_index());
	const bool has_key = instr.operands.size() == 5;
	const bool returns_in_register = dict_op == DICT_OP_HAS || dict_op == DICT_OP_GET_SIZE;

	int result_offset = get_variant_stack_offset(result_vreg);
	int dict_offset = get_variant_stack_offset(dict_vreg);
	int key_offset = has_key
		? get_variant_stack_offset(static_cast<int>(instr.operands[4].reg_index()))
		: 0;

	std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2};
	if (has_key) {
		clobbered_regs.push_back(REG_A3);
	}
	spill_around_syscall(clobbered_regs);

	emit_li(REG_A0, dict_op);
	emit_container_handle(REG_A1, dict_vreg, dict_offset); // scoped index
	if (has_key) {
		emit_load_stack_offset(REG_A2, key_offset);
		emit_load_stack_offset(REG_A3, result_offset);
	} else {
		emit_load_stack_offset(REG_A2, result_offset);
	}

	emit_li(REG_A7, ECALL_DICTIONARY_OPS);
	emit_ecall();

	if (returns_in_register) {
		emit_syscall_result(result_vreg, REG_A0,
			result_offset, dict_op == DICT_OP_HAS ? Variant::BOOL : Variant::INT);
	}
}

// Raw-key Dictionary operations.  Unlike the ordinary keyed syscall, the key
// is a UTF-8 view in rodata and a4 carries the value/result GuestVariant.
void RISCVCodeGen::gen_dict_const(const IRInstruction& instr) {
	const bool is_get = instr.opcode == IROpcode::DICT_GET_CONST;
	const bool is_set = instr.opcode == IROpcode::DICT_SET_CONST;
	const bool has_result = !is_set;
	const int dict_operand = has_result ? 1 : 0;
	const int key_operand = has_result ? 2 : 1;
	const int value_operand = is_set ? 2 : 0;
	const int dict_vreg = instr.operands[dict_operand].reg_index();
	const int string_idx = int(instr.operands[key_operand].immediate());
	if (string_idx < 0 || size_t(string_idx) >= m_string_constants->size()) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"Dictionary constant key index out of range");
	}
	const std::string &key = (*m_string_constants)[size_t(string_idx)];
	const int dict_offset = get_variant_stack_offset(dict_vreg);
	const int value_offset = get_variant_stack_offset(instr.operands[value_operand].reg_index());

	spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3, REG_A4});
	emit_li(REG_A0, dictionary_op(is_get ? Dictionary_Op::GET_RAW
		: is_set ? Dictionary_Op::SET_RAW : Dictionary_Op::HAS_RAW));
	emit_container_handle(REG_A1, dict_vreg, dict_offset);
	emit_la(REG_A2, rodata_string(key));
	emit_li(REG_A3, int64_t(key.size()));
	if (is_get || is_set) {
		emit_add_offset(REG_A4, REG_SP, value_offset);
	}
	emit_li(REG_A7, ECALL_DICTIONARY_OPS);
	emit_ecall();

	if (!is_set && !is_get) {
		emit_syscall_result(instr.operands[0].reg_index(), REG_A0, value_offset, Variant::BOOL);
	}
}

void RISCVCodeGen::gen_struct_check(const IRInstruction& instr) {
	const int result_vreg = instr.operands[0].reg_index();
	const int dict_vreg = instr.operands[1].reg_index();
	const int count = int(instr.operands[2].immediate());
	const int table_space = (count * 16 + 15) & ~15;
	const int result_offset = get_variant_stack_offset(result_vreg);
	const int dict_offset = get_variant_stack_offset(dict_vreg);

	spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3});
	emit_stack_adjust(-table_space);
	for (int i = 0; i < count; i++) {
		const int string_idx = int(instr.operands[size_t(3 + i)].immediate());
		if (string_idx < 0 || size_t(string_idx) >= m_string_constants->size()) {
			throw CompilerException(ErrorType::RISCV_codegen_ERROR,
				"Struct key index out of range");
		}
		const std::string &key = (*m_string_constants)[size_t(string_idx)];
		emit_la(REG_T0, rodata_string(key));
		emit_sd(REG_T0, REG_SP, i * 16);
		emit_li(REG_T0, int64_t(key.size()));
		emit_sd(REG_T0, REG_SP, i * 16 + 8);
	}
	emit_li(REG_A0, dictionary_op(Dictionary_Op::HAS_EXACT_KEYS));
	emit_container_handle(REG_A1, dict_vreg, dict_offset + table_space);
	emit_mv(REG_A2, REG_SP);
	emit_li(REG_A3, count);
	emit_li(REG_A7, ECALL_DICTIONARY_OPS);
	emit_ecall();
	emit_syscall_result(result_vreg, REG_A0, result_offset + table_space, Variant::BOOL);
	emit_stack_adjust(table_space);
}

void RISCVCodeGen::gen_make_dictionary_keyed(const IRInstruction& instr) {
	const int result_vreg = instr.operands[0].reg_index();
	const int count = int(instr.operands[1].immediate());
	const int key_space = count * 16;
	const int values_space = count * variant_size();
	const int total_space = (key_space + values_space + 15) & ~15;
	const int result_offset = get_variant_stack_offset(result_vreg);

	spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3, REG_A4});
	emit_stack_adjust(-total_space);
	for (int i = 0; i < count; i++) {
		const int string_idx = int(instr.operands[size_t(2 + i * 2)].immediate());
		if (string_idx < 0 || size_t(string_idx) >= m_string_constants->size()) {
			throw CompilerException(ErrorType::RISCV_codegen_ERROR,
				"Keyed Dictionary key index out of range");
		}
		const std::string &key = (*m_string_constants)[size_t(string_idx)];
		emit_la(REG_T0, rodata_string(key));
		emit_sd(REG_T0, REG_SP, i * 16);
		emit_li(REG_T0, int64_t(key.size()));
		emit_sd(REG_T0, REG_SP, i * 16 + 8);

		const int value_vreg = instr.operands[size_t(3 + i * 2)].reg_index();
		const int source_offset = get_variant_stack_offset(value_vreg) + total_space;
		emit_variant_move(REG_SP, key_space + i * variant_size(),
			REG_SP, source_offset, REG_T0);
	}
	emit_li(REG_A0, dictionary_op(Dictionary_Op::MAKE_KEYED));
	emit_add_offset(REG_A1, REG_SP, result_offset + total_space);
	emit_mv(REG_A2, REG_SP);
	emit_li(REG_A3, count);
	emit_add_offset(REG_A4, REG_SP, key_space);
	emit_li(REG_A7, ECALL_DICTIONARY_OPS);
	emit_ecall();
	emit_stack_adjust(total_space);
}

// No path argument defaults to ".".
// ECALL_GET_NODE(base, path_ptr, path_len). Path copied to stack for memview;
// base zero = attached node.
void RISCVCodeGen::gen_get_node(const IRInstruction& instr) {
	if (instr.operands.size() != 2) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "GET_NODE requires 2 operands");
	}

	int result_vreg = instr.operands[0].reg_index();
	const std::string& path = text(instr.operands[1]);

	int result_offset = get_variant_stack_offset(result_vreg);
	spill_around_syscall({REG_A0, REG_A1, REG_A2});

	emit_li(REG_A0, 0);
	emit_la(REG_A1, rodata_string(path));
	emit_li(REG_A2, static_cast<int>(path.size()));
	emit_li(REG_A7, ECALL_GET_NODE);
	emit_ecall();

	emit_syscall_result(result_vreg, REG_A0, result_offset, Variant::OBJECT);
}

// Path copied to stack as characters, same shape as ECALL_GET_NODE.
void RISCVCodeGen::gen_load_resource(const IRInstruction& instr) {
	if (instr.operands.size() != 2) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "LOAD_RESOURCE requires 2 operands");
	}

	int result_vreg = instr.operands[0].reg_index();
	const std::string& path = text(instr.operands[1]);

	int result_offset = get_variant_stack_offset(result_vreg);
	spill_around_syscall({ REG_A0, REG_A1, REG_A2 });

	emit_la(REG_A0, rodata_string(path));
	emit_li(REG_A1, static_cast<int>(path.size()));
	emit_load_stack_offset(REG_A2, result_offset);
	emit_li(REG_A7, ECALL_LOAD);
	emit_ecall();
}

// A0=address, A1=bound Variant, A3=flags. Result is a scoped variant index.
void RISCVCodeGen::gen_make_callable(const IRInstruction& instr) {
	if (instr.operands.size() != 3) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "MAKE_CALLABLE requires 3 operands");
	}

	int result_vreg = instr.operands[0].reg_index();
	const std::string& function_name = text(instr.operands[1]);
	int bound_vreg = instr.operands[2].reg_index();

	int result_offset = get_variant_stack_offset(result_vreg);
	int bound_offset = get_variant_stack_offset(bound_vreg);

	spill_around_syscall({ REG_A0, REG_A1, REG_A2, REG_A3 });

	if (function_name.empty()) {
		emit_li(REG_A0, 0);
	} else {
		emit_la(REG_A0, function_name);
	}
	emit_load_stack_offset(REG_A1, bound_offset);
	emit_li(REG_A2, 0);
	emit_li(REG_A3, ECALL_CALLABLE_VARIANT_ARGS);
	emit_li(REG_A7, ECALL_CALLABLE_CREATE);
	emit_ecall();

	emit_syscall_result(result_vreg, REG_A0, result_offset, Variant::CALLABLE);
}

// A1 = ECALL_LOAD_PATH_IS_VARIANT: A0 is a Variant, not characters.
void RISCVCodeGen::gen_load_resource_var(const IRInstruction& instr) {
	if (instr.operands.size() != 2) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "LOAD_RESOURCE_VAR requires 2 operands");
	}

	int result_vreg = instr.operands[0].reg_index();
	int path_vreg = instr.operands[1].reg_index();

	int result_offset = get_variant_stack_offset(result_vreg);
	int path_offset = get_variant_stack_offset(path_vreg);

	spill_around_syscall({ REG_A0, REG_A1, REG_A2 });

	emit_load_stack_offset(REG_A0, path_offset);
	emit_li(REG_A1, -1);
	emit_load_stack_offset(REG_A2, result_offset);
	emit_li(REG_A7, ECALL_LOAD);
	emit_ecall();
}

void RISCVCodeGen::gen_call_syscall(const IRInstruction& instr) {
	if (instr.operands.size() < 2) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "CALL_SYSCALL requires at least 2 operands (result_reg, syscall_num)");
	}

	int result_vreg = instr.operands[0].reg_index();
	int syscall_num = static_cast<int>(instr.operands[1].immediate());

	if (syscall_num == ECALL_GET_OBJ) {
		gen_syscall_get_obj(instr, result_vreg);
	} else if (syscall_num == ECALL_NODE_CREATE) {
		gen_syscall_node_create(instr, result_vreg);
	} else if (syscall_num == ECALL_CLASS_BIND) {
		gen_syscall_class_bind(instr, result_vreg);
	} else if (syscall_num == ECALL_ARRAY_SIZE) {
		gen_syscall_array_size(instr, result_vreg);
	} else if (syscall_num == ECALL_STRING_SIZE) {
		gen_syscall_string_size(instr, result_vreg);
	} else if (syscall_num == ECALL_ARRAY_AT) {
		gen_syscall_array_at(instr, result_vreg);
	} else if (syscall_num == ECALL_STRING_AT) {
		gen_syscall_string_at(instr, result_vreg);
	} else if (syscall_num == ECALL_VARIANT_GET) {
		gen_syscall_variant_get(instr, result_vreg);
	} else if (syscall_num == ECALL_STRING_BATCH) {
		gen_syscall_string_batch(instr, result_vreg);
	} else if (syscall_num == ECALL_DICTIONARY_OPS) {
		gen_syscall_dictionary_ops(instr, result_vreg);
	} else {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Unknown syscall number: " + std::to_string(syscall_num));
	}
}

void RISCVCodeGen::gen_trait_test(const IRInstruction& instr) {
	if (instr.operands.size() != 3 || m_trait_signatures == nullptr) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"TRAIT_TEST requires destination, object and trait index");
	}
	const int result_vreg = instr.operands[0].reg_index();
	const int object_vreg = instr.operands[1].reg_index();
	const size_t index = size_t(instr.operands[2].immediate());
	if (index >= m_trait_signatures->size()) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"TRAIT_TEST trait index is out of range");
	}
	const ClassSignature& iface = (*m_trait_signatures)[index];
	std::string methods;
	for (const FunctionSignature& method : iface.trait_methods) {
		if (!m_trait_structural_fallback || method.is_static) continue;
		methods += method.name;
		methods.push_back('\0');
	}
	const std::string name_label = rodata_string(iface.name);
	const std::string methods_label = rodata_string(methods);
	const std::string cache_label = ".trait_cache_" + std::to_string(index);
	const std::string miss = gen_local_label(".trait_miss");
	const std::string done = gen_local_label(".trait_done");
	const int result_offset = get_variant_stack_offset(result_vreg);
	const int object_offset = get_variant_stack_offset(object_vreg);

	spill_around_syscall({ REG_A0, REG_A1, REG_A2, REG_A3, REG_A4 });
	emit_ld(REG_T0, REG_SP, object_offset + VARIANT_DATA_OFFSET);
	emit_la(REG_T1, cache_label);
	emit_li(REG_T2, 3);
	emit_srl(REG_T2, REG_T0, REG_T2);
	emit_andi(REG_T2, REG_T2, int32_t(TRAIT_CACHE_ENTRIES - 1));
	emit_slli(REG_T2, REG_T2, 4);
	emit_add(REG_T2, REG_T1, REG_T2);
	emit_ld(REG_T3, REG_T2, 0);
	mark_label_use(miss, m_code.size());
	emit_bne(REG_T3, REG_T0, 0);
	emit_ld(REG_T4, REG_T2, 8);
	mark_label_use(done, m_code.size());
	emit_jal(REG_ZERO, 0);

	define_label(miss);
	emit_mv(REG_A0, REG_T0);
	emit_la(REG_A1, name_label);
	emit_li(REG_A2, int64_t(iface.name.size()));
	emit_la(REG_A3, methods_label);
	emit_li(REG_A4, int64_t(methods.size()));
	emit_li(REG_A7, ECALL_OBJ_USES_TRAIT);
	emit_ecall();
	emit_mv(REG_T4, REG_A0);
	emit_sd(REG_T0, REG_T2, 0);
	emit_sd(REG_T4, REG_T2, 8);

	define_label(done);
	emit_li(REG_T5, Variant::BOOL);
	emit_sw(REG_T5, REG_SP, result_offset + VARIANT_TYPE_OFFSET);
	emit_store_variant_bool(REG_T4, REG_SP, result_offset);
}

void RISCVCodeGen::gen_construct(const IRInstruction& instr) {
	if (instr.operands.size() < 3) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "CONSTRUCT requires at least 3 operands");
	}

	const int result_vreg = instr.operands[0].reg_index();
	const int variant_type = static_cast<int>(instr.operands[1].immediate());
	const int arg_count = static_cast<int>(instr.operands[2].immediate());

	if (instr.operands.size() != static_cast<size_t>(3 + arg_count)) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "CONSTRUCT argument count mismatch");
	}

	const int result_offset = get_variant_stack_offset(result_vreg);

	spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3, REG_A7});

	const int additional_space = arg_count > 0
		? ((arg_count * variant_size()) + 15) & ~15 // Align to 16 bytes
		: 0;
	if (arg_count > 0) {
		emit_stack_adjust(-additional_space);
		for (int i = 0; i < arg_count; i++) {
			const int arg_vreg = instr.operands[3 + i].reg_index();
			const int arg_src_offset = get_variant_stack_offset(arg_vreg) + additional_space;
			emit_variant_move(REG_SP, i * variant_size(), REG_SP, arg_src_offset, REG_T0);
		}
		emit_mv(REG_A2, REG_SP);
	} else {
		emit_mv(REG_A2, REG_ZERO);
	}

	emit_load_stack_offset(REG_A0, result_offset + additional_space);
	emit_li(REG_A1, variant_type);
	emit_li(REG_A3, arg_count);
	emit_li(REG_A7, ECALL_VCONSTRUCT);
	emit_ecall();

	emit_stack_adjust(additional_space);
}

// ECALL_VCALL: arguments as contiguous array on stack, method name after, result through a5.
void RISCVCodeGen::gen_vcall(const IRInstruction& instr) {
	if (instr.operands.size() < 4) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "VCALL requires at least 4 operands");
	}

	int result_vreg = instr.operands[0].reg_index();
	int obj_vreg = instr.operands[1].reg_index();
	std::string method_name = text(instr.operands[2]);
	int arg_count = static_cast<int>(instr.operands[3].immediate());

	if (instr.operands.size() != static_cast<size_t>(4 + arg_count)) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "VCALL argument count mismatch");
	}

	int result_offset = get_variant_stack_offset(result_vreg);
	int obj_offset = get_variant_stack_offset(obj_vreg);

	spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3, REG_A4, REG_A5, REG_A6, REG_A7});

	// If we have arguments, allocate stack space for argument array.
	// Everything addressed off sp below this point is shifted by it.
	const bool direct_args = arg_count > 0 && consecutive_vreg_operands(
		instr, 4, size_t(arg_count), result_vreg);
	const int additional_space = arg_count > 0 && !direct_args
		? ((arg_count * variant_size()) + 15) & ~15 // Align to 16 bytes
		: 0;
	if (arg_count > 0) {
		if (direct_args) {
			emit_add_offset(REG_A3, REG_SP,
				get_variant_stack_offset(instr.operands[4].reg_index()));
		} else {
			emit_stack_adjust(-additional_space);
			for (int i = 0; i < arg_count; i++) {
				int arg_vreg = instr.operands[4 + i].reg_index();
				int arg_src_offset = get_variant_stack_offset(arg_vreg) + additional_space;
				emit_variant_move(REG_SP, i * variant_size(), REG_SP, arg_src_offset, REG_T0);
			}
			emit_mv(REG_A3, REG_SP);
		}
	} else {
		// No arguments, a3 = 0
		emit_mv(REG_A3, REG_ZERO);
	}

	// a0 = pointer to object Variant, past whatever the argument array took
	emit_load_stack_offset(REG_A0, obj_offset + additional_space);

	const int method_len = method_name.length();
	emit_la(REG_A1, rodata_string(method_name));

	// a2 = method length
	emit_li(REG_A2, method_len);

	// a4 = argument count
	emit_li(REG_A4, arg_count);

	// a5 = pointer to result Variant, past the argument array
	emit_load_stack_offset(REG_A5, result_offset + additional_space);

	emit_li(REG_A7, instr.super_call ? ECALL_VCALL_SUPER : ECALL_VCALL);
	emit_ecall();

	// Restore stack pointer
	emit_stack_adjust(additional_space);

	// VCALL always writes the result as a full Variant on stack (via a5 pointer)
}

void RISCVCodeGen::gen_make_dictionary(const IRInstruction& instr) {
	// Format: MAKE_DICTIONARY result_reg, pair_count, [key1_reg, val1_reg, key2_reg, val2_reg, ...]
	// For empty dictionaries via Dictionary() constructor: MAKE_DICTIONARY result_reg
	// For empty dictionaries via literal {}: MAKE_DICTIONARY result_reg, 0
	// For dictionaries with elements: pair_count > 0 with key/value regs
	if (instr.operands.size() < 1) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "MAKE_DICTIONARY requires at least 1 operand");
	}

	int result_vreg = instr.operands[0].reg_index();
	int result_offset = get_variant_stack_offset(result_vreg);

	// Check if this is the old Dictionary() constructor format (only 1 operand)
	// or the new dictionary literal format (2+ operands)
	bool is_constructor_format = (instr.operands.size() == 1);
	int pair_count = is_constructor_format ? 0 : static_cast<int>(instr.operands[1].immediate());

	spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3});

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
			int variant_vreg = instr.operands[2 + i].reg_index();
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
		emit_li(REG_A7, ECALL_VCREATE);
		emit_ecall();

		// Restore stack pointer
		emit_add_offset(REG_SP, REG_SP, args_space);
	}
}

void RISCVCodeGen::gen_make_array(const IRInstruction& instr) {
	// Format: MAKE_ARRAY result_reg, element_count, [element_reg1, element_reg2, ...]
	// For empty arrays: element_count = 0, no element regs
	if (instr.operands.size() < 2) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "MAKE_ARRAY requires at least 2 operands");
	}

	int result_vreg = instr.operands[0].reg_index();
	int element_count = static_cast<int>(instr.operands[1].immediate());
	int result_offset = get_variant_stack_offset(result_vreg);

	spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3});

	if (element_count == 0) {
		// Empty Array: use the abstraction
		emit_variant_create_empty_array(result_offset);
	} else {
		// Array with elements: sys_vcreate(&v, ARRAY, size, data_pointer)
		const bool direct_elements = consecutive_vreg_operands(
			instr, 2, size_t(element_count), result_vreg);
		int args_space = direct_elements ? 0 : element_count * variant_size();
		args_space = (args_space + 15) & ~15; // Align to 16 bytes

		if (!direct_elements) {
			emit_stack_adjust(-args_space);
			for (int i = 0; i < element_count; i++) {
				int elem_vreg = instr.operands[2 + i].reg_index();
				int elem_offset = get_variant_stack_offset(elem_vreg);
				emit_variant_move(REG_SP, i * variant_size(), REG_SP,
					elem_offset + args_space, REG_T0);
			}
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

		if (direct_elements) {
			emit_add_offset(REG_A3, REG_SP,
				get_variant_stack_offset(instr.operands[2].reg_index()));
		} else {
			emit_mv(REG_A3, REG_SP);
		}

		// a7 = ECALL_VCREATE (517)
		emit_li(REG_A7, ECALL_VCREATE);
		emit_ecall();

		// Restore stack pointer
		emit_stack_adjust(args_space);
	}
}

void RISCVCodeGen::gen_store_global(const IRInstruction& instr) {
	// STORE_GLOBAL global_index, src_reg
	// Stores a virtual register (Variant) into a global variable
	int64_t global_idx = instr.operands[0].immediate();
	int src_vreg = instr.operands[1].reg_index();
	int src_offset = get_variant_stack_offset(src_vreg);

	// Get the global's type information
	const IRGlobalVar& global = m_globals[global_idx];
	const bool needs_vassign = is_complex_variant_type(global.value_type);

	if (global.value_type == IRInstruction::TypeHint_NONE ||
			(m_fn.is_member_initializer && global.is_member() && needs_vassign)) {
		// A member initializer runs in a temporary call state. VSTORE_GLOBAL
		// promotes its final value into permanent storage; VASSIGN's empty-slot
		// fast path would copy the temporary positive index into the member.
		spill_around_syscall({ REG_A0, REG_A1, REG_A7 });
		emit_address_of_global(REG_A0, static_cast<size_t>(global_idx));
		emit_load_stack_offset(REG_A1, src_offset);
		emit_li(REG_A7, ECALL_VSTORE_GLOBAL);
		emit_ecall();
		return;
	}

	// Load address of global variable
	emit_address_of_global(REG_T0, static_cast<size_t>(global_idx));

	if (global.value_type == Variant::INT && !needs_vassign && src_vreg == m_fn.chained_vreg) {
		emit_li(REG_T1, Variant::INT);
		emit_store_variant_type(REG_T1, REG_T0, 0);
		emit_store_variant_int(REG_T2, REG_T0, 0);
		return;
	}

	// Load source address
	emit_load_stack_offset(REG_T1, src_offset);

	if (needs_vassign) {
		// Complex type: VASSIGN takes a0 = dest index, a1 = src
		// index, and answers with the resulting index in a0. a7
		// carries the syscall number, so it is clobbered too.
		spill_around_syscall({REG_A0, REG_A1, REG_A7});

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
		emit_li(REG_A7, ECALL_VASSIGN);
		emit_ecall();

		// VASSIGN returns the new index in A0
		emit_sd(REG_A0, REG_T0, 8);
	} else {
		if (is_scalar_variant_type(global.value_type)) {
			emit_scalar_variant_move(REG_T0, 0, REG_T1, 0, REG_T2);
		} else {
			emit_variant_move(REG_T0, 0, REG_T1, 0, REG_T2);
		}
	}

	if (global.holds_object) {
		spill_around_syscall({ REG_A0, REG_A7 });
		emit_address_of_global(REG_A0, static_cast<size_t>(global_idx));
		emit_li(REG_A7, ECALL_OBJ_RETAIN);
		emit_ecall();
	}
}

void RISCVCodeGen::gen_vget(const IRInstruction& instr) {
	if (instr.operands.size() != 4) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "VGET requires 4 operands (result_reg, obj_reg, string_idx, string_len)");
	}

	int result_vreg = instr.operands[0].reg_index();
	int obj_vreg = instr.operands[1].reg_index();
	int string_idx = static_cast<int>(instr.operands[2].immediate());
	int string_len = static_cast<int>(instr.operands[3].immediate());

	if (string_idx < 0 || static_cast<size_t>(string_idx) >= m_string_constants->size()) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "String constant index out of range");
	}
	const std::string& str = (*m_string_constants)[string_idx];

	int result_offset = get_variant_stack_offset(result_vreg);
	int obj_offset = get_variant_stack_offset(obj_vreg);

	spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3});

	emit_ld(REG_A0, REG_SP, obj_offset + 8);

	emit_la(REG_A1, rodata_string(str));
	emit_li(REG_A2, string_len);
	emit_load_stack_offset(REG_A3, result_offset);
	emit_li(REG_A7, ECALL_OBJ_PROP_GET);
	emit_ecall();
}

void RISCVCodeGen::gen_vget_inline(const IRInstruction& instr) {
	if (instr.operands.size() != 4) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "VGET_INLINE requires 4 operands");
	}

	int result_vreg = instr.operands[0].reg_index();
	int obj_vreg = instr.operands[1].reg_index();
	std::string member = text(instr.operands[2]);
	int obj_type_hint = static_cast<int>(instr.operands[3].immediate());

	int result_offset = get_variant_stack_offset(result_vreg);
	int obj_offset = get_variant_stack_offset(obj_vreg);

	const BuiltinMember layout = find_builtin_member(uint32_t(obj_type_hint), member);
	if (!layout.valid()) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"VGET_INLINE on a type with no inline member '" + member + "'");
	}

	if (layout.count > 1) {
		emit_li(REG_T0, int32_t(layout.result_type));
		emit_sw(REG_T0, REG_SP, result_offset);
		for (int i = 0; i < layout.count; i++) {
			if (layout.integer) {
				emit_lw(REG_T0, REG_SP, obj_offset + int_offset(layout.first_component + i));
				emit_sw(REG_T0, REG_SP, result_offset + int_offset(i));
			} else {
				emit_flr(REG_FA0, REG_SP, obj_offset + real_offset(layout.first_component + i));
				emit_fsr(REG_FA0, REG_SP, result_offset + real_offset(i));
			}
		}
		return;
	}

	const bool is_int_type = layout.integer;
	const int component_idx = layout.first_component;
	int member_offset = is_int_type ? int_offset(component_idx) : real_offset(component_idx);

	if (is_int_type) {
		emit_lw(REG_T0, REG_SP, obj_offset + member_offset);
		emit_li(REG_T1, 2);
		emit_sw(REG_T1, REG_SP, result_offset);
		emit_sext_w(REG_T0, REG_T0);
		emit_sd(REG_T0, REG_SP, result_offset + VARIANT_DATA_OFFSET);
	} else {
		// Variant::FLOAT is always double; widen real_t if needed
		emit_flr(REG_FA0, REG_SP, obj_offset + member_offset);
		emit_fcvt_d_r(REG_FA0, REG_FA0);
		emit_li(REG_T0, Variant::FLOAT);
		emit_sw(REG_T0, REG_SP, result_offset);
		emit_fsd(REG_FA0, REG_SP, result_offset + VARIANT_DATA_OFFSET);
	}
}

void RISCVCodeGen::gen_vset_inline(const IRInstruction& instr) {
	if (instr.operands.size() != 5) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "VSET_INLINE requires 5 operands");
	}

	int obj_vreg = instr.operands[0].reg_index();
	std::string member = text(instr.operands[1]);
	int obj_type_hint = static_cast<int>(instr.operands[2].immediate());
	int value_vreg = instr.operands[3].reg_index();
	const bool stamp_type = instr.operands[4].immediate() != 0;

	int obj_offset = get_variant_stack_offset(obj_vreg);
	int value_offset = get_variant_stack_offset(value_vreg);

	const BuiltinMember layout = find_builtin_member(uint32_t(obj_type_hint), member);
	if (!layout.valid()) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"VSET_INLINE on a type with no inline member '" + member + "'");
	}

	// Stamp the tag unless a type test already established it.
	if (stamp_type) {
		emit_li(REG_T0, obj_type_hint);
		emit_sw(REG_T0, REG_SP, obj_offset);
	}

	if (layout.count > 1) {
		for (int i = 0; i < layout.count; i++) {
			if (layout.integer) {
				emit_lw(REG_T0, REG_SP, value_offset + int_offset(i));
				emit_sw(REG_T0, REG_SP, obj_offset + int_offset(layout.first_component + i));
			} else {
				emit_flr(REG_FA0, REG_SP, value_offset + real_offset(i));
				emit_fsr(REG_FA0, REG_SP, obj_offset + real_offset(layout.first_component + i));
			}
		}
		return;
	}

	if (layout.integer) {
		emit_variant_component_to_int(value_offset, obj_offset, int_offset(layout.first_component));
	} else {
		// Color channels are reals; integer stores convert, not scale by 255.
		emit_variant_component_to_real(value_offset, obj_offset, real_offset(layout.first_component));
	}
}

void RISCVCodeGen::gen_switch(const IRInstruction& instr) {
	// Dense integer switch via inline jal table; fall-through = no match
	const int subject_vreg = instr.operands[0].reg_index();
	const int64_t base = instr.operands[1].immediate();
	const int64_t count = instr.operands[2].immediate();
	const int subject_offset = get_variant_stack_offset(subject_vreg);

	const std::string past_table = ".switch" + std::to_string(m_switch_tables) + ".out";
	m_switch_tables++;

	// Non-INT types (incl. float) fall through; INT hint skips the test
	if (instr.type_hint != Variant::INT) {
		emit_load_variant_type(REG_T0, REG_SP, subject_offset);
		emit_li(REG_T1, Variant::INT);
		mark_label_use(past_table, m_code.size());
		emit_bne(REG_T0, REG_T1, 0);
	}

	emit_load_variant_int(REG_T0, REG_SP, subject_offset);
	if (base != 0) {
		if (fits_in_signed(-base, I_TYPE_IMM_BITS)) {
			emit_addi(REG_T0, REG_T0, static_cast<int32_t>(-base));
		} else {
			emit_li(REG_T1, base);
			emit_sub(REG_T0, REG_T0, REG_T1);
		}
	}

	std::vector<std::string> targets;
	targets.reserve(size_t(count));
	for (int64_t entry = 0; entry < count; entry++) {
		targets.push_back(text(instr.operands[3 + entry]));
	}
	emit_dense_jump_table(REG_T0, targets, past_table);
	define_label(past_table);
}

// Clobbers t0, t1. index_reg must not be t1; consumed.
void RISCVCodeGen::emit_dense_jump_table(uint8_t index_reg, const std::vector<std::string>& labels,
		const std::string& past_label) {
	// Unsigned compare covers both ends (below base wraps high)
	emit_li(REG_T1, int64_t(labels.size()));
	mark_label_use(past_label, m_code.size());
	emit_bgeu(index_reg, REG_T1, 0);

	// Table address = PC + 12; no relocation needed
	emit_u_type(0x17, REG_T1, 0);           // auipc t1, 0
	emit_sh2add(REG_T0, index_reg, REG_T1); // t1 + index*4
	emit_jalr(REG_ZERO, REG_T0, 12);        // jump into table

	for (const std::string& label : labels) {
		mark_label_use(label, m_code.size());
		emit_jal(REG_ZERO, 0);
	}
}

// AWAIT: hands the Variant slot array to the host as the coroutine frame.
void RISCVCodeGen::gen_await(const IRInstruction& instr) {
	if (instr.operands.size() != 2) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "AWAIT requires 2 operands");
	}
	if (!m_fn.is_coroutine) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"AWAIT emitted in a function that was not planned as a coroutine");
	}

	const int result_vreg = instr.operands[0].reg_index();
	const int operand_vreg = instr.operands[1].reg_index();

	// Offsets before spill barrier to prevent allocator drift.
	const int result_offset = get_variant_stack_offset(result_vreg);
	const int operand_offset = get_variant_stack_offset(operand_vreg);

	// Full clobber: frame slots must be authoritative before the host copies them out.
	spill_all_registers();
	spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3, REG_A4, REG_A5});

	const std::string state_label = m_fn.resume_label + "." + std::to_string(m_fn.await_states.size());
	m_fn.await_states.push_back(state_label);

	emit_add_offset(REG_A0, REG_SP, operand_offset);
	emit_add_offset(REG_A1, REG_SP, SAVED_REG_SPACE);
	emit_li(REG_A2, m_fn.variant_space);
	emit_li(REG_A3, int64_t(m_fn.await_states.size()) - 1);
	emit_la(REG_A4, m_fn.resume_label);
	emit_li(REG_A5, result_offset - SAVED_REG_SPACE);
	emit_li(REG_A7, ECALL_AWAIT);
	emit_ecall();

	// a0 == 0: not awaitable, result slot already written. Fall through.
	mark_label_use(state_label, m_code.size());
	emit_beq(REG_A0, REG_ZERO, 0);

	emit_suspend_epilogue();

	// Fall-through and resume both continue here.
	define_label(state_label);
}

void RISCVCodeGen::emit_suspend_epilogue() {
	if (m_profiling_index >= 0) {
		emit_profiling_exit();
	}
	if (m_debug_index >= 0) {
		emit_debug_exit();
	}
	if (m_fn.saves_return_address) {
		emit_ld(REG_RA, REG_SP, SAVED_RA_OFFSET);
	}
	if (m_fn.stack_frame_size > 0) {
		emit_add_offset(REG_SP, REG_SP, m_fn.stack_frame_size);
	}
	emit_ret();
}

void RISCVCodeGen::emit_coroutine_resume_entry(const IRFunction& func) {
	define_label(m_fn.resume_label);
	m_functions[m_fn.resume_label] = m_code.size();

	// Prologue without parameter copy; the restored frame already holds them.
	if (m_profiling_index >= 0) {
		emit_profiling_entry();
	}
	if (m_fn.stack_frame_size > 0) {
		emit_add_offset(REG_SP, REG_SP, -m_fn.stack_frame_size);
	}
	if (m_debug_index >= 0) {
		emit_debug_entry();
	}
	emit_sd(REG_RA, REG_SP, SAVED_RA_OFFSET);
	emit_sd(REG_A0, REG_SP, SAVED_A0_OFFSET);

	// ECALL_AWAIT_RESTORE: host copies frame back, length-checked against suspension.
	emit_add_offset(REG_A0, REG_SP, SAVED_REG_SPACE);
	emit_li(REG_A1, m_fn.variant_space);
	emit_li(REG_A7, ECALL_AWAIT_RESTORE);
	emit_ecall();

	// a0 = state index (0..N-1). Dispatch via dense jump table.
	const std::string past_table = m_fn.resume_label + ".bad_state";
	emit_mv(REG_T0, REG_A0);
	emit_dense_jump_table(REG_T0, m_fn.await_states, past_table);

	// Invalid state index: return without resuming.
	define_label(past_table);
	emit_suspend_epilogue();
}

// ECALL_THROW(type_ptr, type_len, msg_ptr, msg_len, variant, function).
// Strings copied to stack as raw bytes for memview. Does not return.
void RISCVCodeGen::gen_throw(const IRInstruction& instr) {
	if (instr.operands.size() < 3) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "THROW requires at least 3 operands");
	}

	const std::string& type = text(instr.operands[0]);
	const std::string& message = text(instr.operands[1]);
	const int message_regs = static_cast<int>(instr.operands[2].immediate());
	if (instr.operands.size() != static_cast<size_t>(3 + message_regs)) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "THROW argument count mismatch");
	}
	const int message_offset = message_regs == 1
		? get_variant_stack_offset(instr.operands[3].reg_index())
		: -1;

	spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3, REG_A4, REG_A5});

	emit_la(REG_A0, rodata_string(type));
	emit_li(REG_A1, static_cast<int>(type.size()));
	emit_la(REG_A2, rodata_string(message));
	emit_li(REG_A3, static_cast<int>(message.size()));
	if (message_offset >= 0) {
		emit_load_stack_offset(REG_A4, message_offset);
	} else {
		emit_mv(REG_A4, REG_ZERO);  // variant = none
	}
	emit_mv(REG_A5, REG_ZERO);  // function = none
	emit_li(REG_A7, ECALL_THROW);
	emit_ecall();
}

void RISCVCodeGen::gen_print(const IRInstruction& instr) {
	// Arguments copied into contiguous array below sp for sys_print
	if (instr.operands.size() < 3) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "PRINT requires at least 3 operands");
	}

	int result_vreg = instr.operands[0].reg_index();
	const int channel = static_cast<int>(instr.operands[1].immediate());
	int arg_count = static_cast<int>(instr.operands[2].immediate());

	if (instr.operands.size() != static_cast<size_t>(3 + arg_count)) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "PRINT argument count mismatch");
	}

	// print() uses ECALL_PRINT (no channel); rest use ECALL_PRINT_CHANNEL.
	const bool plain = channel == int(Print_Channel::PRINT);
	const int syscall = plain ? ECALL_PRINT : ECALL_PRINT_CHANNEL;

	int result_offset = get_variant_stack_offset(result_vreg);

	spill_around_syscall({REG_A0, REG_A1, REG_A2});

	if (arg_count == 0) {
		emit_mv(REG_A0, REG_ZERO);
		emit_li(REG_A1, 0);
	} else {
		int args_space = arg_count * variant_size();
		args_space = (args_space + 15) & ~15;
		emit_add_offset(REG_SP, REG_SP, -args_space);

		for (int i = 0; i < arg_count; i++) {
			int arg_vreg = instr.operands[3 + i].reg_index();
			int arg_src_offset = get_variant_stack_offset(arg_vreg) + args_space;
			emit_variant_move(REG_SP, i * variant_size(), REG_SP, arg_src_offset, REG_T0);
		}

		emit_mv(REG_A0, REG_SP);
		emit_li(REG_A1, arg_count);
		if (!plain) {
			emit_li(REG_A2, channel);
		}
		emit_li(REG_A7, syscall);
		emit_ecall();

		emit_add_offset(REG_SP, REG_SP, args_space);

		emit_li(REG_T0, Variant::NIL);
		emit_store_variant_type(REG_T0, REG_SP, result_offset);
		return;
	}

	if (!plain) {
		emit_li(REG_A2, channel);
	}
	emit_li(REG_A7, syscall);
	emit_ecall();

	// print() returns nil
	emit_li(REG_T0, Variant::NIL);
	emit_store_variant_type(REG_T0, REG_SP, result_offset);
}

// Native path when typed, VEVAL otherwise. POW and IN always use VEVAL.
void RISCVCodeGen::gen_binary_op(const IRInstruction& instr) {
	if (instr.operands.size() < 3 ||
		instr.operands[0].type != IRValue::Type::REGISTER) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Arithmetic operations require at least 3 operands with first being REGISTER");
	}

	int dst_vreg = instr.operands[0].reg_index();
	int dst_offset = get_variant_stack_offset(dst_vreg);

	bool lhs_is_reg = instr.operands[1].type == IRValue::Type::REGISTER;
	bool rhs_is_reg = instr.operands.size() > 2 && instr.operands[2].type == IRValue::Type::REGISTER;

	const bool host_only = instr.opcode == IROpcode::POW || instr.opcode == IROpcode::IN;

	if (!host_only && instr.type_hint != IRInstruction::TypeHint_NONE && lhs_is_reg && rhs_is_reg) {
		int lhs_vreg_local = instr.operands[1].reg_index();
		int rhs_vreg_local = instr.operands[2].reg_index();
		int lhs_offset = get_variant_stack_offset(lhs_vreg_local);
		int rhs_offset = get_variant_stack_offset(rhs_vreg_local);

		if (instr.type_hint == Variant::INT) {
			int64_t imm;
			if (folds_to_immediate(instr, 2) &&
				constant_int(rhs_vreg_local, imm)) {
				emit_typed_int_binary_op_imm(dst_vreg, dst_offset, lhs_offset, imm, instr.opcode, lhs_vreg_local);
				return;
			}
			// Commutative: try the left operand.
			if (folds_to_immediate(instr, 1) &&
				constant_int(lhs_vreg_local, imm)) {
				emit_typed_int_binary_op_imm(dst_vreg, dst_offset, rhs_offset, imm, instr.opcode, rhs_vreg_local);
				return;
			}
			emit_typed_int_binary_op(dst_vreg, dst_offset, lhs_offset, rhs_offset, instr.opcode,
				lhs_vreg_local, rhs_vreg_local);
			return;
		} else if (instr.type_hint == Variant::FLOAT) {
			emit_typed_float_binary_op(dst_vreg, dst_offset, lhs_vreg_local, lhs_offset,
				rhs_vreg_local, rhs_offset, instr.opcode);
			return;
		} else if (TypeHintUtils::is_vector(instr.type_hint)) {
			const bool lhs_vector = TypeHintUtils::is_vector(instr.lhs_type_hint);
			const bool rhs_vector = TypeHintUtils::is_vector(instr.rhs_type_hint);
			if (lhs_vector != rhs_vector) {
				const bool scalar_on_left = rhs_vector;
				emit_typed_vector_scalar_op(dst_vreg, dst_offset,
					scalar_on_left ? rhs_offset : lhs_offset,
					scalar_on_left ? lhs_offset : rhs_offset,
					scalar_on_left, instr.opcode, instr.type_hint,
					scalar_on_left ? instr.lhs_type_hint : instr.rhs_type_hint);
			} else {
				emit_typed_vector_binary_op(dst_vreg, dst_offset, lhs_offset, rhs_offset,
					instr.opcode, instr.type_hint);
			}
			return;
		}
	}

	// A known non-numeric operation still belongs on the host, but it does not
	// need speculative INT/FLOAT guards first (notably String + String).
	if (!host_only && instr.type_hint != IRInstruction::TypeHint_NONE &&
		instr.type_hint != Variant::INT && instr.type_hint != Variant::FLOAT &&
		!TypeHintUtils::is_vector(instr.type_hint) && lhs_is_reg && rhs_is_reg)
	{
		const int lhs_offset = get_variant_stack_offset(instr.operands[1].reg_index());
		const int rhs_offset = get_variant_stack_offset(instr.operands[2].reg_index());
		int variant_op = instr.opcode == IROpcode::ADD ? 6 :
			instr.opcode == IROpcode::SUB ? 7 :
			instr.opcode == IROpcode::MUL ? 8 :
			instr.opcode == IROpcode::DIV ? 9 : 12;
		emit_variant_eval(dst_offset, lhs_offset, rhs_offset, variant_op);
		return;
	}

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
		case IROpcode::POW: variant_op = 13; break; // OP_POWER
		case IROpcode::IN: variant_op = 24; break;  // OP_IN
		default: variant_op = 6; break;
	}

	if (lhs_is_reg && rhs_is_reg) {
		int lhs_vreg_local = instr.operands[1].reg_index();
		int rhs_vreg_local = instr.operands[2].reg_index();
		int lhs_offset = get_variant_stack_offset(lhs_vreg_local);
		int rhs_offset = get_variant_stack_offset(rhs_vreg_local);

		const bool int_path = !host_only && has_int_fast_path(instr.opcode);
		const bool float_path = !host_only && has_float_fast_path(instr.opcode);
		if (int_path || float_path) {
			// Spill before branch: allocator state must hold on both paths
			spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3});

			const bool shift = instr.opcode == IROpcode::SHL || instr.opcode == IROpcode::SHR;
			const std::string host = gen_local_label(".veval");
			const std::string done = gen_local_label(".veval_done");
			const std::string floats = float_path ? gen_local_label(".veval_float") : host;

			if (int_path) {
				emit_branch_unless_both_int(lhs_offset, rhs_offset, floats, shift);
				emit_typed_int_binary_op(dst_vreg, dst_offset, lhs_offset, rhs_offset, instr.opcode);
				mark_label_use(done, m_code.size());
				emit_jal(REG_ZERO, 0);
			}
			if (float_path) {
				if (int_path) {
					define_label(floats);
				}
				emit_branch_unless_float_pair(lhs_offset, rhs_offset, host);
				emit_float_pair_binary_op(dst_offset, lhs_offset, rhs_offset, instr.opcode);
				mark_label_use(done, m_code.size());
				emit_jal(REG_ZERO, 0);
			}
			define_label(host);
			emit_variant_eval(dst_offset, lhs_offset, rhs_offset, variant_op, false);
			define_label(done);
			return;
		}

		emit_variant_eval(dst_offset, lhs_offset, rhs_offset, variant_op);
	} else if (lhs_is_reg && !rhs_is_reg && instr.operands[2].type == IRValue::Type::IMMEDIATE) {
		int lhs_vreg_local = instr.operands[1].reg_index();
		int64_t imm_val = instr.operands[2].immediate();
		int lhs_offset = get_variant_stack_offset(lhs_vreg_local);

		int imm_offset = get_scratch_variant_offset();
		emit_variant_create_int(imm_offset, static_cast<int>(imm_val));
		emit_variant_eval(dst_offset, lhs_offset, imm_offset, variant_op);
	} else if (!lhs_is_reg && rhs_is_reg && instr.operands[1].type == IRValue::Type::IMMEDIATE) {
		int64_t imm_val = instr.operands[1].immediate();
		int rhs_vreg_local = instr.operands[2].reg_index();
		int rhs_offset = get_variant_stack_offset(rhs_vreg_local);

		int imm_offset = get_scratch_variant_offset();
		emit_variant_create_int(imm_offset, static_cast<int>(imm_val));
		emit_variant_eval(dst_offset, imm_offset, rhs_offset, variant_op);
	} else {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Unsupported operand types for arithmetic operation");
	}
}

void RISCVCodeGen::gen_make_packed_array(const IRInstruction& instr) {
	if (instr.operands.size() < 2) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Packed array constructor requires at least 2 operands");
	}

	int result_vreg = instr.operands[0].reg_index();
	int element_count = static_cast<int>(instr.operands[1].immediate());
	int result_offset = get_variant_stack_offset(result_vreg);

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
			variant_type = Variant::ARRAY;
			break;
	}

	if (element_count == 0) {
		emit_add_offset(REG_A0, REG_SP, result_offset);
		emit_li(REG_A1, variant_type);
		emit_li(REG_A2, 0);
		emit_li(REG_A3, 0);
		emit_li(REG_A7, ECALL_VCREATE);
		emit_ecall();
	} else {
		// ECALL_PACKED_ARRAY_OPS converts a contiguous Variant array to packed
		int args_space = element_count * variant_size();
		args_space = (args_space + 15) & ~15;
		emit_stack_adjust(-args_space);

		for (int i = 0; i < element_count; i++) {
			int elem_vreg = instr.operands[2 + i].reg_index();
			int elem_offset = get_variant_stack_offset(elem_vreg);
			int dst_offset = i * variant_size();
			emit_variant_move(REG_SP, dst_offset, REG_SP, args_space + elem_offset, REG_T0);
		}

		emit_li(REG_A0, variant_type);
		int adjusted_dst_offset = result_offset + args_space;
		emit_add_offset(REG_A1, REG_SP, adjusted_dst_offset);
		emit_mv(REG_A2, REG_SP);
		emit_li(REG_A3, element_count);
		emit_li(REG_A7, ECALL_PACKED_ARRAY_OPS);
		emit_ecall();

		emit_add_offset(REG_SP, REG_SP, args_space);
	}
}

void RISCVCodeGen::gen_comparison(const IRInstruction& instr) {
	if (instr.operands.size() < 3 ||
		instr.operands[0].type != IRValue::Type::REGISTER) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Comparison operations require at least 3 operands with first being REGISTER");
	}

	int dst_vreg = instr.operands[0].reg_index();
	int dst_offset = get_variant_stack_offset(dst_vreg);

	bool lhs_is_reg = instr.operands[1].type == IRValue::Type::REGISTER;
	bool rhs_is_reg = instr.operands.size() > 2 && instr.operands[2].type == IRValue::Type::REGISTER;

	if (instr.type_hint == Variant::INT && lhs_is_reg && rhs_is_reg) {
		int lhs_vreg = instr.operands[1].reg_index();
		int rhs_vreg = instr.operands[2].reg_index();
		int lhs_offset = get_variant_stack_offset(lhs_vreg);
		int rhs_offset = get_variant_stack_offset(rhs_vreg);
		int64_t imm;
		if (folds_to_immediate(instr, 2) && constant_int(rhs_vreg, imm)) {
			emit_typed_int_comparison_imm(dst_vreg, dst_offset, lhs_vreg, lhs_offset,
				imm, false, instr.opcode);
			return;
		}
		if (folds_to_immediate(instr, 1) && constant_int(lhs_vreg, imm)) {
			emit_typed_int_comparison_imm(dst_vreg, dst_offset, rhs_vreg, rhs_offset,
				imm, true, instr.opcode);
			return;
		}

		emit_typed_int_comparison(dst_vreg, dst_offset, lhs_vreg, lhs_offset,
			rhs_vreg, rhs_offset, instr.opcode);
		return;
	}
	if (instr.type_hint == Variant::FLOAT && lhs_is_reg && rhs_is_reg) {
		const int lhs_vreg = instr.operands[1].reg_index();
		const int rhs_vreg = instr.operands[2].reg_index();
		emit_typed_float_comparison(dst_vreg, dst_offset,
			lhs_vreg, get_variant_stack_offset(lhs_vreg),
			rhs_vreg, get_variant_stack_offset(rhs_vreg), instr.opcode);
		return;
	}

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
		int lhs_vreg = instr.operands[1].reg_index();
		int rhs_vreg = instr.operands[2].reg_index();
		int lhs_offset = get_variant_stack_offset(lhs_vreg);
		int rhs_offset = get_variant_stack_offset(rhs_vreg);

		spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3});
		const std::string floats = gen_local_label(".vcmp_float");
		const std::string host = gen_local_label(".vcmp");
		const std::string done = gen_local_label(".vcmp_done");

		emit_branch_unless_both_int(lhs_offset, rhs_offset, floats, false);
		emit_typed_int_comparison(dst_vreg, dst_offset, lhs_vreg, lhs_offset,
			rhs_vreg, rhs_offset, instr.opcode);
		mark_label_use(done, m_code.size());
		emit_jal(REG_ZERO, 0);

		define_label(floats);
		emit_branch_unless_float_pair(lhs_offset, rhs_offset, host);
		emit_float_pair_comparison(dst_offset, lhs_offset, rhs_offset, instr.opcode);
		mark_label_use(done, m_code.size());
		emit_jal(REG_ZERO, 0);

		define_label(host);
		emit_variant_eval(dst_offset, lhs_offset, rhs_offset, variant_op, false);
		define_label(done);
	} else if (lhs_is_reg && !rhs_is_reg && instr.operands[2].type == IRValue::Type::IMMEDIATE) {
		int lhs_vreg = instr.operands[1].reg_index();
		int lhs_offset = get_variant_stack_offset(lhs_vreg);
		int64_t imm_val = instr.operands[2].immediate();

		int imm_offset = get_scratch_variant_offset();
		emit_variant_create_int(imm_offset, static_cast<int>(imm_val));
		emit_variant_eval(dst_offset, lhs_offset, imm_offset, variant_op);
	} else if (!lhs_is_reg && rhs_is_reg && instr.operands[1].type == IRValue::Type::IMMEDIATE) {
		int rhs_vreg = instr.operands[2].reg_index();
		int rhs_offset = get_variant_stack_offset(rhs_vreg);
		int64_t imm_val = instr.operands[1].immediate();

		int imm_offset = get_scratch_variant_offset();
		emit_variant_create_int(imm_offset, static_cast<int>(imm_val));
		emit_variant_eval(dst_offset, imm_offset, rhs_offset, variant_op);
	} else {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Unsupported operand types for comparison");
	}
}

// Fused comparison + branch: no intermediate bool Variant
void RISCVCodeGen::gen_fused_branch(const IRInstruction& instr) {
	if (instr.operands.size() < 3) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Fused branch requires 3 operands: lhs, rhs, label");
	}

	bool lhs_is_reg = instr.operands[0].type == IRValue::Type::REGISTER;
	bool rhs_is_reg = instr.operands[1].type == IRValue::Type::REGISTER;

	if (!lhs_is_reg || !rhs_is_reg) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Fused branch requires register operands");
	}

	int lhs_vreg = instr.operands[0].reg_index();
	int rhs_vreg = instr.operands[1].reg_index();
	std::string label = text(instr.operands[2]);

	int lhs_offset = get_variant_stack_offset(lhs_vreg);
	int rhs_offset = get_variant_stack_offset(rhs_vreg);

	if (instr.type_hint == Variant::INT) {
		int64_t imm;
		if (folds_to_immediate(instr, 1) && constant_int(rhs_vreg, imm)) {
			emit_int_fused_branch_imm(instr.opcode, lhs_vreg, lhs_offset, imm, false, label);
			return;
		}
		if (folds_to_immediate(instr, 0) && constant_int(lhs_vreg, imm)) {
			emit_int_fused_branch_imm(instr.opcode, rhs_vreg, rhs_offset, imm, true, label);
			return;
		}
		emit_int_fused_branch(instr.opcode, lhs_vreg, lhs_offset, rhs_vreg, rhs_offset, label);
		return;
	}
	if (instr.type_hint == Variant::FLOAT) {
		emit_typed_float_fused_branch(instr.opcode, lhs_vreg, lhs_offset,
			rhs_vreg, rhs_offset, label);
		return;
	}

	int variant_op;
	switch (instr.opcode) {
		case IROpcode::BRANCH_EQ:  variant_op = 0; break;
		case IROpcode::BRANCH_NEQ: variant_op = 1; break;
		case IROpcode::BRANCH_LT:  variant_op = 2; break;
		case IROpcode::BRANCH_LTE: variant_op = 3; break;
		case IROpcode::BRANCH_GT:  variant_op = 4; break;
		case IROpcode::BRANCH_GTE: variant_op = 5; break;
		default: variant_op = 0; break;
	}

	// Speculative INT fast path for untyped operands
	const int tmp_offset = get_scratch_variant_offset();
	spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3});

	const std::string host = gen_local_label(".vcmp");
	const std::string done = gen_local_label(".vcmp_done");
	const std::string floats = gen_local_label(".vcmp_float");
	emit_branch_unless_both_int(lhs_offset, rhs_offset, floats, false);
	emit_int_fused_branch(instr.opcode, lhs_vreg, lhs_offset, rhs_vreg, rhs_offset, label);
	mark_label_use(done, m_code.size());
	emit_jal(REG_ZERO, 0);

	define_label(floats);
	emit_branch_unless_float_pair(lhs_offset, rhs_offset, host);
	emit_float_pair_fused_branch(instr.opcode, lhs_offset, rhs_offset, label);
	mark_label_use(done, m_code.size());
	emit_jal(REG_ZERO, 0);

	define_label(host);
	emit_variant_eval(tmp_offset, lhs_offset, rhs_offset, variant_op, false);
	emit_load_variant_bool(REG_T0, REG_SP, tmp_offset);
	mark_label_use(label, m_code.size());
	emit_bne(REG_T0, REG_ZERO, 0);
	define_label(done);
}

void RISCVCodeGen::gen_vset(const IRInstruction& instr) {
	if (instr.operands.size() != 4) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "VSET requires 4 operands (obj_reg, string_idx, string_len, value_reg)");
	}

	int obj_vreg = instr.operands[0].reg_index();
	int string_idx = static_cast<int>(instr.operands[1].immediate());
	int string_len = static_cast<int>(instr.operands[2].immediate());
	int value_vreg = instr.operands[3].reg_index();

	if (string_idx < 0 || static_cast<size_t>(string_idx) >= m_string_constants->size()) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "String constant index out of range");
	}
	const std::string& str = (*m_string_constants)[string_idx];

	int obj_offset = get_variant_stack_offset(obj_vreg);
	int value_offset = get_variant_stack_offset(value_vreg);

	spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3});

	emit_ld(REG_A0, REG_SP, obj_offset + 8);

	emit_la(REG_A1, rodata_string(str));
	emit_li(REG_A2, string_len);
	emit_load_stack_offset(REG_A3, value_offset);
	emit_li(REG_A7, ECALL_OBJ_PROP_SET);
	emit_ecall();
}

void RISCVCodeGen::gen_call(const IRInstruction& instr) {
	if (instr.operands.size() < 3) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "CALL requires at least 3 operands");
	}

	std::string func_name = text(instr.operands[0]);
	if (instr.trusted_internal_call && m_trusted_internal_entries.count(func_name) != 0) {
		func_name = ".typed$" + func_name;
	}
	int result_vreg = instr.operands[1].reg_index();
	int arg_count = static_cast<int>(instr.operands[2].immediate());

	if (instr.operands.size() != static_cast<size_t>(3 + arg_count)) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "CALL argument count mismatch");
	}

	spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3, REG_A4, REG_A5, REG_A6, REG_A7, REG_RA});
	clear_block_value_state();

	int return_var_offset = get_variant_stack_offset(result_vreg);

	if (arg_count > static_cast<int>(IRFunction::MAX_PARAMETERS)) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"Call passes " + std::to_string(arg_count) +
			" arguments, but the SafeGDScript ABI supports at most " +
			std::to_string(IRFunction::MAX_PARAMETERS));
	}
	const int overflow_count = arg_count > static_cast<int>(IRFunction::REGISTER_PARAMETERS)
		? arg_count - static_cast<int>(IRFunction::REGISTER_PARAMETERS)
		: 0;
	const int outgoing_space = overflow_count > 0
		? ((overflow_count * int(CallABI::STACK_SLOT_SIZE)) + 15) & ~15
		: 0;
	if (outgoing_space > 0) {
		emit_stack_adjust(-outgoing_space);
	}
	for (int i = 0; i < arg_count; i++) {
		int arg_vreg = instr.operands[3 + i].reg_index();
		int arg_offset = get_variant_stack_offset(arg_vreg) + outgoing_space;
		if (i < static_cast<int>(IRFunction::REGISTER_PARAMETERS)) {
			uint8_t arg_reg = REG_A1 + static_cast<uint8_t>(i);
			emit_add_offset(arg_reg, REG_SP, arg_offset);
		} else {
			emit_add_offset(REG_WIDE_SCRATCH, REG_SP, arg_offset);
			emit_sd(REG_WIDE_SCRATCH, REG_SP,
				int32_t((i - IRFunction::REGISTER_PARAMETERS) * CallABI::STACK_SLOT_SIZE));
		}
	}

	emit_add_offset(REG_A0, REG_SP, return_var_offset + outgoing_space);

	if (!m_fn.saves_return_address) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"Call emitted in a function whose prologue did not save the return address");
	}
	mark_label_use(func_name, m_code.size());
	emit_jal(REG_RA, 0);
	emit_stack_adjust(outgoing_space);
}

void RISCVCodeGen::gen_call_hosted(const IRInstruction& instr) {
	if (instr.operands.size() < 3) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "CALL_HOSTED requires at least 3 operands");
	}

	const std::string& func_name = text(instr.operands[0]);
	const int result_vreg = instr.operands[1].reg_index();
	const int arg_count = static_cast<int>(instr.operands[2].immediate());

	if (instr.operands.size() != static_cast<size_t>(3 + arg_count)) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "CALL_HOSTED argument count mismatch");
	}

	const int result_offset = get_variant_stack_offset(result_vreg);

	spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3, REG_A7});

	const bool direct_args = arg_count > 0 && consecutive_vreg_operands(
		instr, 3, size_t(arg_count), result_vreg);
	const int additional_space = arg_count > 0 && !direct_args
		? ((arg_count * variant_size()) + 15) & ~15
		: 0;
	if (arg_count > 0) {
		if (direct_args) {
			emit_add_offset(REG_A1, REG_SP,
				get_variant_stack_offset(instr.operands[3].reg_index()));
		} else {
			emit_stack_adjust(-additional_space);
			for (int i = 0; i < arg_count; i++) {
				const int arg_vreg = instr.operands[3 + i].reg_index();
				const int arg_src_offset = get_variant_stack_offset(arg_vreg) + additional_space;
				emit_variant_move(REG_SP, i * variant_size(), REG_SP, arg_src_offset, REG_T0);
			}
			emit_mv(REG_A1, REG_SP);
		}
	} else {
		emit_mv(REG_A1, REG_ZERO);
	}

	emit_la(REG_A0, func_name);
	emit_li(REG_A2, arg_count);
	emit_load_stack_offset(REG_A3, result_offset + additional_space);
	emit_li(REG_A7, ECALL_CALL_GUEST);
	emit_ecall();

	emit_stack_adjust(additional_space);
}

void RISCVCodeGen::gen_instruction(const IRInstruction& instr) {
	switch (instr.opcode) {
		case IROpcode::LABEL:
			clear_block_value_state();
			define_label(text(instr.operands[0]));
			break;

		case IROpcode::SCOPE_MARK:
			gen_scope_mark(instr);
			break;

		case IROpcode::SCOPE_RELEASE:
			gen_scope_release(instr);
			break;

		// Source-requested stop; not added to the host's installed list.
		case IROpcode::BREAKPOINT:
			emit_breakpoint(instr.line, false, true, true);
			break;

		case IROpcode::LOAD_IMM: {
			int vreg = instr.operands[0].reg_index();
			int64_t value = instr.operands[1].immediate();
			auto [base, offset] = value_destination(vreg);
			emit_variant_create_int(offset, value, base);
			break;
		}

		case IROpcode::LOAD_FLOAT_IMM: {
			int vreg = instr.operands[0].reg_index();
			double value = instr.operands[1].float_number();
			auto [base, offset] = value_destination(vreg);
			emit_variant_create_float(offset, value, base);
			break;
		}

		case IROpcode::LOAD_BOOL: {
			int vreg = instr.operands[0].reg_index();
			int64_t value = instr.operands[1].immediate();
			auto [base, offset] = value_destination(vreg);
			emit_variant_create_bool(offset, value != 0, base);
			break;
		}

		case IROpcode::LOAD_NIL: {
			// NIL: tag only, no payload.
			int vreg = instr.operands[0].reg_index();
			auto [base, offset] = value_destination(vreg);
			emit_li(REG_T0, Variant::NIL);
			emit_store_variant_type(REG_T0, base, offset);
			break;
		}

		case IROpcode::LOAD_STRING: {
			int vreg = instr.operands[0].reg_index();
			int64_t string_idx = instr.operands[1].immediate();
			int stack_offset = get_variant_stack_offset(vreg);
			emit_variant_create_string(stack_offset, static_cast<int>(string_idx));
			break;
		}

		case IROpcode::LOAD_STRING_AS: {
			int vreg = instr.operands[0].reg_index();
			int64_t string_idx = instr.operands[1].immediate();
			int variant_type = static_cast<int>(instr.operands[2].immediate());
			int stack_offset = get_variant_stack_offset(vreg);
			emit_variant_create_string(stack_offset, static_cast<int>(string_idx), variant_type);
			break;
		}

		case IROpcode::MOVE: {
			int dst_vreg = instr.operands[0].reg_index();
			int src_vreg = instr.operands[1].reg_index();

			if (dst_vreg == src_vreg) {
				break;
			}

			int dst_offset = get_variant_stack_offset(dst_vreg);
			int src_offset = get_variant_stack_offset(src_vreg);

			if (dst_offset == src_offset) {
				break;
			}

			auto [base, offset] = value_destination(dst_vreg);
			const int known = src_vreg >= 0 && size_t(src_vreg) < m_fn.known_tags.size()
				? m_fn.known_tags[size_t(src_vreg)] : IRInstruction::TypeHint_NONE;
			if (is_scalar_variant_type(known)) {
				emit_scalar_variant_move(base, offset, REG_SP, src_offset, REG_T0);
			} else {
				emit_variant_move(base, offset, REG_SP, src_offset, REG_T0);
			}
			break;
		}

		case IROpcode::TYPE_TEST_MASK: {
			const int dst_vreg = instr.operands[0].reg_index();
			const int src_vreg = instr.operands[1].reg_index();
			const int64_t mask = instr.operands[2].immediate();
			const int src_offset = get_variant_stack_offset(src_vreg);

			emit_load_variant_type(REG_T0, REG_SP, src_offset);
			emit_li(REG_T1, mask);
			emit_srl(REG_T1, REG_T1, REG_T0);
			emit_andi(REG_T1, REG_T1, 1);

			auto [base, offset] = value_destination(dst_vreg);
			emit_store_variant_bool(REG_T1, base, offset);
			emit_li(REG_T0, Variant::BOOL);
			emit_store_variant_type(REG_T0, base, offset);
			break;
		}

		case IROpcode::TRAIT_TEST:
			gen_trait_test(instr);
			break;

		case IROpcode::TYPE_TEST: {
			// TYPE_TEST dst, src, variant_type
			//
			// The type tag is the first 4 bytes of every Variant, so the test
			// is a load, an xor against the tag, and seqz: no syscall, and no
			// dependence on the payload.
			int dst_vreg = instr.operands[0].reg_index();
			int src_vreg = instr.operands[1].reg_index();
			const int64_t tested = instr.operands[2].immediate();
			int src_offset = get_variant_stack_offset(src_vreg);

			emit_load_variant_type(REG_T0, REG_SP, src_offset);
			// Every Variant::Type fits the 12-bit immediate range.
			emit_xori(REG_T0, REG_T0, static_cast<int32_t>(tested));
			emit_seqz(REG_T0, REG_T0);

			auto [base, offset] = value_destination(dst_vreg);
			emit_store_variant_bool(REG_T0, base, offset);
			emit_li(REG_T1, Variant::BOOL);
			emit_store_variant_type(REG_T1, base, offset);
			break;
		}

		case IROpcode::MAKE_SCOPED: {
			// MAKE_SCOPED dst, src, variant_type
			//
			// The handle a syscall answered with, boxed: the tag from the
			// immediate and the index from src's integer payload.
			int dst_vreg = instr.operands[0].reg_index();
			int src_vreg = instr.operands[1].reg_index();
			const int64_t tag = instr.operands[2].immediate();
			int src_offset = get_variant_stack_offset(src_vreg);

			emit_ld(REG_T0, REG_SP, src_offset + VARIANT_DATA_OFFSET);
			auto [base, offset] = value_destination(dst_vreg);
			emit_sd(REG_T0, base, offset + VARIANT_DATA_OFFSET);
			emit_li(REG_T1, static_cast<int32_t>(tag));
			emit_store_variant_type(REG_T1, base, offset);
			break;
		}

		case IROpcode::TYPE_OF: {
			// typeof(): load tag (first 4 bytes), box as INT.
			int dst_vreg = instr.operands[0].reg_index();
			int src_vreg = instr.operands[1].reg_index();
			int src_offset = get_variant_stack_offset(src_vreg);

			emit_load_variant_type(REG_T0, REG_SP, src_offset);

			auto [base, offset] = value_destination(dst_vreg);
			emit_store_variant_int(REG_T0, base, offset);
			emit_li(REG_T1, Variant::INT);
			emit_store_variant_type(REG_T1, base, offset);
			break;
		}

		case IROpcode::CONVERT: {
			// CONVERT dst_reg, src_reg  with the target type in type_hint.
			int dst_vreg = instr.operands[0].reg_index();
			int src_vreg = instr.operands[1].reg_index();
			int dst_offset = get_variant_stack_offset(dst_vreg);
			int src_offset = get_variant_stack_offset(src_vreg);

			const auto from = static_cast<IRInstruction::TypeHint>(instr.operands[2].immediate());

			// The source type picks the load: a BOOL payload is one byte, an
			// INT all eight. Nothing else differs -- both widen to the same
			// 64-bit value.
			if (from == Variant::BOOL) {
				emit_lbu(REG_T0, REG_SP, src_offset + VARIANT_DATA_OFFSET);
			} else if (from == Variant::INT) {
				emit_ld(REG_T0, REG_SP, src_offset + VARIANT_DATA_OFFSET);
			} else if (from == Variant::FLOAT) {
				// fcvt.l.d: truncate-toward-zero, matching int() semantics.
				emit_fld(REG_FA0, REG_SP, src_offset + VARIANT_DATA_OFFSET);
				emit_fcvt_l_d(REG_T0, REG_FA0);
			} else {
				throw CompilerException(ErrorType::RISCV_codegen_ERROR,
					std::string("CONVERT from ") + variant_type_name(from) + " is not implemented");
			}

			if (instr.type_hint == Variant::FLOAT) {
				// Variant::FLOAT is always a 64-bit double, whatever real_t
				// is, so this is fcvt.d.l and a plain 64-bit store.
				emit_fcvt_d_l(REG_FA0, REG_T0);
				emit_li(REG_T1, Variant::FLOAT);
				emit_sw(REG_T1, REG_SP, dst_offset);
				emit_fsd(REG_FA0, REG_SP, dst_offset + VARIANT_DATA_OFFSET);
			} else if (instr.type_hint == Variant::INT) {
				emit_li(REG_T1, Variant::INT);
				emit_sw(REG_T1, REG_SP, dst_offset);
				emit_sd(REG_T0, REG_SP, dst_offset + VARIANT_DATA_OFFSET);
			} else {
				throw CompilerException(ErrorType::RISCV_codegen_ERROR,
					std::string("CONVERT to ") + variant_type_name(instr.type_hint) +
					" is not implemented");
			}
			break;
		}

		case IROpcode::COERCE: {
			const int dst_vreg = instr.operands[0].reg_index();
			const int src_vreg = instr.operands[1].reg_index();
			gen_coerce(dst_vreg, src_vreg, instr.type_hint);
			break;
		}

		case IROpcode::LOAD_GLOBAL: {
			// LOAD_GLOBAL dst_reg, global_index
			// Loads a global variable (Variant) from the global data area into a virtual register
			int dst_vreg = instr.operands[0].reg_index();
			int64_t global_idx = instr.operands[1].immediate();

			if (m_fn.keep_int_in_reg >= 0) {
				emit_address_of_global(REG_T0, static_cast<size_t>(global_idx));
				emit_load_variant_int(REG_T2, REG_T0, 0);
				m_fn.next_chained_vreg = m_fn.keep_int_in_reg;
				break;
			}

			if (m_fn.global_handles.count(dst_vreg)) {
				break;
			}

			// Load global base address into t0
			// For now, we'll use a label to mark the global data section
			emit_address_of_global(REG_T0, static_cast<size_t>(global_idx));

			// Copy the whole Variant out of the global data area. The
			// destination is addressed off its base register directly
			// rather than through a second pointer register, which is one
			// instruction the stores were already able to do themselves.
			auto [base, offset] = value_destination(dst_vreg);
			if (is_scalar_variant_type(m_globals[size_t(global_idx)].value_type)) {
				emit_scalar_variant_move(base, offset, REG_T0, 0, REG_T1);
			} else {
				emit_variant_move(base, offset, REG_T0, 0, REG_T1);
			}
			break;
		}

		case IROpcode::STORE_GLOBAL:
			gen_store_global(instr);
			break;
		case IROpcode::ADD:
		case IROpcode::SUB:
		case IROpcode::MUL:
		case IROpcode::DIV:
		case IROpcode::MOD:
		case IROpcode::BIT_AND:
		case IROpcode::BIT_OR:
		case IROpcode::BIT_XOR:
		case IROpcode::SHL:
		case IROpcode::SHR:
		case IROpcode::POW:
		case IROpcode::IN:
			gen_binary_op(instr);
			break;
		case IROpcode::BIT_NOT: {
			// ~x is x XOR -1
			int dst_vreg = instr.operands[0].reg_index();
			int src_vreg = instr.operands[1].reg_index();

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
			int dst_vreg = instr.operands[0].reg_index();
			int src_vreg = instr.operands[1].reg_index();

			int dst_offset = get_variant_stack_offset(dst_vreg);
			int src_offset = get_variant_stack_offset(src_vreg);

			if (instr.type_hint == Variant::INT) {
				emit_load_variant_int(REG_T0, REG_SP, src_offset);
				emit_sub(REG_T1, REG_ZERO, REG_T0);
				emit_li(REG_T0, Variant::INT);
				emit_store_variant_type(REG_T0, REG_SP, dst_offset);
				emit_store_variant_int(REG_T1, REG_SP, dst_offset);
				break;
			}

			if (instr.type_hint == Variant::FLOAT) {
				// Sign-bit flip, not `0.0 - x`: preserves -0.0 and NaN payloads.
				emit_load_variant_int(REG_T0, REG_SP, src_offset);
				emit_li(REG_T1, 1);
				emit_slli(REG_T1, REG_T1, 63);
				emit_xor(REG_T1, REG_T0, REG_T1);
				emit_li(REG_T0, Variant::FLOAT);
				emit_store_variant_type(REG_T0, REG_SP, dst_offset);
				emit_store_variant_int(REG_T1, REG_SP, dst_offset);
				break;
			}

			emit_variant_eval_unary(dst_offset, src_offset, 10); // OP_NEGATE
			break;
		}

		case IROpcode::CMP_EQ:
		case IROpcode::CMP_NEQ:
		case IROpcode::CMP_LT:
		case IROpcode::CMP_LTE:
		case IROpcode::CMP_GT:
		case IROpcode::CMP_GTE:
			gen_comparison(instr);
			break;
		case IROpcode::AND: {
			int dst_vreg = instr.operands[0].reg_index();
			int lhs_vreg = instr.operands[1].reg_index();
			int rhs_vreg = instr.operands[2].reg_index();

			int dst_offset = get_variant_stack_offset(dst_vreg);
			int lhs_offset = get_variant_stack_offset(lhs_vreg);
			int rhs_offset = get_variant_stack_offset(rhs_vreg);

			emit_variant_eval(dst_offset, lhs_offset, rhs_offset, 20); // OP_AND
			break;
		}

		case IROpcode::OR: {
			int dst_vreg = instr.operands[0].reg_index();
			int lhs_vreg = instr.operands[1].reg_index();
			int rhs_vreg = instr.operands[2].reg_index();

			int dst_offset = get_variant_stack_offset(dst_vreg);
			int lhs_offset = get_variant_stack_offset(lhs_vreg);
			int rhs_offset = get_variant_stack_offset(rhs_vreg);

			emit_variant_eval(dst_offset, lhs_offset, rhs_offset, 21); // OP_OR
			break;
		}

		case IROpcode::NOT: {
			int dst_vreg = instr.operands[0].reg_index();
			int src_vreg = instr.operands[1].reg_index();

			int dst_offset = get_variant_stack_offset(dst_vreg);
			int src_offset = get_variant_stack_offset(src_vreg);

			emit_variant_eval_unary(dst_offset, src_offset, 23);
			break;
		}

		case IROpcode::BRANCH_ZERO: {
			int vreg = instr.operands[0].reg_index();
			int offset = get_variant_stack_offset(vreg);
			auto [base, off] = variant_source(vreg, offset, REG_T0);
			emit_variant_truthy(REG_T2, off, instr.type_hint, base);
			mark_label_use(text(instr.operands[1]), m_code.size());
			emit_beq(REG_T2, REG_ZERO, 0);
			break;
		}

		case IROpcode::BRANCH_NOT_ZERO: {
			int vreg = instr.operands[0].reg_index();
			int offset = get_variant_stack_offset(vreg);
			auto [base, off] = variant_source(vreg, offset, REG_T0);
			emit_variant_truthy(REG_T2, off, instr.type_hint, base);
			mark_label_use(text(instr.operands[1]), m_code.size());
			emit_bne(REG_T2, REG_ZERO, 0);
			break;
		}

		case IROpcode::BRANCH_EQ:
		case IROpcode::BRANCH_NEQ:
		case IROpcode::BRANCH_LT:
		case IROpcode::BRANCH_LTE:
		case IROpcode::BRANCH_GT:
		case IROpcode::BRANCH_GTE:
			gen_fused_branch(instr);
			break;
		case IROpcode::JUMP:
			mark_label_use(text(instr.operands[0]), m_code.size());
			emit_jal(REG_ZERO, 0);
			break;

		case IROpcode::AWAIT:
			gen_await(instr);
			break;

		case IROpcode::SWITCH:
			gen_switch(instr);
			break;
		case IROpcode::RETURN: {
			// Skip copy if return forwarding already wrote through a0
			if (m_fn.return_value_written) {
				m_fn.return_value_written = false;
			} else if (m_fn.variant_offsets.find(IRFunction::RETURN_REGISTER) != m_fn.variant_offsets.end()) {
				int src_offset = get_variant_stack_offset(IRFunction::RETURN_REGISTER);
				emit_load_return_pointer();
				emit_variant_move(REG_A0, 0, REG_SP, src_offset, REG_T0);
			}

			// After retval written; t0-t5 free.
			if (m_profiling_index >= 0) {
				emit_profiling_exit();
			}
			if (m_debug_index >= 0) {
				emit_debug_exit();
			}

			if (m_fn.saves_return_address) {
				emit_ld(REG_RA, REG_SP, SAVED_RA_OFFSET);
			}

			if (m_fn.stack_frame_size > 0) {
				emit_add_offset(REG_SP, REG_SP, m_fn.stack_frame_size);
			}

			emit_ret();
			break;
		}

		case IROpcode::ARRAY_APPEND: {
			const int result_offset = get_variant_stack_offset(instr.operands[0].reg_index());
			const int array_offset = get_variant_stack_offset(instr.operands[1].reg_index());
			const int value_offset = get_variant_stack_offset(instr.operands[2].reg_index());

			spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3});

			emit_li(REG_A0, array_op(Array_Op::PUSH_BACK));
			emit_container_handle(REG_A1, instr.operands[1].reg_index(), array_offset);
			emit_li(REG_A2, 0);
			emit_add_offset(REG_A3, REG_SP, value_offset);
			emit_li(REG_A7, ECALL_ARRAY_OPS);
			emit_ecall();

			// append() returns nil
			emit_li(REG_T0, Variant::NIL);
			emit_store_variant_type(REG_T0, REG_SP, result_offset);
			break;
		}

		case IROpcode::DICT_SET: {
			const int dict_offset = get_variant_stack_offset(instr.operands[0].reg_index());
			const int key_offset = get_variant_stack_offset(instr.operands[1].reg_index());
			const int value_offset = get_variant_stack_offset(instr.operands[2].reg_index());

			spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3});

			emit_li(REG_A0, dictionary_op(Dictionary_Op::SET));
			emit_container_handle(REG_A1, instr.operands[0].reg_index(), dict_offset);
			emit_add_offset(REG_A2, REG_SP, key_offset);
			emit_add_offset(REG_A3, REG_SP, value_offset);
			emit_li(REG_A7, ECALL_DICTIONARY_OPS);
			emit_ecall();
			break;
		}

		case IROpcode::DICT_GET_CONST:
		case IROpcode::DICT_SET_CONST:
		case IROpcode::DICT_HAS_CONST:
			gen_dict_const(instr);
			break;

		case IROpcode::STRUCT_CHECK:
			gen_struct_check(instr);
			break;

		case IROpcode::ARRAY_GET:
		case IROpcode::ARRAY_SET: {
			const bool is_set = instr.opcode == IROpcode::ARRAY_SET;
			const int array_operand = is_set ? 0 : 1;
			const int index_operand = is_set ? 1 : 2;
			const int value_operand = is_set ? 2 : 0;

			const int array_offset = get_variant_stack_offset(instr.operands[array_operand].reg_index());
			const int index_offset = get_variant_stack_offset(instr.operands[index_operand].reg_index());
			const int value_offset = get_variant_stack_offset(instr.operands[value_operand].reg_index());

			spill_around_syscall({REG_A0, REG_A1, REG_A2});

			const int index_vreg = instr.operands[index_operand].reg_index();
			int64_t constant_index = 0;
			const bool negative_constant_get = !is_set &&
				constant_int(index_vreg, constant_index) && constant_index < 0;
			emit_array_element_access(is_set, array_offset, index_offset, value_offset,
				m_fn.nonnegative.count(index_vreg) != 0,
				negative_constant_get,
				instr.operands[array_operand].reg_index(), index_vreg);
			break;
		}

		case IROpcode::VCALL:
			gen_vcall(instr);
			break;
		case IROpcode::CONSTRUCT:
			gen_construct(instr);
			break;
		case IROpcode::CALL:
			gen_call(instr);
			break;
		case IROpcode::CALL_HOSTED:
			gen_call_hosted(instr);
			break;
		// Rect2/Plane: four contiguous real_t, same payload as Vector4.
		case IROpcode::MAKE_VECTOR2:
		case IROpcode::MAKE_VECTOR3:
		case IROpcode::MAKE_VECTOR4:
		case IROpcode::MAKE_RECT2:
		case IROpcode::MAKE_PLANE: {
			int num_components = (instr.opcode == IROpcode::MAKE_VECTOR2) ? 2 :
								 (instr.opcode == IROpcode::MAKE_VECTOR3) ? 3 : 4;

			if (instr.operands.size() != static_cast<size_t>(1 + num_components)) {
				throw CompilerException(ErrorType::RISCV_codegen_ERROR, "MAKE_VECTOR requires correct number of operands");
			}

			int result_vreg = instr.operands[0].reg_index();
			int result_offset = get_variant_stack_offset(result_vreg);

			int variant_type = (instr.opcode == IROpcode::MAKE_VECTOR2) ? Variant::VECTOR2 :
							   (instr.opcode == IROpcode::MAKE_VECTOR3) ? Variant::VECTOR3 :
							   (instr.opcode == IROpcode::MAKE_RECT2) ? Variant::RECT2 :
							   (instr.opcode == IROpcode::MAKE_PLANE) ? Variant::PLANE : Variant::VECTOR4;
			emit_li(REG_T0, variant_type);
			emit_sw(REG_T0, REG_SP, result_offset);

			for (int i = 0; i < num_components; i++) {
				int comp_vreg = instr.operands[1 + i].reg_index();
				int comp_offset = get_variant_stack_offset(comp_vreg);
				emit_variant_component_to_real(comp_offset, result_offset, real_offset(i));
			}

			break;
		}

		case IROpcode::MAKE_VECTOR2I:
		case IROpcode::MAKE_VECTOR3I:
		case IROpcode::MAKE_VECTOR4I:
		case IROpcode::MAKE_RECT2I: {
			int num_components = (instr.opcode == IROpcode::MAKE_VECTOR2I) ? 2 :
								 (instr.opcode == IROpcode::MAKE_VECTOR3I) ? 3 : 4;

			if (instr.operands.size() != static_cast<size_t>(1 + num_components)) {
				throw CompilerException(ErrorType::RISCV_codegen_ERROR, "MAKE_VECTORnI requires correct number of operands");
			}

			int result_vreg = instr.operands[0].reg_index();
			int result_offset = get_variant_stack_offset(result_vreg);

			int variant_type = (instr.opcode == IROpcode::MAKE_VECTOR2I) ? Variant::VECTOR2I :
							   (instr.opcode == IROpcode::MAKE_VECTOR3I) ? Variant::VECTOR3I :
							   (instr.opcode == IROpcode::MAKE_RECT2I) ? Variant::RECT2I : Variant::VECTOR4I;
			emit_li(REG_T0, variant_type);
			emit_sw(REG_T0, REG_SP, result_offset);

			for (int i = 0; i < num_components; i++) {
				int comp_vreg = instr.operands[1 + i].reg_index();
				int comp_offset = get_variant_stack_offset(comp_vreg);

				emit_variant_component_to_int(comp_offset, result_offset, int_offset(i));
			}

			break;
		}

		case IROpcode::MAKE_COLOR: {
			if (instr.operands.size() != 5) {
				throw CompilerException(ErrorType::RISCV_codegen_ERROR, "MAKE_COLOR requires 5 operands");
			}

			int result_vreg = instr.operands[0].reg_index();
			int result_offset = get_variant_stack_offset(result_vreg);

			emit_li(REG_T0, Variant::COLOR);
			emit_sw(REG_T0, REG_SP, result_offset);

			for (int i = 0; i < 4; i++) {
				int comp_vreg = instr.operands[1 + i].reg_index();
				int comp_offset = get_variant_stack_offset(comp_vreg);
				emit_variant_component_to_real(comp_offset, result_offset, real_offset(i));
			}

			break;
		}

		case IROpcode::PRINT:
			gen_print(instr);
			break;
		case IROpcode::THROW:
			gen_throw(instr);
			break;
		case IROpcode::GLOBAL_CALL:
			// Inline global forms use t3-t5/fa3 as their argument/result bank.
			// Canonical slots are current, so dropping the basic-block cache is free.
			clear_block_value_state();
			emit_global_call(instr);
			break;

		case IROpcode::MAKE_ARRAY:
			gen_make_array(instr);
			break;
		case IROpcode::MAKE_DICTIONARY:
			gen_make_dictionary(instr);
			break;
		case IROpcode::MAKE_DICTIONARY_KEYED:
			gen_make_dictionary_keyed(instr);
			break;
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
			gen_make_packed_array(instr);
			break;
		case IROpcode::VGET_INLINE:
			gen_vget_inline(instr);
			break;
		case IROpcode::VGET:
			gen_vget(instr);
			break;
		case IROpcode::VSET:
			gen_vset(instr);
			break;
		case IROpcode::VSET_INLINE:
			gen_vset_inline(instr);
			break;
		case IROpcode::CALL_SYSCALL:
			gen_call_syscall(instr);
			break;
		case IROpcode::GET_NODE:
			gen_get_node(instr);
			break;
		case IROpcode::MAKE_CALLABLE:
			gen_make_callable(instr);
			break;
		case IROpcode::LOAD_RESOURCE:
			gen_load_resource(instr);
			break;
		case IROpcode::LOAD_RESOURCE_VAR:
			gen_load_resource_var(instr);
			break;
		// No default: new opcodes must be listed (compile error otherwise)
	}
}

void RISCVCodeGen::gen_function(const IRFunction& func) {
	FunctionStateGuard function_state(*this);
	m_fn.is_member_initializer = func.name == "__init_members";

	// Reset per function; first body line always owes a break.
	m_break_line = 0;
	m_break_pending = false;

	plan_frame(func);
	emit_prologue(func);

	const bool emits_trusted_entry = m_trusted_internal_entries.count(func.name) != 0;
	std::vector<int> trusted_parameter_destinations(m_fn.num_params);
	for (size_t i = 0; i < m_fn.num_params; i++) {
		trusted_parameter_destinations[i] = int(i);
	}
	size_t coerce_prefix = 0;
	if (emits_trusted_entry) {
		while (coerce_prefix < func.instructions.size()) {
			const IRInstruction& instr = func.instructions[coerce_prefix];
			if (instr.opcode != IROpcode::COERCE || instr.operands.size() < 2) {
				break;
			}
			const int src = instr.operands[1].reg_index();
			if (src < 0 || size_t(src) >= m_fn.num_params) {
				break;
			}
			trusted_parameter_destinations[size_t(src)] = instr.operands[0].reg_index();
			coerce_prefix++;
		}
	}
	const bool has_trusted_entry = emits_trusted_entry && coerce_prefix > 0;
	const std::string trusted_label = ".typed$" + func.name;
	const std::string body_label = ".typed_body$" + func.name;
	std::vector<std::pair<std::string, std::string>> debug_labels(func.debug_locals.size());
	if (m_debug && m_debug_index >= 0) {
		for (size_t i = 0; i < func.debug_locals.size(); i++) {
			const IRFunction::DebugLocal &local = func.debug_locals[i];
			if (local.name == "self" || local.register_num < 0) continue;
			auto offset = m_fn.variant_offsets.find(local.register_num);
			if (offset == m_fn.variant_offsets.end()) continue;
			debug_labels[i] = {gen_local_label("debug_begin"), gen_local_label("debug_end")};
			PendingDebugVariable pending;
			pending.record.function_index = uint32_t(m_debug_index);
			pending.record.name = local.name;
			pending.record.type = local.type_hint == IRInstruction::TypeHint_NONE
					? -1 : int32_t(local.type_hint);
			pending.record.storage = DebugStorage::FRAME;
			pending.record.offset = offset->second;
			pending.begin_label = debug_labels[i].first;
			pending.end_label = debug_labels[i].second;
			m_pending_debug_variables.push_back(std::move(pending));
		}
	}

	const bool elision_in_effect = std::find(m_fn.elided_scopes.begin(),
		m_fn.elided_scopes.end(), true) != m_fn.elided_scopes.end();

	for (size_t instr_idx = 0; instr_idx < func.instructions.size(); instr_idx++) {
		if (has_trusted_entry && instr_idx == coerce_prefix) {
			mark_label_use(body_label, m_code.size());
			emit_jal(REG_ZERO, 0);
			clear_block_value_state();
			define_label(trusted_label);
			emit_prologue(func, &trusted_parameter_destinations);
			define_label(body_label);
			clear_block_value_state();
		}
		for (size_t i = 0; i < func.debug_locals.size(); i++) {
			if (!debug_labels[i].first.empty() && func.debug_locals[i].begin_instruction == instr_idx) {
				set_label(label_id(debug_labels[i].first), m_code.size());
			}
			if (!debug_labels[i].second.empty() && func.debug_locals[i].end_instruction == instr_idx) {
				set_label(label_id(debug_labels[i].second), m_code.size());
			}
		}
		if (m_fn.unmaterialized_imm[instr_idx]) {
			m_fn.current_instr_idx++;
			continue;
		}

		m_fn.chained_vreg = m_fn.next_chained_vreg;
		m_fn.next_chained_vreg = -1;
		m_fn.keep_int_in_reg = m_fn.int_kept_in_reg[instr_idx]
			? func.instructions[instr_idx].operands[0].reg_index() : -1;

		m_fn.forward_return = m_fn.forward_to_return[instr_idx];
		m_fn.current_instr_idx++;

		const IRInstruction& instr = func.instructions[instr_idx];
		int moved_tag = IRInstruction::TypeHint_NONE;
		if (instr.opcode == IROpcode::MOVE && instr.operands.size() >= 2) {
			const int src = instr.operands[1].reg_index();
			if (src >= 0 && size_t(src) < m_fn.known_tags.size()) {
				moved_tag = m_fn.known_tags[size_t(src)];
			}
		}
		const int instruction_dst = ir_destination_register(instr);
		const bool caches_scalar_result =
			(instr.opcode == IROpcode::ADD || instr.opcode == IROpcode::SUB ||
			 instr.opcode == IROpcode::MUL || instr.opcode == IROpcode::DIV ||
			 instr.opcode == IROpcode::MOD || instr.opcode == IROpcode::BIT_AND ||
			 instr.opcode == IROpcode::BIT_OR || instr.opcode == IROpcode::BIT_XOR ||
			 instr.opcode == IROpcode::SHL || instr.opcode == IROpcode::SHR) &&
			(instr.type_hint == Variant::INT || instr.type_hint == Variant::FLOAT);
		if (!caches_scalar_result) {
			invalidate_cached_vreg(instruction_dst);
		}
		const bool typed_arithmetic = ir_has_effect(instr.opcode, IR_ARITHMETIC) &&
			instr.opcode != IROpcode::POW && instr.opcode != IROpcode::IN &&
			(instr.type_hint == Variant::INT || instr.type_hint == Variant::FLOAT ||
			 TypeHintUtils::is_vector(instr.type_hint));
		if (!typed_arithmetic && instruction_dst >= 0 &&
			size_t(instruction_dst) < m_fn.known_tags.size())
		{
			m_fn.known_tags[size_t(instruction_dst)] = IRInstruction::TypeHint_NONE;
		}
		record_line(instr.line);

		// Break deferred past LABELs: above a label, a loop back-edge skips it.
		if (instr.line > 0 && instr.line != m_break_line) {
			m_break_line = instr.line;
			m_break_pending = m_debug_step_points || m_breakpoints.count(uint32_t(instr.line)) != 0;
		}
		if (m_break_pending && instr.opcode != IROpcode::LABEL) {
			m_break_pending = false;
			if (instr.opcode == IROpcode::BREAKPOINT) {
				// Coalesce: one stop, reported as installed.
				const bool requested = m_breakpoints.count(uint32_t(m_break_line)) != 0;
				emit_breakpoint(m_break_line, requested, true, true);
				continue;
			}
			const bool user_stop = m_breakpoints.count(uint32_t(m_break_line)) != 0;
			emit_breakpoint(m_break_line, user_stop, user_stop);
		}

		m_fn.ecall_refused = elision_in_effect && !instruction_may_ecall(instr);
		gen_instruction(instr);
		m_fn.ecall_refused = false;

		const int dst = ir_destination_register(instr);
		switch (instr.opcode) {
			case IROpcode::LOAD_IMM: note_known_tag(dst, Variant::INT); break;
			case IROpcode::LOAD_FLOAT_IMM: note_known_tag(dst, Variant::FLOAT); break;
			case IROpcode::LOAD_BOOL:
			case IROpcode::TYPE_TEST:
		case IROpcode::TYPE_TEST_MASK: note_known_tag(dst, Variant::BOOL); break;
			case IROpcode::TRAIT_TEST: note_known_tag(dst, Variant::BOOL); break;
			case IROpcode::LOAD_NIL: note_known_tag(dst, Variant::NIL); break;
			case IROpcode::TYPE_OF: note_known_tag(dst, Variant::INT); break;
			case IROpcode::CONVERT:
			case IROpcode::COERCE: note_known_tag(dst, instr.type_hint); break;
			case IROpcode::LOAD_GLOBAL:
				if (instr.operands.size() >= 2) {
					note_known_tag(dst, m_globals[size_t(instr.operands[1].immediate())].value_type);
				}
				break;
			case IROpcode::MOVE:
				if (moved_tag != IRInstruction::TypeHint_NONE) note_known_tag(dst, moved_tag);
				break;
			default: break;
		}
	}
	for (size_t i = 0; i < func.debug_locals.size(); i++) {
		if (!debug_labels[i].first.empty() && func.debug_locals[i].begin_instruction >= func.instructions.size()) {
			set_label(label_id(debug_labels[i].first), m_code.size());
		}
		if (!debug_labels[i].second.empty() && func.debug_locals[i].end_instruction >= func.instructions.size()) {
			set_label(label_id(debug_labels[i].second), m_code.size());
		}
	}

	// Emitted after body; skipped if optimizer removed all awaits.
	if (m_fn.is_coroutine && !m_fn.await_states.empty()) {
		emit_coroutine_resume_entry(func);
	}
}

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
	// B-type immediate layout
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
	// Low 12 bits must be zero — U-type encodes only bits [31:12]
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

void RISCVCodeGen::emit_li(uint8_t rd, int64_t imm) {
	if (fits_in_signed(imm, I_TYPE_IMM_BITS)) {
		emit_i_type(0x13, rd, 0, REG_ZERO, static_cast<int32_t>(imm));
	} else if (imm >= INT32_MIN && imm <= INT32_MAX) {
		int32_t imm32 = static_cast<int32_t>(imm);
		int32_t upper = (imm32 + 0x800) >> 12;
		emit_u_type(0x37, rd, upper << 12);

		// Sign-extend the low 12 bits for a valid I-type immediate
		int32_t lower = ((imm32 & 0xFFF) ^ 0x800) - 0x800;
		if (lower != 0 || upper == 0) {
			emit_i_type(0x13, rd, 0, rd, lower);
		}
	} else {
		// 64-bit: auipc+ld from constant pool
		size_t const_index = add_constant(imm);
		std::string label = ".LC" + std::to_string(const_index);

		size_t auipc_offset = m_code.size();
		mark_label_use(label, auipc_offset);
		emit_u_type(0x17, rd, 0);
		emit_ld(rd, rd, 0);
	}
}

void RISCVCodeGen::emit_mv(uint8_t rd, uint8_t rs) {
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

	// rd == base would clobber the base before the add
	const uint8_t temp = (rd == base) ? REG_WIDE_SCRATCH : rd;
	emit_li(temp, offset);
	emit_add(rd, base, temp);
}

bool RISCVCodeGen::opcode_clobbers_abi_registers(IROpcode op) {
	switch (op) {
		// Frame-local only: no syscall, a0-a7 and ra survive
		case IROpcode::LOAD_IMM:
		case IROpcode::LOAD_FLOAT_IMM:
		case IROpcode::LOAD_BOOL:
		case IROpcode::LOAD_NIL:
		case IROpcode::LOAD_GLOBAL:
		case IROpcode::MOVE:
		case IROpcode::TYPE_TEST:
		case IROpcode::TYPE_TEST_MASK:
		case IROpcode::TYPE_OF:
		case IROpcode::MAKE_SCOPED:
		case IROpcode::LABEL:
		case IROpcode::SWITCH:
		case IROpcode::JUMP:
		case IROpcode::RETURN:
			return false;

		case IROpcode::BREAKPOINT: // saves/restores a0/a7 around the ecall

			return false;

		case IROpcode::SCOPE_MARK:
		case IROpcode::SCOPE_RELEASE:
			return true;

		// May syscall or call; must hold for the slow path too
		case IROpcode::LOAD_STRING:
		case IROpcode::LOAD_STRING_AS:
		case IROpcode::STORE_GLOBAL:
		case IROpcode::CONVERT:
		case IROpcode::COERCE:
		case IROpcode::POW:
		case IROpcode::IN:
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
		case IROpcode::BRANCH_ZERO:
		case IROpcode::BRANCH_NOT_ZERO:
		case IROpcode::BRANCH_EQ:
		case IROpcode::BRANCH_NEQ:
		case IROpcode::BRANCH_LT:
		case IROpcode::BRANCH_LTE:
		case IROpcode::BRANCH_GT:
		case IROpcode::BRANCH_GTE:
		case IROpcode::ARRAY_APPEND:
		case IROpcode::ARRAY_GET:
		case IROpcode::ARRAY_SET:
		case IROpcode::DICT_SET:
		case IROpcode::DICT_GET_CONST:
		case IROpcode::DICT_SET_CONST:
		case IROpcode::DICT_HAS_CONST:
		case IROpcode::STRUCT_CHECK:
		case IROpcode::AWAIT:
		case IROpcode::CALL:
		case IROpcode::CALL_HOSTED:
		case IROpcode::CALL_SYSCALL:
		case IROpcode::TRAIT_TEST:
		case IROpcode::GET_NODE:
		case IROpcode::LOAD_RESOURCE:
		case IROpcode::LOAD_RESOURCE_VAR:
		case IROpcode::MAKE_CALLABLE:
		case IROpcode::CONSTRUCT:
		case IROpcode::VCALL:
		case IROpcode::VGET:
		case IROpcode::VSET:
		case IROpcode::PRINT:
		case IROpcode::THROW:
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
		case IROpcode::MAKE_DICTIONARY_KEYED:
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

// CFG successors; over-approximation is safe (extra edges only add conservatism).
std::vector<std::vector<size_t>> RISCVCodeGen::build_successors(const IRFunction& func) {
	const size_t n = func.instructions.size();
	std::unordered_map<uint32_t, size_t> label_index;
	for (size_t i = 0; i < n; i++) {
		if (ir_has_effect(func.instructions[i].opcode, IR_LABEL)) {
			label_index[func.instructions[i].operands.at(0).string_id] = i;
		}
	}

	std::vector<std::vector<size_t>> successors(n);
	for (size_t i = 0; i < n; i++) {
		const IRInstruction& instr = func.instructions[i];
		for (const auto& operand : instr.operands) {
			if (operand.type != IRValue::Type::LABEL || ir_has_effect(instr.opcode, IR_LABEL)) {
				continue;
			}
			auto it = label_index.find(operand.string_id);
			if (it != label_index.end()) {
				successors[i].push_back(it->second);
			}
		}
		if (!ir_has_effect(instr.opcode, IR_TERMINATOR) && i + 1 < n) {
			successors[i].push_back(i + 1);
		}
	}
	return successors;
}

std::vector<bool> RISCVCodeGen::find_live_parameters(const IRFunction& func) {
	// Backward liveness to a fixed point; loops can carry reads past writes.
	const size_t count = func.parameters.size();
	std::vector<bool> live_at_entry(count, false);
	if (count == 0 || func.instructions.empty()) {
		return live_at_entry;
	}

	const size_t n = func.instructions.size();

	const std::vector<std::vector<size_t>> successors = build_successors(func);

	// live_in[i][p]: parameter p read before written on some path from i
	std::vector<std::vector<bool>> live_in(n, std::vector<bool>(count, false));
	std::vector<int> reads;
	std::vector<bool> live_out(count, false);

	for (bool changed = true; changed; ) {
		changed = false;
		for (size_t rev = 0; rev < n; rev++) {
			const size_t i = n - 1 - rev;
			const IRInstruction& instr = func.instructions[i];

			live_out.assign(count, false);
			for (size_t succ : successors[i]) {
				for (size_t p = 0; p < count; p++) {
					live_out[p] = live_out[p] || live_in[succ][p];
				}
			}

			// Read before write in the same instruction
			const int dst = ir_destination_register(instr);
			if (dst >= 0 && static_cast<size_t>(dst) < count) {
				live_out[dst] = false;
			}
			reads.clear();
			ir_collect_read_registers(instr, reads);
			for (int reg : reads) {
				if (reg >= 0 && static_cast<size_t>(reg) < count) {
					live_out[reg] = true;
				}
			}

			if (live_out != live_in[i]) {
				live_in[i] = live_out;
				changed = true;
			}
		}
	}

	return live_in[0];
}

bool RISCVCodeGen::int_op_is_commutative(IROpcode op) {
	switch (op) {
		case IROpcode::ADD:
		case IROpcode::MUL:
		case IROpcode::BIT_AND:
		case IROpcode::BIT_OR:
		case IROpcode::BIT_XOR:
			return true;
		default:
			return false;
	}
}

// True when op(_, value) has an exact I-type encoding.
bool RISCVCodeGen::int_op_takes_immediate(IROpcode op, int64_t value) {
	switch (op) {
		case IROpcode::ADD:
		case IROpcode::BIT_AND:
		case IROpcode::BIT_OR:
		case IROpcode::BIT_XOR:
			return fits_in_signed(value, I_TYPE_IMM_BITS);
		case IROpcode::SUB:
			return fits_in_signed(-value, I_TYPE_IMM_BITS);
		case IROpcode::SHL:
		case IROpcode::SHR:
			return value >= 0 && value < 64;
		case IROpcode::MUL:
			return value > 0 && (uint64_t(value) & (uint64_t(value) - 1)) == 0;
		default:
			return false;
	}
}

bool RISCVCodeGen::int_compare_takes_immediate(IROpcode op, int64_t value,
	bool constant_on_left)
{
	if (constant_on_left) {
		switch (op) {
			case IROpcode::CMP_LT: op = IROpcode::CMP_GT; break;
			case IROpcode::CMP_LTE: op = IROpcode::CMP_GTE; break;
			case IROpcode::CMP_GT: op = IROpcode::CMP_LT; break;
			case IROpcode::CMP_GTE: op = IROpcode::CMP_LTE; break;
			case IROpcode::BRANCH_LT: op = IROpcode::BRANCH_GT; break;
			case IROpcode::BRANCH_LTE: op = IROpcode::BRANCH_GTE; break;
			case IROpcode::BRANCH_GT: op = IROpcode::BRANCH_LT; break;
			case IROpcode::BRANCH_GTE: op = IROpcode::BRANCH_LTE; break;
			default: break;
		}
	}
	switch (op) {
		case IROpcode::CMP_EQ: case IROpcode::CMP_NEQ:
		case IROpcode::BRANCH_EQ: case IROpcode::BRANCH_NEQ:
			return value >= -2047 && value <= 2048;
		case IROpcode::CMP_LT: case IROpcode::CMP_GTE:
		case IROpcode::BRANCH_LT: case IROpcode::BRANCH_GTE:
			return fits_in_signed(value, I_TYPE_IMM_BITS);
		case IROpcode::CMP_LTE: case IROpcode::CMP_GT:
		case IROpcode::BRANCH_LTE: case IROpcode::BRANCH_GT:
			return value != INT64_MAX && fits_in_signed(value + 1, I_TYPE_IMM_BITS);
		default:
			return false;
	}
}

bool RISCVCodeGen::constant_int(int vreg, int64_t& value) const {
	auto it = m_fn.const_ints.find(vreg);
	if (it == m_fn.const_ints.end()) {
		return false;
	}
	value = it->second;
	return true;
}

// Whether operand_index of a typed-INT binary op is a foldable constant.
bool RISCVCodeGen::folds_to_immediate(const IRInstruction& instr, size_t operand_index) const {
	if (instr.type_hint != Variant::INT) {
		return false;
	}
	if (instr.operands.size() < 3) {
		return false;
	}
	const bool fused = ir_has_effect(instr.opcode, IR_FUSED_BRANCH);
	const size_t lhs_index = fused ? 0 : 1;
	const size_t rhs_index = fused ? 1 : 2;
	if (operand_index != lhs_index && operand_index != rhs_index) {
		return false;
	}
	if (instr.operands[lhs_index].type != IRValue::Type::REGISTER ||
		instr.operands[rhs_index].type != IRValue::Type::REGISTER)
	{
		return false;
	}
	const bool comparison = ir_has_effect(instr.opcode, IR_COMPARISON) || fused;
	if (!comparison && instr.operands[0].type != IRValue::Type::REGISTER) {
		return false;
	}
	switch (instr.opcode) {
		case IROpcode::CMP_EQ: case IROpcode::CMP_NEQ:
		case IROpcode::CMP_LT: case IROpcode::CMP_LTE:
		case IROpcode::CMP_GT: case IROpcode::CMP_GTE:
		case IROpcode::BRANCH_EQ: case IROpcode::BRANCH_NEQ:
		case IROpcode::BRANCH_LT: case IROpcode::BRANCH_LTE:
		case IROpcode::BRANCH_GT: case IROpcode::BRANCH_GTE:
			break;
		case IROpcode::ADD:
		case IROpcode::SUB:
		case IROpcode::MUL:
		case IROpcode::BIT_AND:
		case IROpcode::BIT_OR:
		case IROpcode::BIT_XOR:
		case IROpcode::SHL:
		case IROpcode::SHR:
			break;
		default:
			return false;
	}
	int64_t value;
	if (!constant_int(instr.operands[operand_index].reg_index(), value)) {
		return false;
	}
	const size_t other_index = operand_index == lhs_index ? rhs_index : lhs_index;
	const int other = instr.operands[other_index].reg_index();
	if (other == instr.operands[operand_index].reg_index()) {
		return false;
	}
	const bool constant_on_left = operand_index == lhs_index;
	if (comparison) {
		if (!int_compare_takes_immediate(instr.opcode, value, constant_on_left)) {
			return false;
		}
	} else if (constant_on_left && !int_op_is_commutative(instr.opcode)) {
		return false;
	} else if (!comparison && !int_op_takes_immediate(instr.opcode, value)) {
		return false;
	}
	// RHS wins when both operands fold; LHS stays in its frame slot.
	if (constant_on_left) {
		int64_t rhs_value;
		if (constant_int(other, rhs_value) &&
			(comparison ? int_compare_takes_immediate(instr.opcode, rhs_value, false)
				: int_op_takes_immediate(instr.opcode, rhs_value))) {
			return false;
		}
	}
	return true;
}

void RISCVCodeGen::plan_constants(const IRFunction& func) {
	m_fn.const_ints.clear();
	m_fn.unmaterialized_imm.assign(func.instructions.size(), false);

	// Parameters count as a prior definition.
	std::unordered_map<int, size_t> def_count;
	for (size_t i = 0; i < func.parameters.size(); i++) {
		def_count[static_cast<int>(i)] = 1;
	}
	for (const auto& instr : func.instructions) {
		const int dst = ir_destination_register(instr);
		if (dst >= 0) {
			def_count[dst]++;
		}
	}

	std::unordered_map<int, size_t> defining_instruction;
	for (size_t i = 0; i < func.instructions.size(); i++) {
		const IRInstruction& instr = func.instructions[i];
		if (instr.opcode != IROpcode::LOAD_IMM || instr.operands.size() < 2) {
			continue;
		}
		if (instr.operands[0].type != IRValue::Type::REGISTER ||
			instr.operands[1].type != IRValue::Type::IMMEDIATE) {
			continue;
		}
		if (instr.operands[1].type != IRValue::Type::IMMEDIATE) {
			continue;
		}
		const int dst = instr.operands[0].reg_index();
		if (def_count[dst] != 1) {
			continue;
		}
		m_fn.const_ints[dst] = instr.operands[1].immediate();
		defining_instruction[dst] = i;
	}

	if (m_fn.const_ints.empty()) {
		return;
	}

	// Keep the Variant if any read does not fold.
	std::unordered_set<int> materialized;
	std::vector<int> reads;
	for (const auto& instr : func.instructions) {
		reads.clear();
		ir_collect_read_registers(instr, reads);
		for (size_t index = 0; index < instr.operands.size(); index++) {
			if (instr.operands[index].type != IRValue::Type::REGISTER ||
				!ir_reads_operand(instr, index)) {
				continue;
			}
			const int vreg = instr.operands[index].reg_index();
			if (m_fn.const_ints.count(vreg) && !folds_to_immediate(instr, index)) {
				materialized.insert(vreg);
			}
		}
		// Bare RETURN reads r0 implicitly.
		for (int vreg : reads) {
			if (m_fn.const_ints.count(vreg) && instr.operands.empty()) {
				materialized.insert(vreg);
			}
		}
	}

	for (const auto& entry : defining_instruction) {
		if (!materialized.count(entry.first)) {
			m_fn.unmaterialized_imm[entry.second] = true;
		}
	}
}

// Elide the frame copy when every read of a global takes only its handle or
// payload directly from the data area.
void RISCVCodeGen::plan_global_handles(const IRFunction& func) {
	m_fn.global_handles.clear();

	// Operand positions that have a variant_source() call at the emit site.
	// Unlisted positions read the frame slot, so the copy must stay.
	const auto direct_operands = [](const IRInstruction& instr) -> std::pair<int, int> {
		if (is_typed_int_binary(instr)) {
			return { 1, 2 };
		}
		switch (instr.opcode) {
			case IROpcode::BRANCH_ZERO:
			case IROpcode::BRANCH_NOT_ZERO:
				return { 0, -1 };
			case IROpcode::ARRAY_SET:
				return { 0, 1 };  // container, index
			case IROpcode::ARRAY_GET:
				return { 1, 2 };
			case IROpcode::DICT_SET:
				return { 0, -1 };
			case IROpcode::ARRAY_APPEND:
				return { 1, -1 };
			case IROpcode::CALL_SYSCALL: {
				if (instr.operands.size() < 2 ||
					instr.operands[1].type != IRValue::Type::IMMEDIATE) {
					return { -1, -1 };
				}
				switch (instr.operands[1].immediate()) {
					case ECALL_ARRAY_SIZE:
					case ECALL_STRING_SIZE:
						return { 2, -1 };
					case ECALL_ARRAY_AT:
					case ECALL_STRING_AT:
					case ECALL_DICTIONARY_OPS:
						return { 3, -1 };
					default:
						return { -1, -1 };
				}
			}
			default:
				return { -1, -1 };
		}
	};

	const size_t n = func.instructions.size();
	std::unordered_map<int, size_t> candidates;
	std::unordered_map<int, size_t> def_count;
	for (size_t i = 0; i < func.parameters.size(); i++) {
		def_count[static_cast<int>(i)]++;
	}
	for (const auto& instr : func.instructions) {
		const int dst = ir_destination_register(instr);
		if (dst >= 0) {
			def_count[dst]++;
		}
	}

	const std::vector<std::vector<size_t>> successors = build_successors(func);

	const auto assigned_before = [&](size_t from, int64_t index) {
		const auto stores = [&](size_t node) {
			const IRInstruction& instr = func.instructions[node];
			if (instr.opcode == IROpcode::CALL || instr.opcode == IROpcode::CALL_HOSTED ||
				instr.opcode == IROpcode::AWAIT) {
				return true;
			}
			return instr.opcode == IROpcode::STORE_GLOBAL && !instr.operands.empty() &&
				instr.operands[0].type == IRValue::Type::IMMEDIATE &&
				instr.operands[0].immediate() == index;
		};

		std::vector<bool> reachable(n, false);
		std::vector<bool> tainted(n, false);
		reachable[from] = true;
		for (bool changed = true; changed; ) {
			changed = false;
			for (size_t node = 0; node < n; node++) {
				if (!reachable[node]) {
					continue;
				}
				const bool out = tainted[node] || stores(node);
				for (size_t succ : successors[node]) {
					if (!reachable[succ]) {
						reachable[succ] = true;
						changed = true;
					}
					if (out && !tainted[succ]) {
						tainted[succ] = true;
						changed = true;
					}
				}
			}
		}
		return tainted;
	};

	for (size_t i = 0; i < n; i++) {
		const IRInstruction& instr = func.instructions[i];
		if (instr.opcode != IROpcode::LOAD_GLOBAL || instr.operands.size() < 2 ||
			instr.operands[0].type != IRValue::Type::REGISTER ||
			instr.operands[1].type != IRValue::Type::IMMEDIATE) {
			continue;
		}
		const int64_t index = instr.operands[1].immediate();
		if (index < 0 || static_cast<size_t>(index) >= m_globals.size()) {
			continue;
		}
		const int dst = instr.operands[0].reg_index();
		if (dst == IRFunction::RETURN_REGISTER || def_count[dst] != 1) {
			continue;
		}
		const std::vector<bool> tainted = assigned_before(i, index);
		bool safe = true;
		std::vector<int> dst_reads;
		for (size_t j = 0; j < n && safe; j++) {
			if (!tainted[j]) {
				continue;
			}
			dst_reads.clear();
			ir_collect_read_registers(func.instructions[j], dst_reads);
			safe = std::find(dst_reads.begin(), dst_reads.end(), dst) == dst_reads.end();
		}
		if (safe) {
			candidates[dst] = static_cast<size_t>(index);
		}
	}

	if (candidates.empty()) {
		return;
	}

	// Any non-direct read disqualifies the candidate.
	std::vector<int> reads;
	for (const auto& instr : func.instructions) {
		const auto [direct_a, direct_b] = direct_operands(instr);
		reads.clear();
		ir_collect_read_registers(instr, reads);
		for (size_t index = 0; index < instr.operands.size(); index++) {
			if (instr.operands[index].type != IRValue::Type::REGISTER ||
				!ir_reads_operand(instr, index)) {
				continue;
			}
			if (static_cast<int>(index) == direct_a || static_cast<int>(index) == direct_b) {
				continue;
			}
			candidates.erase(instr.operands[index].reg_index());
		}
		if (instr.operands.empty()) {
			for (int vreg : reads) {
				candidates.erase(vreg);
			}
		}
	}

	m_fn.global_handles = std::move(candidates);
}

// Prove which vregs are non-negative so array subscripts can skip the wrap.
void RISCVCodeGen::plan_nonnegative(const IRFunction& func) {
	m_fn.nonnegative.clear();

	std::unordered_map<int, size_t> def_count;
	for (size_t i = 0; i < func.parameters.size(); i++) {
		def_count[static_cast<int>(i)]++;
	}
	for (const auto& instr : func.instructions) {
		const int dst = ir_destination_register(instr);
		if (dst >= 0) {
			def_count[dst]++;
		}
	}

	const auto known_nonnegative = [&](const IRValue& operand) {
		if (operand.type == IRValue::Type::IMMEDIATE &&
			operand.type == IRValue::Type::IMMEDIATE) {
			return operand.immediate() >= 0;
		}
		if (operand.type != IRValue::Type::REGISTER) {
			return false;
		}
		return m_fn.nonnegative.count(operand.reg_index()) != 0;
	};

	// Fixed point: a mask may be built from another mask.
	for (bool changed = true; changed; ) {
		changed = false;
		for (const auto& instr : func.instructions) {
			const int dst = ir_destination_register(instr);
			if (dst < 0 || def_count[dst] != 1 || m_fn.nonnegative.count(dst)) {
				continue;
			}
			bool nonneg = false;
			switch (instr.opcode) {
				case IROpcode::LOAD_IMM:
					nonneg = instr.operands.size() >= 2 && known_nonnegative(instr.operands[1]);
					break;
				case IROpcode::BIT_AND:
					nonneg = instr.type_hint == Variant::INT && instr.operands.size() >= 3 &&
						(known_nonnegative(instr.operands[1]) || known_nonnegative(instr.operands[2]));
					break;
				case IROpcode::SHR:
					nonneg = instr.type_hint == Variant::INT && instr.operands.size() >= 3 &&
						known_nonnegative(instr.operands[1]);
					break;
				default:
					break;
			}
			if (nonneg) {
				m_fn.nonnegative.insert(dst);
				changed = true;
			}
		}
	}
}

// Three-register int arithmetic: dst = op(lhs, rhs), payload-only.
bool RISCVCodeGen::is_typed_int_binary(const IRInstruction& instr) {
	if (instr.type_hint != Variant::INT || instr.operands.size() < 3) {
		return false;
	}
	for (size_t i = 0; i < 3; i++) {
		if (instr.operands[i].type != IRValue::Type::REGISTER) {
			return false;
		}
	}
	switch (instr.opcode) {
		case IROpcode::ADD:
		case IROpcode::SUB:
		case IROpcode::MUL:
		case IROpcode::DIV:
		case IROpcode::MOD:
		case IROpcode::BIT_AND:
		case IROpcode::BIT_OR:
		case IROpcode::BIT_XOR:
		case IROpcode::SHL:
		case IROpcode::SHR:
			return true;
		default:
			return false;
	}
}

// Keep a typed int result in REG_T2 when the sole reader is the next
// instruction and also wants an int.
void RISCVCodeGen::plan_int_chaining(const IRFunction& func) {
	const size_t n = func.instructions.size();
	m_fn.int_kept_in_reg.assign(n, false);

	std::unordered_map<int, size_t> def_count;
	std::unordered_map<int, size_t> read_count;
	for (size_t i = 0; i < func.parameters.size(); i++) {
		def_count[static_cast<int>(i)]++;
	}
	std::vector<int> reads;
	for (const auto& instr : func.instructions) {
		const int dst = ir_destination_register(instr);
		if (dst >= 0) {
			def_count[dst]++;
		}
		reads.clear();
		ir_collect_read_registers(instr, reads);
		for (int vreg : reads) {
			read_count[vreg]++;
		}
	}

	// A declared-int global produces an int without copying the Variant.
	const auto int_global = [&](int64_t index) {
		return index >= 0 && static_cast<size_t>(index) < m_globals.size() &&
			m_globals[index].value_type == Variant::INT;
	};
	const auto produces_int = [&](const IRInstruction& instr) {
		if (is_typed_int_binary(instr)) {
			return true;
		}
		return instr.opcode == IROpcode::LOAD_GLOBAL && instr.operands.size() >= 2 &&
			instr.operands[0].type == IRValue::Type::REGISTER &&
			instr.operands[1].type == IRValue::Type::IMMEDIATE &&
			int_global(instr.operands[1].immediate());
	};

	for (size_t i = 0; i + 1 < n; i++) {
		if (!produces_int(func.instructions[i])) {
			continue;
		}
		const int dst = func.instructions[i].operands[0].reg_index();
		// ABI-visible slots cannot be elided.
		if (dst < static_cast<int>(func.parameters.size()) || dst == IRFunction::RETURN_REGISTER) {
			continue;
		}
		if (def_count[dst] != 1 || read_count[dst] != 1) {
			continue;
		}
		// Skip unmaterialised LOAD_IMMs; they emit nothing.
		size_t j = i + 1;
		while (j < n && m_fn.unmaterialized_imm[j]) {
			j++;
		}
		if (j >= n) {
			continue;
		}
		const IRInstruction& next = func.instructions[j];
		if (next.opcode == IROpcode::STORE_GLOBAL) {
			if (next.operands.size() >= 2 &&
				next.operands[0].type == IRValue::Type::IMMEDIATE &&
				next.operands[1].type == IRValue::Type::REGISTER &&
				int_global(next.operands[0].immediate()) &&
				next.operands[1].reg_index() == dst) {
				m_fn.int_kept_in_reg[i] = true;
			}
			continue;
		}
		if (!is_typed_int_binary(next)) {
			continue;
		}
		const int next_lhs = next.operands[1].reg_index();
		const int next_rhs = next.operands[2].reg_index();
		if (next_lhs != dst && next_rhs != dst) {
			continue;
		}
		m_fn.int_kept_in_reg[i] = true;
	}
}

std::vector<bool> RISCVCodeGen::find_return_forwarding(const IRFunction& func) {
	// Write directly through a0 instead of to r0's slot when the next
	// instruction is RETURN and no later code reads r0.
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
			case IROpcode::LOAD_NIL:
			case IROpcode::LOAD_GLOBAL:
			case IROpcode::MOVE:
				break;
			case IROpcode::ADD:
			case IROpcode::SUB:
			case IROpcode::MUL:
			case IROpcode::DIV:
			case IROpcode::MOD:
			case IROpcode::BIT_AND:
			case IROpcode::BIT_OR:
			case IROpcode::BIT_XOR:
			case IROpcode::SHL:
			case IROpcode::SHR:
				if (instr.type_hint != Variant::INT && instr.type_hint != Variant::FLOAT) {
					continue;
				}
				break;
			default:
				continue;
		}
		if (ir_destination_register(instr) != IRFunction::RETURN_REGISTER) {
			continue;
		}
		reads.clear();
		ir_collect_read_registers(instr, reads);
		if (std::find(reads.begin(), reads.end(), IRFunction::RETURN_REGISTER) != reads.end()) {
			continue;
		}

		// RETURN follows immediately; later reads of r0 are on other paths.
		forward[i] = true;
	}

	return forward;
}

void RISCVCodeGen::emit_load_return_pointer() {
	if (m_fn.spills_return_pointer) {
		emit_ld(REG_A0, REG_SP, SAVED_A0_OFFSET);
	}
}

void RISCVCodeGen::emit_address_of_global(uint8_t rd, size_t index) {
	if (index >= m_global_count) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"Global variable " + std::to_string(index) + " is out of range");
	}
	const int64_t byte_offset = static_cast<int64_t>(m_global_slots[index]) * variant_size();
	if (byte_offset > INT32_MAX) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"Global variable " + std::to_string(index) + " is past the end of a 32-bit address space");
	}
	if (m_globals[index].is_member()) {
		emit_add_offset(rd, REG_TP, static_cast<int32_t>(byte_offset));
		return;
	}
	// Offset in relocation, not a separate addi (12-bit limit = ~85 globals)
	emit_la(rd, GLOBALS_LABEL, static_cast<int32_t>(byte_offset));
}

void RISCVCodeGen::emit_address_of_global_area(uint8_t rd) {
	emit_la(rd, GLOBALS_LABEL, 0);
}

void RISCVCodeGen::emit_address_of_init_scratch(uint8_t rd) {
	const int64_t byte_offset = static_cast<int64_t>(m_data_global_count) * variant_size();
	emit_la(rd, GLOBALS_LABEL, static_cast<int32_t>(byte_offset));
}

bool RISCVCodeGen::emits_instance_init(const IRProgram& program) const {
	return m_instance_count > 0 || program.has_member_init;
}

void RISCVCodeGen::emit_instance_init(const IRProgram& program) {
	m_profiling_index = -1;
	m_debug_index = -1;
	record_line(0, true);
	set_label(label_id(INSTANCE_INIT_LABEL), m_code.size());
	m_instance_init_offset = m_code.size();

	emit_add_offset(REG_SP, REG_SP, -16);
	emit_sd(REG_RA, REG_SP, 0);
	emit_mv(REG_TP, REG_A0);

	emit_folded_initializers(program, true);

	if (program.has_member_init) {
		emit_address_of_init_scratch(REG_A0);
		mark_label_use(MEMBER_INIT_LABEL, m_code.size());
		emit_jal(REG_RA, 0);
	}

	emit_ld(REG_RA, REG_SP, 0);
	emit_add_offset(REG_SP, REG_SP, 16);
	emit_jalr(REG_ZERO, REG_RA, 0);
}

void RISCVCodeGen::emit_la(uint8_t rd, const std::string& label, int32_t addend) {
	// auipc+addi; addend folded into relocation (not a separate addi)
	size_t auipc_offset = m_code.size();
	mark_label_use(label, auipc_offset, addend);
	emit_u_type(0x17, rd, 0);
	emit_i_type(0x13, rd, 0, rd, 0);
}

void RISCVCodeGen::emit_add(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 0, rs1, rs2, 0);
}

void RISCVCodeGen::emit_sub(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 0, rs1, rs2, 0x20);
}

void RISCVCodeGen::emit_mul(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 0, rs1, rs2, 1);
}

void RISCVCodeGen::emit_div(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 4, rs1, rs2, 1);
}

void RISCVCodeGen::emit_rem(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 6, rs1, rs2, 1);
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

void RISCVCodeGen::emit_slti(uint8_t rd, uint8_t rs, int32_t imm) {
	emit_i_type(0x13, rd, 2, rs, imm);
}

void RISCVCodeGen::emit_xori(uint8_t rd, uint8_t rs, int32_t imm) {
	emit_i_type(0x13, rd, 4, rs, imm);
}

void RISCVCodeGen::emit_andi(uint8_t rd, uint8_t rs, int32_t imm) {
	emit_i_type(0x13, rd, 7, rs, imm);
}

void RISCVCodeGen::emit_ori(uint8_t rd, uint8_t rs, int32_t imm) {
	emit_i_type(0x13, rd, 6, rs, imm);
}

void RISCVCodeGen::emit_seqz(uint8_t rd, uint8_t rs) {
	emit_i_type(0x13, rd, 3, rs, 1);
}

void RISCVCodeGen::emit_snez(uint8_t rd, uint8_t rs) {
	emit_r_type(0x33, rd, 3, REG_ZERO, rs, 0);
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
	clear_block_value_state();
	// Breakpoint saves/restores a0 itself; all others need the prologue spill.
	if (m_fn.in_function && !m_fn.spills_return_pointer && !m_emitting_breakpoint) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"System call emitted in a function whose prologue did not save the return-value pointer");
	}
	// Scope elision depends on this predicate; a false negative leaks scoped variants.
	if (m_fn.ecall_refused) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"System call emitted by an instruction that instruction_may_ecall() "
			"reported as frame-local");
	}
	emit_i_type(0x73, 0, 0, 0, 0);
}

void RISCVCodeGen::emit_ret() {
	emit_jalr(REG_ZERO, REG_RA, 0);
}

const std::string& RISCVCodeGen::text(const IRValue& value) const {
	static const std::string missing;
	return m_strings ? (*m_strings)[value.string_id] : missing;
}

uint32_t RISCVCodeGen::label_id(const std::string& name) {
	return m_label_names.intern(name);
}

void RISCVCodeGen::set_label(uint32_t id, size_t offset) {
	if (id >= m_label_offsets.size()) {
		m_label_offsets.resize(size_t(id) + 1, NO_LABEL);
	}
	m_label_offsets[id] = offset;
}

void RISCVCodeGen::define_label(const std::string& label) {
	set_label(label_id(label), m_code.size());
}

void RISCVCodeGen::define_label(const IRValue& label) {
	set_label(label.string_id, m_code.size());
}

void RISCVCodeGen::mark_label_use(const std::string& label, size_t code_offset, int32_t addend) {
	m_label_uses.push_back({ label_id(label), code_offset, addend });
}

void RISCVCodeGen::mark_label_use(const IRValue& label, size_t code_offset, int32_t addend) {
	m_label_uses.push_back({ label.string_id, code_offset, addend });
}

void RISCVCodeGen::relax_branches() {
	// Invert B-type condition by flipping funct3 low bit
	auto invert_condition = [](uint8_t funct3) -> uint8_t { return funct3 ^ 1; };

	// One scan collects every out-of-range branch, one rebuild applies them all.
	// An inserted jal only lengthens spans, so out-of-range is monotone in the
	// relaxed set: the batch converges on the same least fixed point the
	// restart-per-insert loop did. Encodings depend on that set, not on
	// discovery order. The outer loop covers neighbours pushed out of range by
	// an insertion -- two rounds in practice.
	std::vector<size_t> relaxed_uses;
	std::vector<size_t> insert_points;

	while (true) {
		relaxed_uses.clear();
		insert_points.clear();

		for (size_t use_index = 0; use_index < m_label_uses.size(); use_index++) {
			const LabelUse& use = m_label_uses[use_index];
			if (use.code_offset + 4 > m_code.size()) {
				continue;
			}

			uint32_t instr = 0;
			std::memcpy(&instr, &m_code[use.code_offset], 4);
			if ((instr & 0x7F) != 0x63) {
				continue;
			}

			if (use.label >= m_label_offsets.size() || m_label_offsets[use.label] == NO_LABEL) {
				continue;
			}
			const int64_t displacement =
				static_cast<int64_t>(m_label_offsets[use.label]) - static_cast<int64_t>(use.code_offset) + use.addend;
			if (fits_in_signed(displacement, B_TYPE_IMM_BITS)) {
				continue;
			}
			relaxed_uses.push_back(use_index);
		}

		if (relaxed_uses.empty()) {
			return;
		}

		// Rewrite as inverted branch over an inserted jal
		for (size_t use_index : relaxed_uses) {
			const size_t code_offset = m_label_uses[use_index].code_offset;
			uint32_t instr = 0;
			std::memcpy(&instr, &m_code[code_offset], 4);
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
			std::memcpy(&m_code[code_offset], &inverted, 4);

			insert_points.push_back(code_offset + 4);
		}
		std::sort(insert_points.begin(), insert_points.end());

		std::vector<uint8_t> rebuilt;
		rebuilt.reserve(m_code.size() + insert_points.size() * 4);
		size_t copied = 0;
		for (size_t point : insert_points) {
			rebuilt.insert(rebuilt.end(), m_code.begin() + static_cast<long>(copied),
				m_code.begin() + static_cast<long>(point));
			const uint32_t jal = 0x6F;
			uint8_t bytes[4];
			std::memcpy(bytes, &jal, 4);
			rebuilt.insert(rebuilt.end(), bytes, bytes + 4);
			copied = point;
		}
		rebuilt.insert(rebuilt.end(), m_code.begin() + static_cast<long>(copied), m_code.end());
		m_code = std::move(rebuilt);

		auto shift = [&insert_points](size_t offset) -> size_t {
			const size_t before = size_t(std::upper_bound(insert_points.begin(), insert_points.end(), offset)
				- insert_points.begin());
			return offset + before * 4;
		};

		for (size_t& offset : m_label_offsets) {
			if (offset != NO_LABEL) {
				offset = shift(offset);
			}
		}
		for (auto& entry : m_functions) {
			entry.second = shift(entry.second);
		}
		for (auto& entry : m_line_table.entries) {
			entry.address = uint32_t(shift(entry.address));
		}
		for (auto& use : m_label_uses) {
			use.code_offset = shift(use.code_offset);
		}
		// The relaxed use now names the jal, one instruction on.
		for (size_t use_index : relaxed_uses) {
			m_label_uses[use_index].code_offset += 4;
		}
	}
}

void RISCVCodeGen::resolve_labels() {
	for (const auto& use : m_label_uses) {
		size_t use_offset = use.code_offset;

		if (use.label >= m_label_offsets.size() || m_label_offsets[use.label] == NO_LABEL) {
			throw CompilerException(ErrorType::RISCV_codegen_ERROR,
				"Undefined label: " + m_label_names[use.label]);
		}

		const std::string& label = m_label_names[use.label];
		size_t target_offset = m_label_offsets[use.label];
		int32_t offset = static_cast<int32_t>(target_offset - use_offset) + use.addend;

		uint32_t instr;
		memcpy(&instr, &m_code[use_offset], 4);

		uint8_t opcode = instr & 0x7F;

		if (opcode == 0x63) {
			check_displacement("B-type (branch) to '" + label + "'", offset, B_TYPE_IMM_BITS);
			uint8_t funct3 = (instr >> 12) & 0x7;
			uint8_t rs1 = (instr >> 15) & 0x1F;
			uint8_t rs2 = (instr >> 20) & 0x1F;

			uint32_t imm12 = (offset >> 12) & 1;
			uint32_t imm10_5 = (offset >> 5) & 0x3F;
			uint32_t imm4_1 = (offset >> 1) & 0xF;
			uint32_t imm11 = (offset >> 11) & 1;

			instr = opcode | (imm11 << 7) | (imm4_1 << 8) | (funct3 << 12) | (rs1 << 15) | (rs2 << 20) | (imm10_5 << 25) | (imm12 << 31);
		} else if (opcode == 0x6F) {
			check_displacement("J-type (jump) to '" + label + "'", offset, J_TYPE_IMM_BITS);
			uint8_t rd = (instr >> 7) & 0x1F;

			uint32_t imm20 = (offset >> 20) & 1;
			uint32_t imm10_1 = (offset >> 1) & 0x3FF;
			uint32_t imm11 = (offset >> 11) & 1;
			uint32_t imm19_12 = (offset >> 12) & 0xFF;

			instr = opcode | (rd << 7) | (imm19_12 << 12) | (imm11 << 20) | (imm10_1 << 21) | (imm20 << 31);
		} else if (opcode == 0x17) {
			uint8_t rd = (instr >> 7) & 0x1F;

			uint32_t next_instr;
			memcpy(&next_instr, &m_code[use_offset + 4], 4);
			uint8_t next_opcode = next_instr & 0x7F;

			if (next_opcode == 0x03) {
				int32_t upper = (offset + 0x800) >> 12;
				int32_t lower = offset & 0xFFF;

				instr = opcode | (rd << 7) | ((upper & 0xFFFFF) << 12);
				memcpy(&m_code[use_offset], &instr, 4);

				uint8_t ld_rd = (next_instr >> 7) & 0x1F;
				uint8_t ld_rs1 = (next_instr >> 15) & 0x1F;
				uint8_t ld_funct3 = (next_instr >> 12) & 0x7;
				next_instr = 0x03 | (ld_rd << 7) | (ld_funct3 << 12) | (ld_rs1 << 15) | ((lower & 0xFFF) << 20);
				memcpy(&m_code[use_offset + 4], &next_instr, 4);

				continue;
			} else if (next_opcode == 0x13) {
				uint8_t addi_funct3 = (next_instr >> 12) & 0x7;
				uint8_t addi_rs1 = (next_instr >> 15) & 0x1F;

				int32_t upper = (offset + 0x800) >> 12;
				int32_t lower = offset & 0xFFF;

				if (addi_rs1 == rd && addi_funct3 == 0) {
					instr = opcode | (rd << 7) | ((upper & 0xFFFFF) << 12);
					memcpy(&m_code[use_offset], &instr, 4);

					uint8_t addi_rd = (next_instr >> 7) & 0x1F;
					next_instr = 0x13 | (addi_rd << 7) | (addi_funct3 << 12) | (addi_rs1 << 15) | ((lower & 0xFFF) << 20);
					memcpy(&m_code[use_offset + 4], &next_instr, 4);
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

	// Frame already sized; growing it now would corrupt the caller
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
	return SAVED_REG_SPACE + ((m_fn.scratch_slot_base + index) * variant_size());
}

void RISCVCodeGen::emit_variant_create_int(int stack_offset, int64_t value, uint8_t base_reg) {
	emit_li(REG_T0, 2);
	emit_store_variant_type(REG_T0, base_reg, stack_offset);
	emit_li(REG_T0, value);
	emit_store_variant_int(REG_T0, base_reg, stack_offset);
}

void RISCVCodeGen::emit_variant_create_float(int stack_offset, double value, uint8_t base_reg) {
	// v.f is always 64-bit double regardless of real_t
	int64_t bits;
	memcpy(&bits, &value, sizeof(double));

	emit_li(REG_T0, Variant::FLOAT);
	emit_store_variant_type(REG_T0, base_reg, stack_offset);

	emit_li(REG_T0, bits);
	emit_sd(REG_T0, base_reg, stack_offset + VARIANT_DATA_OFFSET);
}

void RISCVCodeGen::emit_variant_create_bool(int stack_offset, bool value, uint8_t base_reg) {
	emit_li(REG_T0, 1);
	emit_store_variant_type(REG_T0, base_reg, stack_offset);
	emit_li(REG_T0, value ? 1 : 0);
	emit_store_variant_bool(REG_T0, base_reg, stack_offset);
}

void RISCVCodeGen::emit_variant_create_string(int stack_offset, int string_idx, int variant_type) {

	if (!m_string_constants || string_idx < 0 || string_idx >= static_cast<int>(m_string_constants->size())) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Invalid string constant index: " + std::to_string(string_idx));
	}

	const std::string& str = (*m_string_constants)[string_idx];
	int str_len = static_cast<int>(str.length());

	spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3});

	const int total_space = 16;

	emit_add_offset(REG_SP, REG_SP, -total_space);

	// VCREATE method 1: struct { char*, size_t }
	emit_la(REG_T0, rodata_string(str));
	emit_sd(REG_T0, REG_SP, 0);
	emit_li(REG_T0, str_len);
	emit_sd(REG_T0, REG_SP, 8);

	int adjusted_dst_offset = stack_offset + total_space;
	emit_add_offset(REG_A0, REG_SP, adjusted_dst_offset);
	emit_li(REG_A1, variant_type);
	emit_li(REG_A2, 1);
	emit_mv(REG_A3, REG_SP);
	emit_li(REG_A7, ECALL_VCREATE);
	emit_ecall();

	emit_add_offset(REG_SP, REG_SP, total_space);
}

void RISCVCodeGen::emit_vcreate_syscall(int variant_type, int method, uint8_t data_ptr_reg, int result_offset) {
	spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3});
	emit_add_offset(REG_A0, REG_SP, result_offset);
	emit_li(REG_A1, variant_type);
	emit_li(REG_A2, method);
	if (data_ptr_reg != REG_A3) {
		emit_mv(REG_A3, data_ptr_reg);
	}
	emit_li(REG_A7, ECALL_VCREATE);
	emit_ecall();
}

void RISCVCodeGen::emit_variant_create_empty_array(int stack_offset) {
	emit_vcreate_syscall(Variant::ARRAY, 0, REG_ZERO, stack_offset);
}

void RISCVCodeGen::emit_variant_create_empty_dictionary(int stack_offset) {
	emit_vcreate_syscall(Variant::DICTIONARY, 0, REG_ZERO, stack_offset);
}

void RISCVCodeGen::emit_variant_move(uint8_t dst_base, int32_t dst_offset, uint8_t src_base, int32_t src_offset, uint8_t tmp_reg) {
	for (int i = 0; i < m_layout.variant_words(); i++) {
		emit_ld(tmp_reg, src_base, src_offset + i * 8);
		emit_sd(tmp_reg, dst_base, dst_offset + i * 8);
	}
}

void RISCVCodeGen::emit_scalar_variant_move(uint8_t dst_base, int32_t dst_offset,
	uint8_t src_base, int32_t src_offset, uint8_t tmp_reg)
{
	for (int i = 0; i < 2; i++) {
		emit_ld(tmp_reg, src_base, src_offset + i * 8);
		emit_sd(tmp_reg, dst_base, dst_offset + i * 8);
	}
}

void RISCVCodeGen::emit_variant_eval_unary(int result_offset, int operand_offset, int op) {
	emit_variant_eval_unary(result_offset, REG_SP, operand_offset, op);
}

void RISCVCodeGen::emit_variant_eval_unary(int result_offset, uint8_t operand_base, int operand_offset,
	int op)
{
	// A1 before NIL setup: operand_base may alias REG_T0.
	emit_add_offset(REG_A1, operand_base, operand_offset);

	// Godot indexes operator_evaluator_table[op][a_type][b_type]; unary ops
	// require b_type = NIL, not the operand's type
	const int nil_offset = get_scratch_variant_offset(1);
	emit_li(REG_T0, Variant::NIL);
	emit_store_variant_type(REG_T0, REG_SP, nil_offset);

	emit_li(REG_A0, op);
	emit_add_offset(REG_A2, REG_SP, nil_offset);
	emit_add_offset(REG_A3, REG_SP, result_offset);
	emit_li(REG_A7, ECALL_VEVAL);
	emit_ecall();
}

void RISCVCodeGen::emit_variant_eval(int result_offset, int lhs_offset, int rhs_offset, int op,
	bool handle_clobbering)
{
	if (handle_clobbering) {
		spill_around_syscall({REG_A0, REG_A1, REG_A2, REG_A3});
	}
	emit_li(REG_A0, op);
	emit_add_offset(REG_A1, REG_SP, lhs_offset);
	emit_add_offset(REG_A2, REG_SP, rhs_offset);
	emit_add_offset(REG_A3, REG_SP, result_offset);
	emit_li(REG_A7, ECALL_VEVAL);
	emit_ecall();
}

bool RISCVCodeGen::has_int_fast_path(IROpcode op) {
	switch (op) {
		// DIV/MOD excluded: native trap vs Godot error on zero divisor
		case IROpcode::ADD:
		case IROpcode::SUB:
		case IROpcode::MUL:
		case IROpcode::BIT_AND:
		case IROpcode::BIT_OR:
		case IROpcode::BIT_XOR:
		case IROpcode::SHL:
		case IROpcode::SHR:
			return true;
		default:
			return false;
	}
}

void RISCVCodeGen::emit_branch_unless_both_int(int lhs_offset, int rhs_offset,
	const std::string& slow_label, bool require_non_negative)
{
	emit_lwu(REG_T0, REG_SP, lhs_offset + VARIANT_TYPE_OFFSET);
	emit_lwu(REG_T1, REG_SP, rhs_offset + VARIANT_TYPE_OFFSET);
	emit_xori(REG_T0, REG_T0, Variant::INT);
	emit_xori(REG_T1, REG_T1, Variant::INT);
	emit_or(REG_T0, REG_T0, REG_T1);
	mark_label_use(slow_label, m_code.size());
	emit_bne(REG_T0, REG_ZERO, 0);

	if (require_non_negative) {
		// Negative shift: only the host can raise Godot's error
		emit_load_variant_int(REG_T0, REG_SP, lhs_offset);
		emit_load_variant_int(REG_T1, REG_SP, rhs_offset);
		emit_or(REG_T0, REG_T0, REG_T1);
		mark_label_use(slow_label, m_code.size());
		emit_blt(REG_T0, REG_ZERO, 0);
	}
}

bool RISCVCodeGen::has_float_fast_path(IROpcode op) {
	switch (op) {
		// DIV: int/int zero-division is an error; the guard rejects that pair.
		case IROpcode::ADD:
		case IROpcode::SUB:
		case IROpcode::MUL:
		case IROpcode::DIV:
			return true;
		default:
			return false;
	}
}

void RISCVCodeGen::emit_branch_unless_float_pair(int lhs_offset, int rhs_offset,
	const std::string& slow_label)
{
	emit_lwu(REG_T0, REG_SP, lhs_offset + VARIANT_TYPE_OFFSET);
	emit_lwu(REG_T1, REG_SP, rhs_offset + VARIANT_TYPE_OFFSET);

	emit_addi(REG_T2, REG_T0, -Variant::INT);
	emit_i_type(0x13, REG_T2, 0x3, REG_T2, 2); // sltiu t2, t2, 2
	mark_label_use(slow_label, m_code.size());
	emit_beq(REG_T2, REG_ZERO, 0);

	emit_addi(REG_T2, REG_T1, -Variant::INT);
	emit_i_type(0x13, REG_T2, 0x3, REG_T2, 2); // sltiu t2, t2, 2
	mark_label_use(slow_label, m_code.size());
	emit_beq(REG_T2, REG_ZERO, 0);

	// Reject int/int: the int path already declined, or it's int/int division.
	emit_or(REG_T2, REG_T0, REG_T1);
	emit_andi(REG_T2, REG_T2, 1);
	mark_label_use(slow_label, m_code.size());
	emit_beq(REG_T2, REG_ZERO, 0);
}

void RISCVCodeGen::emit_numeric_to_double(uint8_t fd, int variant_offset) {
	const std::string is_float = gen_local_label(".num_float");
	const std::string done = gen_local_label(".num_done");

	emit_lwu(REG_T0, REG_SP, variant_offset + VARIANT_TYPE_OFFSET);
	emit_addi(REG_T0, REG_T0, -Variant::FLOAT);
	mark_label_use(is_float, m_code.size());
	emit_beq(REG_T0, REG_ZERO, 0);

	emit_load_variant_int(REG_T0, REG_SP, variant_offset);
	emit_fcvt_d_l(fd, REG_T0);
	mark_label_use(done, m_code.size());
	emit_jal(REG_ZERO, 0);

	define_label(is_float);
	emit_fld(fd, REG_SP, variant_offset + VARIANT_DATA_OFFSET);
	define_label(done);
}

void RISCVCodeGen::emit_float_pair_binary_op(int result_offset, int lhs_offset, int rhs_offset,
	IROpcode op)
{
	emit_numeric_to_double(REG_FA0, lhs_offset);
	emit_numeric_to_double(REG_FA1, rhs_offset);

	switch (op) {
		case IROpcode::ADD: emit_fadd_d(REG_FA2, REG_FA0, REG_FA1); break;
		case IROpcode::SUB: emit_fsub_d(REG_FA2, REG_FA0, REG_FA1); break;
		case IROpcode::MUL: emit_fmul_d(REG_FA2, REG_FA0, REG_FA1); break;
		case IROpcode::DIV: emit_fdiv_d(REG_FA2, REG_FA0, REG_FA1); break;
		default:
			throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Unsupported float pair binary op");
	}

	emit_li(REG_T0, Variant::FLOAT);
	emit_store_variant_type(REG_T0, REG_SP, result_offset);
	emit_fsd(REG_FA2, REG_SP, result_offset + VARIANT_DATA_OFFSET);
}

void RISCVCodeGen::emit_double_compare(uint8_t rd, IROpcode cmp_op) {
	switch (cmp_op) {
		case IROpcode::CMP_EQ:
		case IROpcode::BRANCH_EQ:
			emit_feq_d(rd, REG_FA0, REG_FA1);
			break;
		case IROpcode::CMP_NEQ:
		case IROpcode::BRANCH_NEQ:
			// feq(NaN,NaN)==0, so inverting feq is the correct !=.
			emit_feq_d(rd, REG_FA0, REG_FA1);
			emit_xori(rd, rd, 1);
			break;
		case IROpcode::CMP_LT:
		case IROpcode::BRANCH_LT:
			emit_flt_d(rd, REG_FA0, REG_FA1);
			break;
		case IROpcode::CMP_LTE:
		case IROpcode::BRANCH_LTE:
			// fle.d, not swapped flt.d: unordered pairs must be false.
			emit_r_type(0x53, rd, 0x0, REG_FA0, REG_FA1, 0x51); // fle.d
			break;
		case IROpcode::CMP_GT:
		case IROpcode::BRANCH_GT:
			emit_flt_d(rd, REG_FA1, REG_FA0);
			break;
		case IROpcode::CMP_GTE:
		case IROpcode::BRANCH_GTE:
			emit_r_type(0x53, rd, 0x0, REG_FA1, REG_FA0, 0x51); // fle.d
			break;
		default:
			throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Unknown double comparison");
	}
}

void RISCVCodeGen::emit_float_pair_comparison(int result_offset, int lhs_offset, int rhs_offset,
	IROpcode cmp_op)
{
	emit_numeric_to_double(REG_FA0, lhs_offset);
	emit_numeric_to_double(REG_FA1, rhs_offset);
	emit_double_compare(REG_T2, cmp_op);

	emit_li(REG_T0, Variant::BOOL);
	emit_store_variant_type(REG_T0, REG_SP, result_offset);
	emit_store_variant_bool(REG_T2, REG_SP, result_offset);
}

void RISCVCodeGen::emit_float_pair_fused_branch(IROpcode op, int lhs_offset, int rhs_offset,
	const std::string& label)
{
	emit_numeric_to_double(REG_FA0, lhs_offset);
	emit_numeric_to_double(REG_FA1, rhs_offset);
	emit_double_compare(REG_T2, op);

	mark_label_use(label, m_code.size());
	emit_bne(REG_T2, REG_ZERO, 0);
}

void RISCVCodeGen::emit_typed_float_comparison(int result_vreg, int result_offset,
	int lhs_vreg, int lhs_offset, int rhs_vreg, int rhs_offset, IROpcode cmp_op)
{
	emit_float_operand(REG_FA0, lhs_vreg, lhs_offset);
	emit_float_operand(REG_FA1, rhs_vreg, rhs_offset);
	emit_double_compare(REG_T2, cmp_op);
	emit_known_variant_type(result_vreg, result_offset, Variant::BOOL);
	emit_store_variant_bool(REG_T2, REG_SP, result_offset);
}

void RISCVCodeGen::emit_typed_float_fused_branch(IROpcode op, int lhs_vreg, int lhs_offset,
	int rhs_vreg, int rhs_offset, const std::string& label)
{
	emit_float_operand(REG_FA0, lhs_vreg, lhs_offset);
	emit_float_operand(REG_FA1, rhs_vreg, rhs_offset);
	emit_double_compare(REG_T2, op);
	mark_label_use(label, m_code.size());
	emit_bne(REG_T2, REG_ZERO, 0);
}

void RISCVCodeGen::emit_int_fused_branch(IROpcode op, int lhs_vreg, int lhs_offset,
	int rhs_vreg, int rhs_offset, const std::string& label)
{
	const uint8_t lhs = emit_int_operand(REG_T0, lhs_vreg, lhs_offset);
	const uint8_t rhs = emit_int_operand(REG_T1, rhs_vreg, rhs_offset);

	mark_label_use(label, m_code.size());
	switch (op) {
		case IROpcode::BRANCH_EQ:
			emit_beq(lhs, rhs, 0);
			break;
		case IROpcode::BRANCH_NEQ:
			emit_bne(lhs, rhs, 0);
			break;
		case IROpcode::BRANCH_LT:
			emit_blt(lhs, rhs, 0);
			break;
		case IROpcode::BRANCH_LTE:
			emit_bge(rhs, lhs, 0);
			break;
		case IROpcode::BRANCH_GT:
			emit_blt(rhs, lhs, 0);
			break;
		case IROpcode::BRANCH_GTE:
			emit_bge(lhs, rhs, 0);
			break;
		default:
			throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Unknown fused branch opcode");
	}
}

void RISCVCodeGen::emit_int_compare_imm(uint8_t result, uint8_t value, int64_t imm,
	IROpcode op, bool constant_on_left)
{
	if (constant_on_left) {
		switch (op) {
			case IROpcode::CMP_LT: op = IROpcode::CMP_GT; break;
			case IROpcode::CMP_LTE: op = IROpcode::CMP_GTE; break;
			case IROpcode::CMP_GT: op = IROpcode::CMP_LT; break;
			case IROpcode::CMP_GTE: op = IROpcode::CMP_LTE; break;
			case IROpcode::BRANCH_LT: op = IROpcode::BRANCH_GT; break;
			case IROpcode::BRANCH_LTE: op = IROpcode::BRANCH_GTE; break;
			case IROpcode::BRANCH_GT: op = IROpcode::BRANCH_LT; break;
			case IROpcode::BRANCH_GTE: op = IROpcode::BRANCH_LTE; break;
			default: break;
		}
	}
	switch (op) {
		case IROpcode::CMP_EQ: case IROpcode::BRANCH_EQ:
			emit_addi(result, value, int32_t(-imm));
			emit_seqz(result, result);
			break;
		case IROpcode::CMP_NEQ: case IROpcode::BRANCH_NEQ:
			emit_addi(result, value, int32_t(-imm));
			emit_snez(result, result);
			break;
		case IROpcode::CMP_LT: case IROpcode::BRANCH_LT:
			emit_slti(result, value, int32_t(imm));
			break;
		case IROpcode::CMP_GTE: case IROpcode::BRANCH_GTE:
			emit_slti(result, value, int32_t(imm));
			emit_xori(result, result, 1);
			break;
		case IROpcode::CMP_LTE: case IROpcode::BRANCH_LTE:
			emit_slti(result, value, int32_t(imm + 1));
			break;
		case IROpcode::CMP_GT: case IROpcode::BRANCH_GT:
			emit_slti(result, value, int32_t(imm + 1));
			emit_xori(result, result, 1);
			break;
		default:
			throw CompilerException(ErrorType::RISCV_codegen_ERROR,
				"Unknown integer comparison with immediate");
	}
}

void RISCVCodeGen::emit_int_fused_branch_imm(IROpcode op, int value_vreg, int value_offset,
	int64_t imm, bool constant_on_left, const std::string& label)
{
	const uint8_t value = emit_int_operand(REG_T0, value_vreg, value_offset);
	// Zero equality maps directly to the ISA's register/zero branch form.
	if (imm == 0 && (op == IROpcode::BRANCH_EQ || op == IROpcode::BRANCH_NEQ)) {
		mark_label_use(label, m_code.size());
		if (op == IROpcode::BRANCH_EQ) emit_beq(value, REG_ZERO, 0);
		else emit_bne(value, REG_ZERO, 0);
		return;
	}
	emit_int_compare_imm(REG_T2, value, imm, op, constant_on_left);
	mark_label_use(label, m_code.size());
	emit_bne(REG_T2, REG_ZERO, 0);
}

void RISCVCodeGen::emit_array_element_access(bool is_set, int array_offset, int index_offset, int value_offset,
	bool index_nonnegative, bool negative_constant_get, int array_vreg, int index_vreg) {
	// ECALL_ARRAY_AT: negative a1 = set (-index-1), unless a2's alignment bit
	// marks a negative constant get; non-negative a1 is always a get.
	emit_container_handle(REG_A0, array_vreg, array_offset);
	{
		auto [base, off] = variant_source(index_vreg, index_offset, REG_T0);
		emit_load_variant_int(REG_A1, base, off);
	}

	if (!index_nonnegative && !negative_constant_get) {
		// Negative index: wrap from end via ECALL_ARRAY_SIZE
		const std::string in_range = gen_local_label(".array_index");
		mark_label_use(in_range, m_code.size());
		emit_bge(REG_A1, REG_ZERO, 0);
		emit_li(REG_A7, ECALL_ARRAY_SIZE);
		emit_ecall();
		emit_add(REG_A1, REG_A1, REG_A0);
		emit_container_handle(REG_A0, array_vreg, array_offset);
		mark_label_use(in_range, m_code.size());
		emit_bge(REG_A1, REG_ZERO, 0);
		// Still negative after wrap: force out-of-range for the host to report
		emit_li(REG_A1, 0x7fffffff);
		define_label(in_range);
	}

	if (is_set) {
		emit_xori(REG_A1, REG_A1, -1); // -index - 1
	}
	emit_add_offset(REG_A2, REG_SP, value_offset);
	if (negative_constant_get) {
		// GuestVariant slots are aligned, so bit zero can select a signed-index read
		// without changing the syscall number or the legacy write convention.
		emit_ori(REG_A2, REG_A2, 1);
	}
	emit_li(REG_A7, ECALL_ARRAY_AT);
	emit_ecall();
}

// Frame slot or global data area, depending on whether the copy was elided.
std::pair<uint8_t, int> RISCVCodeGen::variant_source(int vreg, int offset, uint8_t scratch) {
	auto it = m_fn.global_handles.find(vreg);
	if (it == m_fn.global_handles.end()) {
		return { REG_SP, offset };
	}
	emit_address_of_global(scratch, it->second);
	return { scratch, 0 };
}

void RISCVCodeGen::emit_container_handle(uint8_t rd, int vreg, int offset) {
	auto [base, off] = variant_source(vreg, offset, rd);
	emit_lw(rd, base, off + VARIANT_DATA_OFFSET);
}

void RISCVCodeGen::clear_block_value_state() {
	std::fill(m_fn.known_tags.begin(), m_fn.known_tags.end(), IRInstruction::TypeHint_NONE);
	std::fill(m_fn.int_cache_slots.begin(), m_fn.int_cache_slots.end(), int8_t(-1));
	std::fill(m_fn.float_cache_slots.begin(), m_fn.float_cache_slots.end(), int8_t(-1));
	m_fn.int_cache_owners = {{ -1, -1, -1 }};
	m_fn.float_cache_owners = {{ -1, -1, -1 }};
	m_fn.next_int_cache = 0;
	m_fn.next_float_cache = 0;
	m_fn.chained_vreg = -1;
	m_fn.next_chained_vreg = -1;
}

void RISCVCodeGen::invalidate_cached_vreg(int vreg) {
	if (vreg < 0 || size_t(vreg) >= m_fn.known_tags.size()) {
		return;
	}
	const int int_slot = m_fn.int_cache_slots[size_t(vreg)];
	if (int_slot >= 0) {
		m_fn.int_cache_owners[size_t(int_slot)] = -1;
		m_fn.int_cache_slots[size_t(vreg)] = -1;
	}
	const int float_slot = m_fn.float_cache_slots[size_t(vreg)];
	if (float_slot >= 0) {
		m_fn.float_cache_owners[size_t(float_slot)] = -1;
		m_fn.float_cache_slots[size_t(vreg)] = -1;
	}
}

void RISCVCodeGen::note_known_tag(int vreg, int tag) {
	if (vreg >= 0 && size_t(vreg) < m_fn.known_tags.size()) {
		m_fn.known_tags[size_t(vreg)] = tag;
	}
}

void RISCVCodeGen::emit_known_variant_type(int vreg, int offset, int tag) {
	if (vreg < 0 || size_t(vreg) >= m_fn.known_tags.size() ||
		m_fn.known_tags[size_t(vreg)] != tag)
	{
		emit_li(REG_T0, tag);
		emit_store_variant_type(REG_T0, REG_SP, offset);
	}
	note_known_tag(vreg, tag);
}

void RISCVCodeGen::cache_int_result(int vreg, uint8_t source) {
	if (vreg < 0 || size_t(vreg) >= m_fn.int_cache_slots.size()) {
		return;
	}
	static constexpr std::array<uint8_t, 3> REGS {{ REG_T3, REG_T4, REG_T5 }};
	int slot = m_fn.int_cache_slots[size_t(vreg)];
	if (slot < 0) {
		slot = m_fn.next_int_cache++ % REGS.size();
		const int old = m_fn.int_cache_owners[size_t(slot)];
		if (old >= 0) {
			m_fn.int_cache_slots[size_t(old)] = -1;
		}
		m_fn.int_cache_owners[size_t(slot)] = vreg;
		m_fn.int_cache_slots[size_t(vreg)] = int8_t(slot);
	}
	emit_mv(REGS[size_t(slot)], source);
}

void RISCVCodeGen::cache_float_result(int vreg, uint8_t source) {
	if (vreg < 0 || size_t(vreg) >= m_fn.float_cache_slots.size()) {
		return;
	}
	static constexpr std::array<uint8_t, 3> REGS {{ 3, 4, 5 }};
	int slot = m_fn.float_cache_slots[size_t(vreg)];
	if (slot < 0) {
		slot = m_fn.next_float_cache++ % REGS.size();
		const int old = m_fn.float_cache_owners[size_t(slot)];
		if (old >= 0) {
			m_fn.float_cache_slots[size_t(old)] = -1;
		}
		m_fn.float_cache_owners[size_t(slot)] = vreg;
		m_fn.float_cache_slots[size_t(vreg)] = int8_t(slot);
	}
	emit_fmv_d(REGS[size_t(slot)], source);
}

// Int payload into rd, or a cached caller-saved register inside this block.
uint8_t RISCVCodeGen::emit_int_operand(uint8_t rd, int vreg, int offset) {
	static constexpr std::array<uint8_t, 3> REGS {{ REG_T3, REG_T4, REG_T5 }};
	if (vreg >= 0 && size_t(vreg) < m_fn.int_cache_slots.size()) {
		const int slot = m_fn.int_cache_slots[size_t(vreg)];
		if (slot >= 0) {
			return REGS[size_t(slot)];
		}
	}
	if (vreg >= 0 && vreg == m_fn.chained_vreg) {
		return REG_T2;
	}
	auto [base, off] = variant_source(vreg, offset, rd);
	emit_load_variant_int(rd, base, off);
	return rd;
}

uint8_t RISCVCodeGen::emit_float_operand(uint8_t fd, int vreg, int offset) {
	static constexpr std::array<uint8_t, 3> REGS {{ 3, 4, 5 }};
	if (vreg >= 0 && size_t(vreg) < m_fn.float_cache_slots.size()) {
		const int slot = m_fn.float_cache_slots[size_t(vreg)];
		if (slot >= 0) {
			emit_fmv_d(fd, REGS[size_t(slot)]);
			return fd;
		}
	}
	emit_fld(fd, REG_SP, offset + VARIANT_DATA_OFFSET);
	return fd;
}

void RISCVCodeGen::emit_typed_int_result(int result_vreg, int result_offset) {
	if (m_fn.forward_return) {
		auto [base, offset] = value_destination(result_vreg);
		emit_li(REG_T0, Variant::INT);
		emit_store_variant_type(REG_T0, base, offset);
		emit_store_variant_int(REG_T2, base, offset);
		return;
	}
	emit_known_variant_type(result_vreg, result_offset, Variant::INT);
	emit_store_variant_int(REG_T2, REG_SP, result_offset);
	cache_int_result(result_vreg, REG_T2);
}

// I-type form: the constant is the instruction's immediate, no Variant built.
void RISCVCodeGen::emit_typed_int_binary_op_imm(int result_vreg, int result_offset, int lhs_offset, int64_t imm, IROpcode op,
	int lhs_vreg) {
	const uint8_t lhs = emit_int_operand(REG_T0, lhs_vreg, lhs_offset);

	switch (op) {
		case IROpcode::ADD:
			emit_addi(REG_T2, lhs, static_cast<int32_t>(imm));
			break;
		case IROpcode::SUB:
			emit_addi(REG_T2, lhs, static_cast<int32_t>(-imm));
			break;
		case IROpcode::MUL: {
			uint8_t shift = 0;
			for (uint64_t value = uint64_t(imm); value > 1; value >>= 1) shift++;
			emit_slli(REG_T2, lhs, shift);
			break;
		}
		case IROpcode::BIT_AND:
			emit_andi(REG_T2, lhs, static_cast<int32_t>(imm));
			break;
		case IROpcode::BIT_OR:
			emit_ori(REG_T2, lhs, static_cast<int32_t>(imm));
			break;
		case IROpcode::BIT_XOR:
			emit_xori(REG_T2, lhs, static_cast<int32_t>(imm));
			break;
		case IROpcode::SHL:
			emit_slli(REG_T2, lhs, static_cast<uint8_t>(imm));
			break;
		case IROpcode::SHR:
			emit_srai(REG_T2, lhs, static_cast<uint8_t>(imm));
			break;
		default:
			throw CompilerException(ErrorType::RISCV_codegen_ERROR,
				"Unsupported typed int binary op with an immediate operand");
	}

	emit_typed_int_result(result_vreg, result_offset);
}

void RISCVCodeGen::emit_typed_int_binary_op(int result_vreg, int result_offset, int lhs_offset, int rhs_offset, IROpcode op,
	int lhs_vreg, int rhs_vreg) {
	const uint8_t lhs = emit_int_operand(REG_T0, lhs_vreg, lhs_offset);
	const uint8_t rhs = emit_int_operand(REG_T1, rhs_vreg, rhs_offset);

	switch (op) {
		case IROpcode::ADD:
			emit_add(REG_T2, lhs, rhs);
			break;
		case IROpcode::SUB:
			emit_sub(REG_T2, lhs, rhs);
			break;
		case IROpcode::MUL:
			emit_mul(REG_T2, lhs, rhs);
			break;
		case IROpcode::DIV:
			emit_div(REG_T2, lhs, rhs);
			break;
		case IROpcode::MOD:
			emit_rem(REG_T2, lhs, rhs);
			break;
		case IROpcode::BIT_AND:
			emit_and(REG_T2, lhs, rhs);
			break;
		case IROpcode::BIT_OR:
			emit_or(REG_T2, lhs, rhs);
			break;
		case IROpcode::BIT_XOR:
			emit_xor(REG_T2, lhs, rhs);
			break;
		case IROpcode::SHL:
			emit_sll(REG_T2, lhs, rhs);
			break;
		case IROpcode::SHR:
			emit_sra(REG_T2, lhs, rhs);
			break;
		default:
			throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Unsupported typed int binary op");
	}

	emit_typed_int_result(result_vreg, result_offset);
}

void RISCVCodeGen::emit_typed_int_comparison(int result_vreg, int result_offset,
	int lhs_vreg, int lhs_offset, int rhs_vreg, int rhs_offset, IROpcode cmp_op) {
	// Optimized path for type-hinted integer comparisons
	// Common in loops: for i: int in range(N)
	//
	// Process:
	// 1. Load int64 values from Variants
	// 2. Perform RISC-V comparison
	// 3. Store result as BOOL Variant (0 or 1)

	// Load lhs and rhs int64 values
	const uint8_t lhs = emit_int_operand(REG_T0, lhs_vreg, lhs_offset);
	const uint8_t rhs = emit_int_operand(REG_T1, rhs_vreg, rhs_offset);

	// Perform comparison and set REG_T2 to 0 or 1
	switch (cmp_op) {
		case IROpcode::CMP_EQ:
			// xor t2, t0, t1; seqz t2, t2  (set if equal to zero)
			emit_xor(REG_T2, lhs, rhs);
			emit_seqz(REG_T2, REG_T2);
			break;

		case IROpcode::CMP_NEQ:
			// xor t2, t0, t1; snez t2, t2  (set if not equal to zero)
			emit_xor(REG_T2, lhs, rhs);
			emit_snez(REG_T2, REG_T2);
			break;

		case IROpcode::CMP_LT:
			// slt t2, t0, t1  (set if t0 < t1, signed)
			emit_slt(REG_T2, lhs, rhs);
			break;

		case IROpcode::CMP_LTE:
			// t0 <= t1  is equivalent to  !(t1 < t0)
			// slt t2, t1, t0; xori t2, t2, 1
			emit_slt(REG_T2, rhs, lhs);
			emit_xori(REG_T2, REG_T2, 1);
			break;

		case IROpcode::CMP_GT:
			// t0 > t1  is equivalent to  t1 < t0
			emit_slt(REG_T2, rhs, lhs);
			break;

		case IROpcode::CMP_GTE:
			// t0 >= t1  is equivalent to  !(t0 < t1)
			// slt t2, t0, t1; xori t2, t2, 1
			emit_slt(REG_T2, lhs, rhs);
			emit_xori(REG_T2, REG_T2, 1);
			break;

		default:
			throw CompilerException(ErrorType::RISCV_codegen_ERROR, "Unsupported typed int comparison");
	}

	// Store result as BOOL Variant
	emit_known_variant_type(result_vreg, result_offset, Variant::BOOL);
	emit_store_variant_bool(REG_T2, REG_SP, result_offset);
}

void RISCVCodeGen::emit_typed_int_comparison_imm(int result_vreg, int result_offset,
	int value_vreg, int value_offset, int64_t imm, bool constant_on_left, IROpcode cmp_op)
{
	const uint8_t value = emit_int_operand(REG_T0, value_vreg, value_offset);
	emit_int_compare_imm(REG_T2, value, imm, cmp_op, constant_on_left);
	emit_known_variant_type(result_vreg, result_offset, Variant::BOOL);
	emit_store_variant_bool(REG_T2, REG_SP, result_offset);
}

void RISCVCodeGen::emit_typed_float_binary_op(int result_vreg, int result_offset,
	int lhs_vreg, int lhs_offset, int rhs_vreg, int rhs_offset, IROpcode op) {
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
	emit_float_operand(REG_FA0, lhs_vreg, lhs_offset);

	// Load rhs double value: fld fa1, rhs_offset+8(sp)
	emit_float_operand(REG_FA1, rhs_vreg, rhs_offset);

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

	// Store result as FLOAT Variant, directly through a0 for an arithmetic leaf.
	if (m_fn.forward_return) {
		auto [base, offset] = value_destination(result_vreg);
		emit_li(REG_T0, Variant::FLOAT);
		emit_store_variant_type(REG_T0, base, offset);
		emit_fsd(REG_FA2, base, offset + VARIANT_DATA_OFFSET);
		return;
	}
	emit_known_variant_type(result_vreg, result_offset, Variant::FLOAT);
	emit_fsd(REG_FA2, REG_SP, result_offset + VARIANT_DATA_OFFSET);
	cache_float_result(result_vreg, REG_FA2);
}

void RISCVCodeGen::emit_typed_vector_binary_op(int result_vreg, int result_offset,
	int lhs_offset, int rhs_offset, IROpcode op, IRInstruction::TypeHint type_hint) {
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

			emit_sw(REG_T2, REG_SP, result_offset + component_offset);
		} else {
			emit_flr(REG_FA0, REG_SP, lhs_offset + component_offset);
			emit_flr(REG_FA1, REG_SP, rhs_offset + component_offset);

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

			emit_fsr(REG_FA2, REG_SP, result_offset + component_offset);
		}
	}

	emit_known_variant_type(result_vreg, result_offset, type_hint);
}

void RISCVCodeGen::emit_typed_vector_scalar_op(int result_vreg, int result_offset,
	int vector_offset, int scalar_offset, bool scalar_on_left, IROpcode op,
	IRInstruction::TypeHint vector_type, IRInstruction::TypeHint scalar_type)
{
	int elem_count = 0;
	bool is_int = false;
	switch (vector_type) {
		case Variant::VECTOR2: elem_count = 2; break;
		case Variant::VECTOR3: elem_count = 3; break;
		case Variant::VECTOR4:
		case Variant::COLOR: elem_count = 4; break;
		case Variant::VECTOR2I: elem_count = 2; is_int = true; break;
		case Variant::VECTOR3I: elem_count = 3; is_int = true; break;
		case Variant::VECTOR4I: elem_count = 4; is_int = true; break;
		default:
			throw CompilerException(ErrorType::RISCV_codegen_ERROR,
				"Invalid vector type hint for scalar broadcast");
	}

	if (is_int) {
		if (scalar_type != Variant::INT) {
			throw CompilerException(ErrorType::RISCV_codegen_ERROR,
				"Integer vector broadcast requires an INT scalar");
		}
		emit_load_variant_int(REG_T1, REG_SP, scalar_offset);
		for (int i = 0; i < elem_count; i++) {
			const int component_offset = int_offset(i);
			emit_lw(REG_T0, REG_SP, vector_offset + component_offset);
			switch (op) {
				case IROpcode::MUL: emit_mul(REG_T2, REG_T0, REG_T1); break;
				case IROpcode::DIV:
					if (scalar_on_left) emit_div(REG_T2, REG_T1, REG_T0);
					else emit_div(REG_T2, REG_T0, REG_T1);
					break;
				default:
					throw CompilerException(ErrorType::RISCV_codegen_ERROR,
						"Unsupported integer vector/scalar operation");
			}
			emit_sw(REG_T2, REG_SP, result_offset + component_offset);
		}
	} else {
		if (scalar_type == Variant::FLOAT) {
			emit_fld(REG_FA1, REG_SP, scalar_offset + VARIANT_DATA_OFFSET);
		} else if (scalar_type == Variant::INT) {
			emit_load_variant_int(REG_T0, REG_SP, scalar_offset);
			emit_fcvt_d_l(REG_FA1, REG_T0);
		} else {
			throw CompilerException(ErrorType::RISCV_codegen_ERROR,
				"Float vector broadcast requires a numeric scalar");
		}
		emit_fcvt_r_d(REG_FA1, REG_FA1);
		for (int i = 0; i < elem_count; i++) {
			const int component_offset = real_offset(i);
			emit_flr(REG_FA0, REG_SP, vector_offset + component_offset);
			switch (op) {
				case IROpcode::MUL: emit_fmul_r(REG_FA2, REG_FA0, REG_FA1); break;
				case IROpcode::DIV:
					if (scalar_on_left) emit_fdiv_r(REG_FA2, REG_FA1, REG_FA0);
					else emit_fdiv_r(REG_FA2, REG_FA0, REG_FA1);
					break;
				default:
					throw CompilerException(ErrorType::RISCV_codegen_ERROR,
						"Unsupported float vector/scalar operation");
			}
			emit_fsr(REG_FA2, REG_SP, result_offset + component_offset);
		}
	}
	emit_known_variant_type(result_vreg, result_offset, vector_type);
}

void RISCVCodeGen::emit_load_variant_type(uint8_t rd, uint8_t base_reg, int32_t variant_offset) {
	emit_lw(rd, base_reg, variant_offset + VARIANT_TYPE_OFFSET);
}

void RISCVCodeGen::emit_store_variant_type(uint8_t rs, uint8_t base_reg, int32_t variant_offset) {
	emit_sw(rs, base_reg, variant_offset + VARIANT_TYPE_OFFSET);
}

void RISCVCodeGen::emit_variant_truthy(uint8_t rd, int variant_offset, int32_t type_hint, uint8_t base_reg) {
	// lbu of the low byte is only correct for BOOL; INT 256 and scoped index 0x..00 would mis-booleanize.
	switch (type_hint) {
		case Variant::NIL:
			emit_li(rd, 0);
			return;
		case Variant::BOOL:
			emit_lbu(rd, base_reg, variant_offset + VARIANT_DATA_OFFSET);
			return;
		case Variant::INT:
			emit_ld(rd, base_reg, variant_offset + VARIANT_DATA_OFFSET);
			emit_snez(rd, rd);
			return;
		case Variant::FLOAT:
			// slli 1 clears the sign bit: -0.0 becomes 0, NaN stays non-zero.
			emit_ld(rd, base_reg, variant_offset + VARIANT_DATA_OFFSET);
			emit_i_type(0x13, rd, 1, rd, 1); // slli rd, rd, 1
			emit_snez(rd, rd);
			return;
		default:
			break;
	}

	// Unknown type: host decides via OP_NOT (!booleanize), then invert.
	// Unconditional: the allocator's spill moves must not sit on a conditional path.
	const int scratch_offset = get_scratch_variant_offset();
	emit_variant_eval_unary(scratch_offset, base_reg, variant_offset, 23); // OP_NOT
	emit_lbu(rd, REG_SP, scratch_offset + VARIANT_DATA_OFFSET);
	emit_seqz(rd, rd);
}

void RISCVCodeGen::emit_load_variant_bool(uint8_t rd, uint8_t base_reg, int32_t variant_offset) {
	emit_lbu(rd, base_reg, variant_offset + VARIANT_DATA_OFFSET);
}

void RISCVCodeGen::emit_store_variant_bool(uint8_t rs, uint8_t base_reg, int32_t variant_offset) {
	emit_sb(rs, base_reg, variant_offset + VARIANT_DATA_OFFSET);
}

void RISCVCodeGen::emit_load_variant_int(uint8_t rd, uint8_t base_reg, int32_t variant_offset) {
	emit_ld(rd, base_reg, variant_offset + VARIANT_DATA_OFFSET);
}

void RISCVCodeGen::emit_store_variant_int(uint8_t rs, uint8_t base_reg, int32_t variant_offset) {
	emit_sd(rs, base_reg, variant_offset + VARIANT_DATA_OFFSET);
}

// Single point for the 12-bit immediate range check on every load/store.
void RISCVCodeGen::emit_load_with_offset(uint8_t opcode, uint8_t funct3, uint8_t rd, uint8_t rs1, int32_t offset) {
	if (fits_in_signed(offset, I_TYPE_IMM_BITS)) {
		emit_i_type(opcode, rd, funct3, rs1, offset);
		return;
	}
	// Wide path: address in REG_WIDE_SCRATCH to avoid clobbering live temporaries.
	emit_add_offset(REG_WIDE_SCRATCH, rs1, offset);
	emit_i_type(opcode, rd, funct3, REG_WIDE_SCRATCH, 0);
}

void RISCVCodeGen::emit_store_with_offset(uint8_t opcode, uint8_t funct3, uint8_t rs2, uint8_t rs1, int32_t offset) {
	if (fits_in_signed(offset, S_TYPE_IMM_BITS)) {
		emit_s_type(opcode, funct3, rs1, rs2, offset);
		return;
	}
	// REG_WIDE_SCRATCH, not a temporary: using rs2 would overwrite the value being stored.
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
	// funct3=6; was 4 (LBU), which only worked by accident on LE.
	emit_load_with_offset(0x03, 6, rd, rs1, offset);
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

void RISCVCodeGen::emit_fld(uint8_t rd, uint8_t rs1, int32_t offset) {
	emit_load_with_offset(0x07, 3, rd, rs1, offset);
}

void RISCVCodeGen::emit_fsd(uint8_t rs2, uint8_t rs1, int32_t offset) {
	emit_store_with_offset(0x27, 3, rs2, rs1, offset);
}

void RISCVCodeGen::emit_flw(uint8_t rd, uint8_t rs1, int32_t offset) {
	emit_load_with_offset(0x07, 2, rd, rs1, offset);
}

void RISCVCodeGen::emit_fsw(uint8_t rs2, uint8_t rs1, int32_t offset) {
	emit_store_with_offset(0x27, 2, rs2, rs1, offset);
}

void RISCVCodeGen::emit_fcvt_d_s(uint8_t rd, uint8_t rs1) {
	emit_r4_type(0b1010011, rd, 0, rs1, 0, 0b01000, 1);
}

void RISCVCodeGen::emit_fcvt_s_d(uint8_t rd, uint8_t rs1) {
	emit_r4_type(0b1010011, rd, 0, rs1, 1, 0b01000, 0);
}

void RISCVCodeGen::emit_fcvt_d_l(uint8_t rd, uint8_t rs1) {
	emit_r4_type(0b1010011, rd, 0, rs1, 2, 0b11010, 1);
}

void RISCVCodeGen::emit_fadd_d(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x01);
}

void RISCVCodeGen::emit_fsub_d(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x05);
}

void RISCVCodeGen::emit_fmul_d(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x09);
}

void RISCVCodeGen::emit_fdiv_d(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x0D);
}

void RISCVCodeGen::emit_fmv_d(uint8_t rd, uint8_t rs) {
	emit_r4_type(0x53, rd, 0x0, rs, rs, 0b00100, 1);
}

void RISCVCodeGen::emit_fsqrt_d(uint8_t rd, uint8_t rs1) {
	emit_r_type(0x53, rd, 0, rs1, 0, 0b0101101);
}

void RISCVCodeGen::emit_fabs_d(uint8_t rd, uint8_t rs1) {
	// FSGNJX.D rd, rs, rs: sign XOR sign = positive, exact for all inputs including NaN.
	emit_r_type(0x53, rd, 0b010, rs1, rs1, 0b0010001);
}

void RISCVCodeGen::emit_flt_d(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	// Result into integer register; quiet, false when either operand is NaN.
	emit_r_type(0x53, rd, 0b001, rs1, rs2, 0b1010001);
}

void RISCVCodeGen::emit_feq_d(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	// Result into integer register; quiet, false when either operand is NaN.
	emit_r_type(0x53, rd, 0b010, rs1, rs2, 0b1010001);
}

void RISCVCodeGen::emit_fcvt_l_d(uint8_t rd, uint8_t rs1) {
	// rm=001 (toward zero): the (int64_t) cast.
	emit_r_type(0x53, rd, 0b001, rs1, 0b00010, 0b1100001);
}

void RISCVCodeGen::emit_fadd_s(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x00);
}

void RISCVCodeGen::emit_fsub_s(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x04);
}

void RISCVCodeGen::emit_fmul_s(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x08);
}

void RISCVCodeGen::emit_fdiv_s(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x0C);
}

void RISCVCodeGen::emit_fmv_s(uint8_t rd, uint8_t rs) {
	emit_r4_type(0x53, rd, 0x0, rs, rs, 0b00100, 0);
}

// real_t-width FP: single set of call sites, dispatched to flw/fld or fadd.s/fadd.d by m_layout.

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
	// Widen real_t to double (Variant::FLOAT is always 64-bit).
	if (m_layout.double_precision) {
		if (rd != rs1) {
			emit_fmv_d(rd, rs1);
		}
	} else {
		emit_fcvt_d_s(rd, rs1);
	}
}

void RISCVCodeGen::emit_fcvt_r_d(uint8_t rd, uint8_t rs1) {
	// Narrow double to real_t; no-op when real_t is double.
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

void RISCVCodeGen::emit_sh2add(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	// Zba: libriscv decodes unconditionally; jump table shift-and-add in one instruction.
	emit_r_type(0x33, rd, 4, rs1, rs2, 0b0010000);
}

void RISCVCodeGen::emit_srai(uint8_t rd, uint8_t rs, uint8_t shamt) {
	emit_i_type(0x13, rd, 5, rs, (0b010000 << 6) | (shamt & 0x3F));
}

void RISCVCodeGen::emit_slli(uint8_t rd, uint8_t rs, uint8_t shamt) {
	emit_i_type(0x13, rd, 1, rs, shamt);
}

void RISCVCodeGen::emit_sext_w(uint8_t rd, uint8_t rs) {
	emit_i_type(0x1b, rd, 0, rs, 0);
}

void RISCVCodeGen::spill_all_registers() {
	// Frame slots are authoritative; drop all vreg→preg mappings.
	for (int vreg : m_allocator.mapped_vregs()) {
		m_allocator.spill_register(vreg);
	}
}

// Refines opcode_clobbers_abi_registers for typed arithmetic that stays in-frame.
bool RISCVCodeGen::instruction_may_ecall(const IRInstruction& instr) const {
	if (!opcode_clobbers_abi_registers(instr.opcode)) {
		return instr.opcode == IROpcode::BREAKPOINT; // ecalls despite !clobbers_abi
	}

	const bool both_reg = instr.operands.size() >= 3 &&
		instr.operands[1].type == IRValue::Type::REGISTER &&
		instr.operands[2].type == IRValue::Type::REGISTER;
	const auto hint = instr.type_hint;

	switch (instr.opcode) {
		// Native for INT, FLOAT, vectors. POW/IN always host.
		case IROpcode::ADD:
		case IROpcode::SUB:
		case IROpcode::MUL:
		case IROpcode::DIV:
		case IROpcode::MOD:
		case IROpcode::BIT_AND:
		case IROpcode::BIT_OR:
		case IROpcode::BIT_XOR:
		case IROpcode::SHL:
		case IROpcode::SHR:
			return !(both_reg && (hint == Variant::INT || hint == Variant::FLOAT ||
				TypeHintUtils::is_vector(hint)));

		// Native for typed numeric pairs.
		case IROpcode::CMP_EQ:
		case IROpcode::CMP_NEQ:
		case IROpcode::CMP_LT:
		case IROpcode::CMP_LTE:
		case IROpcode::CMP_GT:
		case IROpcode::CMP_GTE:
			return !(both_reg && (hint == Variant::INT || hint == Variant::FLOAT));

		case IROpcode::BRANCH_EQ:
		case IROpcode::BRANCH_NEQ:
		case IROpcode::BRANCH_LT:
		case IROpcode::BRANCH_LTE:
		case IROpcode::BRANCH_GT:
		case IROpcode::BRANCH_GTE:
			return hint != Variant::INT && hint != Variant::FLOAT;

		// Inline for NIL, BOOL, INT, FLOAT.
		case IROpcode::BRANCH_ZERO:
		case IROpcode::BRANCH_NOT_ZERO:
			return !(hint == Variant::NIL || hint == Variant::BOOL ||
				hint == Variant::INT || hint == Variant::FLOAT);

		case IROpcode::BIT_NOT:
			return hint != Variant::INT;

		case IROpcode::CONVERT:
		case IROpcode::COERCE:
			return false;

		case IROpcode::GLOBAL_CALL: {
			const GlobalFn fn = static_cast<GlobalFn>(instr.operands[1].immediate());
			return global_call_may_ecall(fn);
		}

		default:
			return true;
	}
}

// NUMERIC dispatches to int_form/float_form; inline only if both are.
bool RISCVCodeGen::global_call_may_ecall(GlobalFn fn) {
	const GlobalFunction& info = global_function(fn);
	auto is_inline = [](GlobalKind kind) {
		return kind == GlobalKind::INT_OP || kind == GlobalKind::FLOAT_OP;
	};
	if (info.kind == GlobalKind::NUMERIC) {
		return !(is_inline(global_function(info.int_form).kind) &&
			is_inline(global_function(info.float_form).kind));
	}
	return !is_inline(info.kind);
}

// A syscall whose whole answer arrives in a0 leaves nothing behind on the host:
// no scoped variant is created. Everything else -- a Variant result, a VCALL, a
// guest call -- is assumed to allocate.
static bool syscall_answers_in_register(const IRInstruction& instr) {
	if (instr.opcode != IROpcode::CALL_SYSCALL || instr.operands.size() < 2 ||
		instr.operands[1].type != IRValue::Type::IMMEDIATE) {
		return false;
	}
	switch (instr.operands[1].immediate()) {
		case ECALL_ARRAY_SIZE:
		case ECALL_STRING_SIZE:
		// Attaches a script to an object the caller already holds; nothing new
		// is handed back, so no scoped variant is made.
		case ECALL_CLASS_BIND:
			return true;
		case ECALL_DICTIONARY_OPS: {
			if (instr.operands.size() < 3 || instr.operands[2].type != IRValue::Type::IMMEDIATE) {
				return false;
			}
			const int64_t op = instr.operands[2].immediate();
			return op == dictionary_op(Dictionary_Op::GET_SIZE) ||
				op == dictionary_op(Dictionary_Op::HAS);
		}
		default:
			return false;
	}
}

static bool leaves_nothing_scoped(const IRInstruction& instr) {
	switch (instr.opcode) {
		case IROpcode::ARRAY_SET:
		case IROpcode::ARRAY_APPEND:
		case IROpcode::DICT_SET:
			return true;
		default:
			return syscall_answers_in_register(instr);
	}
}

bool RISCVCodeGen::instruction_may_allocate_scoped(const IRInstruction& instr) const {
	if (instr.opcode == IROpcode::CALL && !instr.operands.empty() &&
		m_scoped_clean_functions.count(text(instr.operands[0])) != 0)
	{
		return false;
	}
	return instruction_may_ecall(instr) && !leaves_nothing_scoped(instr);
}

void RISCVCodeGen::plan_program_call_properties(const IRProgram& program) {
	m_scoped_clean_functions.clear();
	m_trusted_internal_entries.clear();

	for (const IRFunction& func : program.functions) {
		for (const IRInstruction& instr : func.instructions) {
			if (instr.opcode == IROpcode::CALL && instr.trusted_internal_call &&
				!instr.operands.empty())
			{
				m_trusted_internal_entries.insert(text(instr.operands[0]));
			}
		}
	}

	// Start with functions that either cannot create a scoped value at all or
	// explicitly own a mark/release pair. Then propagate through calls. Starting
	// from the empty set keeps recursive cycles conservative.
	bool changed = true;
	while (changed) {
		changed = false;
		for (const IRFunction& func : program.functions) {
			if (m_scoped_clean_functions.count(func.name) != 0) {
				continue;
			}
			if (func.return_type_hint != Variant::NIL &&
				func.return_type_hint != Variant::BOOL &&
				func.return_type_hint != Variant::INT &&
				func.return_type_hint != Variant::FLOAT)
			{
				continue;
			}

			bool owns_scope = false;
			bool unresolved_call = false;
			bool host_may_allocate = false;
			for (const IRInstruction& instr : func.instructions) {
				owns_scope = owns_scope || instr.opcode == IROpcode::SCOPE_MARK;
				if (instr.opcode == IROpcode::CALL) {
					if (instr.operands.empty() ||
						m_scoped_clean_functions.count(text(instr.operands[0])) == 0)
					{
						unresolved_call = true;
					}
				} else if (instruction_may_ecall(instr) && !leaves_nothing_scoped(instr)) {
					host_may_allocate = true;
				}
			}
			if (!unresolved_call && (owns_scope || !host_may_allocate)) {
				m_scoped_clean_functions.insert(func.name);
				changed = true;
			}
		}
	}
}

bool RISCVCodeGen::scope_body_may_allocate(const IRFunction& func, size_t mark_index) const {
	const size_t count = func.instructions.size();
	const int scope_id = int(func.instructions[mark_index].operands.at(0).immediate());

	size_t last = mark_index;
	bool bounded = false;

	for (size_t i = mark_index + 1; i < count; i++) {
		const IRInstruction& instr = func.instructions[i];
		if (instr.opcode == IROpcode::SCOPE_RELEASE &&
			int(instr.operands.at(0).immediate()) == scope_id)
		{
			last = std::max(last, i);
			bounded = true;
		}
	}

	const size_t label_index = mark_index + 1;
	if (label_index < count && func.instructions[label_index].opcode == IROpcode::LABEL) {
		const uint32_t label = func.instructions[label_index].operands.at(0).string_id;
		for (size_t i = label_index + 1; i < count; i++) {
			const IRInstruction& instr = func.instructions[i];
			const IROperandSignature& sig = ir_opcode_info(instr.opcode).signature;
			for (size_t op = 0; op < instr.operands.size(); op++) {
				if (sig.kind_at(op) != IROperandKind::LBL && sig.kind_at(op) != IROperandKind::LBL_LIST) {
					continue;
				}
				if (instr.operands[op].type == IRValue::Type::LABEL &&
					instr.operands[op].string_id == label)
				{
					last = std::max(last, i);
					bounded = true;
				}
			}
		}
	}

	if (!bounded) {
		return false;
	}

	for (size_t i = mark_index + 1; i <= last; i++) {
		const IROpcode op = func.instructions[i].opcode;
		if (op == IROpcode::SCOPE_MARK || op == IROpcode::SCOPE_RELEASE) {
			continue;
		}
		if (instruction_may_allocate_scoped(func.instructions[i])) {
			return true;
		}
	}
	return false;
}

// Registers nothing will read again at each SCOPE_RELEASE.
//
// The host's release walk reads the whole frame looking for handles to rescue,
// and a slot the program is done with still holds the one it last had. In a
// container walk that is the loop variable: every pass rescues the previous
// element -- a Variant move, a re-registration and a write back through guest
// memory -- for a value no instruction will ever look at. Storing NIL over the
// dead slots first is one instruction each, and guest instructions are not what
// these loops cost.
void RISCVCodeGen::plan_release_clears(const IRFunction& func) {
	m_fn.release_clears.clear();


	const size_t count = func.instructions.size();
	const size_t nregs = size_t(std::max(func.max_registers, 0));
	if (nregs == 0) {
		return;
	}
	bool has_release = false;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::SCOPE_RELEASE) {
			has_release = true;
			break;
		}
	}
	if (!has_release) {
		return;
	}

	// Only slots that can hold a handle are worth an instruction. A register
	// every write to which leaves a number, a bool or nothing behind carries
	// none, whatever else the loop does with it.
	std::vector<bool> carries_handle(nregs, false);
	for (const auto& instr : func.instructions) {
		const int dst = ir_destination_register(instr);
		if (dst < 0 || size_t(dst) >= nregs) {
			continue;
		}
		switch (instr.opcode) {
			case IROpcode::LOAD_IMM:
			case IROpcode::LOAD_FLOAT_IMM:
			case IROpcode::LOAD_BOOL:
			case IROpcode::LOAD_NIL:
			case IROpcode::TYPE_TEST:
			case IROpcode::TYPE_TEST_MASK:
			case IROpcode::TYPE_OF:
				continue;
			default:
				break;
		}
		// A comparison answers a bool; its type hint describes its operands.
		if (ir_has_effect(instr.opcode, IR_COMPARISON)) {
			continue;
		}
		if (instr.type_hint == Variant::INT || instr.type_hint == Variant::FLOAT ||
			instr.type_hint == Variant::BOOL) {
			continue;
		}
		if (syscall_answers_in_register(instr)) {
			continue;
		}
		carries_handle[size_t(dst)] = true;
	}

	const std::vector<std::vector<size_t>> successors = build_successors(func);

	// def/use once, before the fixpoint: a full write kills, INOUT reads first.
	std::vector<int> defs(count, -1);
	std::vector<RegisterSet> uses(count);
	std::vector<int> reads;
	for (size_t k = 0; k < count; k++) {
		const IRInstruction& instr = func.instructions[k];
		uses[k].resize(nregs);
		if (instr.opcode == IROpcode::AWAIT) {
			continue;
		}
		const int dst_index = ir_destination_operand_index(instr.opcode);
		if (dst_index >= 0 && size_t(dst_index) < instr.operands.size() &&
			ir_opcode_info(instr.opcode).signature.kind_at(size_t(dst_index)) == IROperandKind::DST) {
			const int dst = ir_destination_register(instr);
			if (dst >= 0 && size_t(dst) < nregs) {
				defs[k] = dst;
			}
		}
		reads.clear();
		ir_collect_read_registers(instr, reads);
		for (int reg : reads) {
			if (reg >= 0 && size_t(reg) < nregs) {
				uses[k].set(size_t(reg));
			}
		}
	}

	// Backward liveness to a fixed point. A slot counts as live wherever the
	// analysis cannot see: an unresolved label operand leaves the successor
	// list short, which is the one direction that would be unsafe, so
	// AWAIT and CALL_GUEST -- which resume through the host -- keep everything.
	std::vector<RegisterSet> live_in(count, RegisterSet(nregs));
	RegisterSet live(nregs);
	bool changed = true;
	while (changed) {
		changed = false;
		for (size_t k = count; k-- > 0;) {
			live.clear();
			for (size_t s : successors[k]) {
				live |= live_in[s];
			}
			if (func.instructions[k].opcode == IROpcode::AWAIT) {
				live.set_all();
			} else {
				if (defs[k] >= 0) {
					live.reset(size_t(defs[k]));
				}
				live |= uses[k];
			}
			if (live != live_in[k]) {
				live_in[k] = live;
				changed = true;
			}
		}
	}

	RegisterSet live_out(nregs);
	for (size_t k = 0; k < count; k++) {
		if (func.instructions[k].opcode != IROpcode::SCOPE_RELEASE) {
			continue;
		}
		live_out.clear();
		for (size_t s : successors[k]) {
			live_out |= live_in[s];
		}
		std::vector<int> dead;
		for (size_t r = 0; r < nregs; r++) {
			if (!live_out.test(r) && carries_handle[r] && m_fn.global_handles.count(int(r)) == 0) {
				dead.push_back(int(r));
			}
		}
		if (!dead.empty()) {
			m_fn.release_clears.emplace(int(k), std::move(dead));
		}
	}
}

void RISCVCodeGen::plan_scopes(const IRFunction& func) {
	m_fn.elided_scopes.clear();
	m_fn.scope_slots.clear();
	m_fn.scope_slot_count = 0;

	int max_scope_id = -1;
	for (const IRInstruction& instr : func.instructions) {
		if (instr.opcode == IROpcode::SCOPE_MARK || instr.opcode == IROpcode::SCOPE_RELEASE) {
			max_scope_id = std::max(max_scope_id,
				int(instr.operands[0].immediate()));
		}
	}
	if (max_scope_id < 0) {
		return;
	}

	// Default kept: a release whose mark was removed must read a zeroed slot.
	m_fn.elided_scopes.assign(size_t(max_scope_id) + 1, false);
	for (size_t i = 0; i < func.instructions.size(); i++) {
		if (func.instructions[i].opcode != IROpcode::SCOPE_MARK) {
			continue;
		}
		const int scope_id = int(func.instructions[i].operands[0].immediate());
		m_fn.elided_scopes[size_t(scope_id)] = !scope_body_may_allocate(func, i);
	}

	m_fn.scope_slots.assign(size_t(max_scope_id) + 1, -1);
	for (int scope_id = 0; scope_id <= max_scope_id; scope_id++) {
		if (!m_fn.elided_scopes[size_t(scope_id)]) {
			m_fn.scope_slots[size_t(scope_id)] = m_fn.scope_slot_count++;
		}
	}
}

bool RISCVCodeGen::scope_is_elided(int scope_id) const {
	return scope_id >= 0 && size_t(scope_id) < m_fn.elided_scopes.size() &&
		m_fn.elided_scopes[size_t(scope_id)];
}

int RISCVCodeGen::scope_slot_offset(int scope_id) const {
	if (scope_id < 0 || size_t(scope_id) >= m_fn.scope_slots.size() ||
		m_fn.scope_slots[size_t(scope_id)] < 0)
	{
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			"Scope id " + std::to_string(scope_id) + " was not planned into the frame");
	}
	return m_fn.scope_slot_base + m_fn.scope_slots[size_t(scope_id)] * 8;
}

void RISCVCodeGen::gen_scope_mark(const IRInstruction& instr) {
	const int scope_id = int(instr.operands[0].immediate());
	if (scope_is_elided(scope_id)) {
		return;
	}
	const int offset = scope_slot_offset(scope_id);

	spill_around_syscall({ REG_A0, REG_A7 });
	emit_li(REG_A0, int64_t(Scope_Op::MARK));
	emit_li(REG_A7, ECALL_VSCOPE);
	emit_ecall();
	emit_sd(REG_A0, REG_SP, offset);
}

void RISCVCodeGen::gen_scope_release(const IRInstruction& instr) {
	const int scope_id = int(instr.operands[0].immediate());
	if (scope_is_elided(scope_id)) {
		return;
	}
	const int offset = scope_slot_offset(scope_id);

	// Host reads the frame as Variants; registers must be spilled first.
	spill_all_registers();
	spill_around_syscall({ REG_A0, REG_A1, REG_A2, REG_A3, REG_A4, REG_A5, REG_A6, REG_A7 });

	// After the spill, so a dead register's stale value cannot land back on top.
	// current_instr_idx is bumped before the instruction is generated.
	auto clears = m_fn.release_clears.find(m_fn.current_instr_idx - 1);
	if (clears != m_fn.release_clears.end()) {
		for (int vreg : clears->second) {
			auto slot = m_fn.variant_offsets.find(vreg);
			if (slot != m_fn.variant_offsets.end()) {
				emit_sw(REG_ZERO, REG_SP, slot->second + VARIANT_TYPE_OFFSET);
			}
		}
	}

	emit_ld(REG_A1, REG_SP, offset);
	emit_add_offset(REG_A2, REG_SP, SAVED_REG_SPACE);
	emit_li(REG_A3, m_fn.variant_space);
	if (m_data_global_count > 0) {
		emit_address_of_global_area(REG_A4);
		emit_li(REG_A5, int64_t(m_data_global_count) * variant_size());
	} else {
		emit_li(REG_A4, 0);
		emit_li(REG_A5, 0);
	}
	emit_li(REG_A6, int64_t(m_instance_count) * variant_size());
	emit_li(REG_A0, int64_t(Scope_Op::RELEASE));
	emit_li(REG_A7, ECALL_VSCOPE);
	emit_ecall();
}

void RISCVCodeGen::spill_around_syscall(const std::vector<uint8_t>& clobbered_regs) {
	for (const auto& move : m_allocator.handle_syscall_clobbering(clobbered_regs, m_fn.current_instr_idx)) {
		emit_mv(move.second, move.first);
	}
}

void RISCVCodeGen::emit_syscall_result(int result_vreg, uint8_t result_reg, int result_offset, int variant_type) {
	emit_li(REG_T0, variant_type);
	emit_sw(REG_T0, REG_SP, result_offset);
	emit_sd(result_reg, REG_SP, result_offset + 8);
}

void RISCVCodeGen::emit_stack_adjust(int32_t amount) {
	if (amount == 0) {
		return;
	}
	emit_add_offset(REG_SP, REG_SP, amount);
}

void RISCVCodeGen::emit_load_stack_offset(uint8_t rd, int32_t offset) {
	emit_add_offset(rd, REG_SP, offset);
}

bool RISCVCodeGen::is_complex_variant_type(int variant_type) {
	// Inline types (no scoped-variant storage) vs complex types (String, Array, Dictionary, ...).
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
		// Object payload is a handle (tagged ObjectID), not a scoped-variant index.
		case Variant::OBJECT:
			return false;
		default:
			return true;
	}
}

} // namespace gdscript
