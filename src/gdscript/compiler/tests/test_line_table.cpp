// Address-to-line table: ordering, function-boundary breaks, and blob decoding.
#include "../codegen.h"
#include "../compiler.h"
#include "../elf_builder.h"
#include "../ir_optimizer.h"
#include "../lexer.h"
#include "../line_table.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
	if (!condition) {
		std::cerr << "FAILED: " << what << std::endl;
		failures++;
	}
}

template <typename T>
void check_eq(T actual, T expected, const std::string& what) {
	if (actual != expected) {
		std::cerr << "FAILED: " << what << ": expected " << expected
			<< ", got " << actual << std::endl;
		failures++;
	}
}

// Text offsets (not rebased vaddrs); failures point at the backend, not ElfBuilder.
struct Generated {
	LineTable table;
	std::unordered_map<std::string, size_t> functions;
	bool ok = false;
};

Generated generate(const std::string& source, bool optimize = true) {
	Generated out;
	try {
		Lexer lexer(source);
		Parser parser(lexer.tokenize());
		parser.set_doc_comments(lexer.doc_comments());
		Program program = parser.parse();

		CodeGenerator codegen;
		IRProgram ir = codegen.generate(program);
		if (optimize) {
			IROptimizer optimizer;
			optimizer.optimize(ir);
		}

		RISCVCodeGen backend;
		backend.generate(ir);
		out.table = backend.get_line_table();
		out.functions = backend.get_function_offsets();
		out.ok = true;
	} catch (const std::exception& e) {
		std::cerr << "FAILED to compile: " << e.what() << std::endl;
		failures++;
	}
	return out;
}

// -= The table the backend produces =-

void test_ordering_and_shape() {
	const std::string source =
		"func first():\n" // 1
		"\tvar a = 1\n" // 2
		"\tvar b = 2\n" // 3
		"\treturn a + b\n" // 4
		"func second():\n" // 5
		"\treturn 7\n"; // 6

	for (bool optimize : { false, true }) {
		const Generated gen = generate(source, optimize);
		if (!gen.ok) {
			continue;
		}
		const std::string when = optimize ? " (optimized)" : " (unoptimized)";

		check(!gen.table.entries.empty(), "the table has rows" + when);
		check(gen.table.is_normalized(),
			"rows ascend by address and never repeat a line" + when);

		for (const LineTableEntry& entry : gen.table.entries) {
			check(entry.line <= 6, "no row names a line past the source" + when);
		}
	}
}

void test_function_entry_breaks_the_run() {
	// Forced entry row prevents the previous function's last line from
	// covering the next prologue. Empty prologues get replaced by the first
	// statement's row at the same address.
	const std::string source =
		"func first():\n" // 1
		"\tvar a = 1\n" // 2
		"\tvar b = 2\n" // 3
		"\treturn a + b\n" // 4
		"func second():\n" // 5
		"\treturn 7\n"; // 6

	const Generated gen = generate(source);
	if (!gen.ok) {
		return;
	}

	const auto first = gen.functions.find("first");
	const auto second = gen.functions.find("second");
	check(first != gen.functions.end() && second != gen.functions.end(),
		"both functions are in the offset map");
	if (first == gen.functions.end() || second == gen.functions.end()) {
		return;
	}

	check_eq(gen.table.line_for_address(uint32_t(first->second)), uint32_t(1),
		"a prologue that emits code reports the declaration line");

	const uint32_t entry_line = gen.table.line_for_address(uint32_t(second->second));
	check(entry_line >= 5, "second()'s entry never reports a line from first(),"
		" got " + std::to_string(entry_line));
	check(entry_line <= 6, "second()'s entry reports one of its own lines,"
		" got " + std::to_string(entry_line));
}

void test_statements_are_reachable() {
	// Every statement that survives the optimizer owns a row.
	const std::string source =
		"func test(n):\n" // 1
		"\tvar total = 0\n" // 2
		"\twhile n > 0:\n" // 3
		"\t\ttotal = total + n\n" // 4
		"\t\tn = n - 1\n" // 5
		"\treturn total\n"; // 6

	const Generated gen = generate(source);
	if (!gen.ok) {
		return;
	}

	for (uint32_t line : { 2u, 3u, 4u, 5u, 6u }) {
		check(gen.table.address_for_line(line) != 0,
			"line " + std::to_string(line) + " has an address");
	}
}

void test_branch_relaxation_keeps_addresses() {
	// relax_branches() inserts instructions; rows after the insertion must shift.
	std::string source =
		"func test(n):\n"
		"\tvar total = 0\n"
		"\twhile n > 0:\n";
	for (int i = 0; i < 700; i++) {
		source += "\t\ttotal = total + " + std::to_string(i) + "\n";
	}
	source += "\t\tn = n - 1\n";
	source += "\treturn total\n";

	const Generated gen = generate(source);
	if (!gen.ok) {
		return;
	}
	check(gen.table.is_normalized(), "the table is still ordered after relaxation");

	const auto test_fn = gen.functions.find("test");
	if (test_fn == gen.functions.end()) {
		check(false, "test() is in the offset map");
		return;
	}
	check_eq(gen.table.line_for_address(uint32_t(test_fn->second)), uint32_t(1),
		"test()'s entry still reports its declaration line after relaxation");
}

void test_addresses_are_rebased_by_the_elf() {
	const std::string source =
		"func test():\n"
		"\treturn 1\n";

	Compiler compiler;
	CompilerOptions options;
	const std::vector<uint8_t> elf = compiler.compile(source, options);
	check(!elf.empty(), "the program compiles");
	if (elf.empty()) {
		return;
	}

	const LineTable& table = compiler.get_line_table();
	check(!table.entries.empty(), "a plain compile still publishes a line table");
	for (const LineTableEntry& entry : table.entries) {
		check(entry.address >= 0x10000,
			"published addresses are the ELF's, not text offsets");
	}
}

// -= Lookup =-

void test_lookup() {
	LineTable table;
	table.entries = { { 100, 5 }, { 140, 6 }, { 200, 9 } };

	check_eq(table.line_for_address(99), uint32_t(0), "before the first row: no line");
	check_eq(table.line_for_address(100), uint32_t(5), "on a row: that row");
	check_eq(table.line_for_address(139), uint32_t(5), "between rows: the row below");
	check_eq(table.line_for_address(140), uint32_t(6), "on the next row");
	check_eq(table.line_for_address(1000), uint32_t(9), "past the last row: the last row");

	check_eq(table.address_for_line(6), uint32_t(140), "a line maps back to its address");
	check_eq(table.address_for_line(7), uint32_t(0), "a line with no code has no address");

	LineTable empty;
	check_eq(empty.line_for_address(0), uint32_t(0), "an empty table looks up as unknown");
	check_eq(empty.address_for_line(1), uint32_t(0), "an empty table has no addresses");

	// Lowest address wins when a line appears at multiple addresses.
	LineTable duplicated;
	duplicated.entries = { { 100, 5 }, { 140, 6 }, { 200, 5 } };
	check_eq(duplicated.address_for_line(5), uint32_t(100),
		"a line emitted twice reports its first address");
}

// -= The blob =-

void test_roundtrip() {
	LineTable table;
	table.entries = { { 0x10000, 1 }, { 0x10024, 2 }, { 0x100f0, 17 } };

	const std::vector<uint8_t> blob = encode_line_table(table);
	LineTable decoded;
	check(decode_line_table(blob.data(), blob.size(), decoded), "the blob decodes");
	check_eq(decoded.entries.size(), table.entries.size(), "row count survives");
	for (size_t i = 0; i < decoded.entries.size() && i < table.entries.size(); i++) {
		check_eq(decoded.entries[i].address, table.entries[i].address, "address survives");
		check_eq(decoded.entries[i].line, table.entries[i].line, "line survives");
	}

	LineTable empty_decoded;
	const std::vector<uint8_t> empty_blob = encode_line_table(LineTable{});
	check(decode_line_table(empty_blob.data(), empty_blob.size(), empty_decoded),
		"an empty table roundtrips");
	check(empty_decoded.entries.empty(), "and decodes to no rows");
}

void test_decode_rejects_bad_input() {
	LineTable table;
	table.entries = { { 0x10000, 1 }, { 0x10024, 2 } };
	const std::vector<uint8_t> blob = encode_line_table(table);

	LineTable out;
	check(!decode_line_table(nullptr, 0, out), "a null blob is refused");
	check(!decode_line_table(blob.data(), 4, out), "a blob shorter than the header is refused");
	check(!decode_line_table(blob.data(), blob.size() - 1, out),
		"a truncated row is refused");

	std::vector<uint8_t> bad_magic = blob;
	bad_magic[0] ^= 0xFF;
	check(!decode_line_table(bad_magic.data(), bad_magic.size(), out),
		"a blob with the wrong magic is refused");

	std::vector<uint8_t> bad_version = blob;
	bad_version[4] = 99;
	check(!decode_line_table(bad_version.data(), bad_version.size(), out),
		"a blob from another version is refused");

	// Row 1 rewritten to an address below row 0.
	std::vector<uint8_t> misordered = blob;
	misordered[20] = 0;
	misordered[21] = 0;
	misordered[22] = 0;
	misordered[23] = 0;
	check(!decode_line_table(misordered.data(), misordered.size(), out),
		"a misordered blob is refused");
	check(out.entries.empty(), "and leaves the output empty");
}

} // namespace

int main() {
	test_ordering_and_shape();
	test_function_entry_breaks_the_run();
	test_statements_are_reachable();
	test_branch_relaxation_keeps_addresses();
	test_addresses_are_rebased_by_the_elf();
	test_lookup();
	test_roundtrip();
	test_decode_rejects_bad_input();

	if (failures > 0) {
		std::cerr << failures << " line table test(s) failed" << std::endl;
		return 1;
	}
	std::cout << "All line table tests passed" << std::endl;
	return 0;
}
