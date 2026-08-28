#include "compiler.h"
#include "debug_layout.h"
#include "variant_types.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace gdscript;

int main() {
	CompilerOptions options;
	options.debug_info = true;
	Compiler compiler;
	const auto elf = compiler.compile(
			"static var process_total: int = 2\n"
			"var member_value: int = 3\n"
			"func calculate(amount: int):\n"
			"\tvar doubled: int = amount * 2\n"
			"\treturn doubled + member_value\n", options);
	assert(!elf.empty());
	const auto &records = compiler.get_debug_variables();
	const auto named = [&](const char *name, DebugStorage storage) {
		return std::find_if(records.begin(), records.end(), [&](const DebugVariableRecord &record) {
			return record.name == name && record.storage == storage;
		});
	};
	assert(named("amount", DebugStorage::FRAME) != records.end());
	assert(named("doubled", DebugStorage::FRAME) != records.end());
	assert(named("member_value", DebugStorage::MEMBER) != records.end());
	assert(named("process_total", DebugStorage::GLOBAL) != records.end());
	for (const auto &record : records) {
		assert(record.offset >= 0);
		assert(record.pc_begin <= record.pc_end);
	}

	// A typed parameter is coerced into a fresh register, and the debugger reads
	// it from there: the record follows the coerced slot, declared type attached.
	assert(named("amount", DebugStorage::FRAME)->type == int32_t(Variant::INT));

	// Optimizer passes insert and delete under the recorded live ranges, so the
	// ranges are remapped rather than clamped: every local keeps a live span.
	{
		const char *source =
				"func work(count: int) -> int:\n"
				"\tvar folded: int = 1 + 2\n"
				"\tvar dead: int = 3 * 4\n"
				"\tvar total: int = 0\n"
				"\tfor i in range(count):\n"
				"\t\ttotal += i\n"
				"\tvar tail: int = total * 2 + folded\n"
				"\treturn tail\n";
		CompilerOptions optimized;
		optimized.debug_info = true;
		Compiler with_passes;
		assert(!with_passes.compile(source, optimized).empty());
		size_t spans = 0;
		for (const auto &record : with_passes.get_debug_variables()) {
			if (record.storage != DebugStorage::FRAME) continue;
			assert(record.pc_begin < record.pc_end); // clamped ranges collapse here
			spans++;
		}
		assert(spans >= 3);
	}

	const std::vector<uint8_t> blob = encode_debug_variables(records);
	std::vector<DebugVariableRecord> decoded;
	assert(decode_debug_variables(blob.data(), blob.size(), decoded));
	assert(decoded.size() == records.size());
	for (size_t cut = 0; cut < std::min<size_t>(blob.size(), 32); cut++) {
		std::vector<DebugVariableRecord> rejected{{}};
		assert(!decode_debug_variables(blob.data(), cut, rejected));
		assert(rejected.empty());
	}
	std::vector<uint8_t> trailing = blob;
	trailing.push_back(0);
	assert(!decode_debug_variables(trailing.data(), trailing.size(), decoded));
	assert(decoded.empty());

	std::cout << "All debug layout tests passed\n";
	return 0;
}
