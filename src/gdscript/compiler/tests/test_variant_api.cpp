//   ./test_variant_api             run the sweep
//   ./test_variant_api --list      print every row and its verdict
//   GDSC_WRITE_BASELINE=1 ./test_variant_api    rewrite the list
#include "../compiler.h"
#include "../compiler_exception.h"
#include "../codegen.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "../traits.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace gdscript;

namespace {

struct MethodRow {
	const char* type;
	const char* name;
	bool is_static;
	const char* result;
	int argc;
	const char* args[8];
};

struct CtorRow {
	const char* type;
	int argc;
	const char* args[8];
};

struct MemberRow { const char* type; const char* name; const char* member_type; };
struct ConstantRow { const char* type; const char* name; const char* constant_type; };
struct OperatorRow { const char* type; const char* op; const char* right; const char* result; };
struct UnaryRow { const char* type; const char* op; const char* result; };
struct TypeRow { const char* type; const char* index_type; };
struct SampleRow { const char* type; const char* expression; };

// Include once: the .def file provides fallback #defines that would stick.
struct Table {
	std::vector<TypeRow> types;
	std::vector<CtorRow> ctors;
	std::vector<MemberRow> members;
	std::vector<ConstantRow> constants;
	std::vector<OperatorRow> operators;
	std::vector<UnaryRow> unaries;
	std::vector<MethodRow> methods;
	std::vector<SampleRow> samples;
	std::vector<SampleRow> nonzero;
};

Table build_table() {
	Table table;
#define GDSC_VARIANT_TYPE(type, index_type) \
	table.types.push_back({ #type, #index_type });
#define GDSC_VARIANT_CTOR(type, argc, a0, a1, a2, a3, a4, a5, a6, a7) \
	table.ctors.push_back({ #type, argc, { #a0, #a1, #a2, #a3, #a4, #a5, #a6, #a7 } });
#define GDSC_VARIANT_MEMBER(type, member, member_type) \
	table.members.push_back({ #type, #member, #member_type });
#define GDSC_VARIANT_CONSTANT(type, constant, constant_type) \
	table.constants.push_back({ #type, #constant, #constant_type });
#define GDSC_VARIANT_OPERATOR(type, op, right_type, result_type) \
	table.operators.push_back({ #type, op, #right_type, #result_type });
#define GDSC_VARIANT_UNARY(type, op, result_type) \
	table.unaries.push_back({ #type, op, #result_type });
#define GDSC_VARIANT_METHOD(type, method, is_static, result_type, argc, a0, a1, a2, a3, a4, a5, a6, a7) \
	table.methods.push_back({ #type, #method, is_static != 0, #result_type, argc, \
		{ #a0, #a1, #a2, #a3, #a4, #a5, #a6, #a7 } });
#define GDSC_VARIANT_SAMPLE(type, expression) \
	table.samples.push_back({ #type, expression });
#define GDSC_VARIANT_NONZERO(type, expression) \
	table.nonzero.push_back({ #type, expression });
#include "variant_api.def"
	return table;
}

const Table& api() {
	static const Table table = build_table();
	return table;
}

std::string sample_of(const std::string& type) {
	for (const SampleRow& row : api().samples) {
		if (type == row.type) {
			return row.expression;
		}
	}
	return {};
}

std::string nonzero_of(const std::string& type) {
	for (const SampleRow& row : api().nonzero) {
		if (type == row.type) {
			return row.expression;
		}
	}
	return sample_of(type);
}

std::string argument_list(int argc, const char* const* args) {
	std::string out;
	for (int i = 0; i < argc; i++) {
		if (i > 0) {
			out += ", ";
		}
		out += sample_of(args[i]);
	}
	return out;
}

std::string unary_spelling(const std::string& op) {
	if (op == "unary-") return "-";
	if (op == "unary+") return "+";
	if (op == "not") return "not ";
	return op;
}

struct Row {
	std::string id;
	std::string source;
};

std::vector<Row> collect_rows() {
	std::vector<Row> rows;
	const auto add = [&rows](std::string id, std::string body) {
		rows.push_back({ std::move(id), "func test():\n" + body });
	};

	for (const TypeRow& row : api().types) {
		if (std::string(row.index_type) == "Nil") {
			continue;
		}
		add(std::string(row.type) + " index",
			"\tvar v = " + sample_of(row.type) + "\n\treturn v[0]\n");
	}
	for (const CtorRow& row : api().ctors) {
		std::string id = std::string(row.type) + " ctor(";
		for (int i = 0; i < row.argc; i++) {
			id += (i ? "," : "");
			id += row.args[i];
		}
		id += ")";
		add(id, "\treturn " + std::string(row.type) + "(" + argument_list(row.argc, row.args) + ")\n");
	}
	for (const MemberRow& row : api().members) {
		add(std::string(row.type) + " member " + row.name,
			"\tvar v = " + sample_of(row.type) + "\n\treturn v." + row.name + "\n");
		// Member write is a separate lowering from member read.
		add(std::string(row.type) + " member= " + row.name,
			"\tvar v = " + sample_of(row.type) + "\n"
			"\tv." + row.name + " = " + sample_of(row.member_type) + "\n"
			"\treturn v\n");
	}
	for (const ConstantRow& row : api().constants) {
		add(std::string(row.type) + " const " + row.name,
			"\treturn " + std::string(row.type) + "." + row.name + "\n");
	}
	for (const OperatorRow& row : api().operators) {
		const std::string op = row.op;
		const bool divides = op == "/" || op == "%";
		add(std::string(row.type) + " op " + op + " " + row.right,
			"\tvar a = " + sample_of(row.type) + "\n"
			"\tvar b = " + (divides ? nonzero_of(row.right) : sample_of(row.right)) + "\n"
			"\treturn a " + op + " b\n");
	}
	for (const UnaryRow& row : api().unaries) {
		add(std::string(row.type) + " unary " + row.op,
			"\tvar a = " + sample_of(row.type) + "\n"
			"\treturn " + unary_spelling(row.op) + "a\n");
	}
	for (const MethodRow& row : api().methods) {
		std::string id = std::string(row.type) + (row.is_static ? " static " : " method ") + row.name + "(";
		for (int i = 0; i < row.argc; i++) {
			id += (i ? "," : "");
			id += row.args[i];
		}
		id += ")";
		const std::string arguments = argument_list(row.argc, row.args);
		if (row.is_static) {
			add(id, "\treturn " + std::string(row.type) + "." + row.name + "(" + arguments + ")\n");
		} else {
			add(id, "\tvar v = " + sample_of(row.type) + "\n"
					"\treturn v." + row.name + "(" + arguments + ")\n");
		}
	}
	return rows;
}

std::string compile_failure(const std::string& source) {
	try {
		Lexer lexer(source);
		Parser parser(lexer.tokenize());
		Program program = parser.parse();
		apply_traits(program);
		CodeGenerator codegen;
		IRProgram ir = codegen.generate(program);
		ir_verify(ir, "codegen");
		IROptimizer optimizer;
		optimizer.optimize(ir);
		ir_verify(ir, "optimizer");

		Compiler compiler;
		if (compiler.compile(source).empty()) {
			const std::string message = compiler.get_error_info().message;
			return message.empty() ? "ELF generation produced nothing" : message;
		}
		return {};
	} catch (const CompilerException& e) {
		return e.what();
	} catch (const std::exception& e) {
		return std::string("exception: ") + e.what();
	}
}

std::string baseline_file() {
#ifdef GDSC_VARIANT_BASELINE
	return GDSC_VARIANT_BASELINE;
#else
	const char* relative = "tests/variant_api_unsupported.txt";
	for (const std::string& candidate : { std::string("../") + relative, std::string(relative) }) {
		std::ifstream file(candidate);
		if (file.good()) {
			return candidate;
		}
	}
	return relative;
#endif
}

std::set<std::string> read_baseline(const std::string& path) {
	std::set<std::string> ids;
	std::ifstream file(path);
	std::string line;
	while (std::getline(file, line)) {
		if (line.empty() || line[0] == '#') {
			continue;
		}
		ids.insert(line);
	}
	return ids;
}

void write_baseline(const std::string& path, const std::map<std::string, std::string>& failures) {
	std::ofstream file(path);
	file << "# Generated by test_variant_api. Rewrite: GDSC_WRITE_BASELINE=1 ./test_variant_api\n"
		 << "\n";
	for (const auto& [id, reason] : failures) {
		file << id << "\n";
	}
}

} // namespace

int main(int argc, char** argv) {
	const bool list_all = argc > 1 && std::strcmp(argv[1], "--list") == 0;
	const bool rewrite = std::getenv("GDSC_WRITE_BASELINE") != nullptr;

	const std::vector<Row> rows = collect_rows();
	std::map<std::string, std::string> failures;
	for (const Row& row : rows) {
		const std::string failure = compile_failure(row.source);
		if (!failure.empty()) {
			failures[row.id] = failure;
		}
		if (list_all) {
			std::cout << (failure.empty() ? "ok    " : "FAIL  ") << row.id;
			if (!failure.empty()) {
				std::cout << "\n          " << failure;
			}
			std::cout << "\n";
		}
	}

	const std::string path = baseline_file();
	if (rewrite) {
		write_baseline(path, failures);
		std::cout << "wrote " << path << " with " << failures.size() << " unsupported rows\n";
		return 0;
	}

	const std::set<std::string> expected = read_baseline(path);
	std::vector<std::string> regressions;
	std::vector<std::string> fixed;
	for (const auto& [id, reason] : failures) {
		if (expected.count(id) == 0) {
			regressions.push_back(id + "\n      " + reason);
		}
	}
	for (const std::string& id : expected) {
		if (failures.count(id) == 0) {
			fixed.push_back(id);
		}
	}

	std::cout << rows.size() << " rows, " << (rows.size() - failures.size()) << " compile, "
			  << failures.size() << " do not (" << expected.size() << " listed as known)\n";

	if (!regressions.empty()) {
		std::cout << "\nThese rows stopped compiling:\n";
		for (const std::string& id : regressions) {
			std::cout << "  " << id << "\n";
		}
	}
	if (!fixed.empty()) {
		std::cout << "\nThese rows compile now and should leave the list:\n";
		for (const std::string& id : fixed) {
			std::cout << "  " << id << "\n";
		}
		std::cout << "\n  GDSC_WRITE_BASELINE=1 " << argv[0] << "\n";
	}
	if (!regressions.empty() || !fixed.empty()) {
		return 1;
	}
	std::cout << "The supported surface is exactly what the list says.\n";
	return 0;
}
