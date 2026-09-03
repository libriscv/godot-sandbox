#pragma once
// A generator of small, type-correct GDScript programs, and a shrinker for the
// ones that fail.
//
// V1 (the IR verifier), V2 (optimization invariance) and V3 (the differential
// run against a real machine) are only as good as the programs they are pointed
// at. A generator turns all three into fuzzers: it emits programs nobody
// thought to write, and each check decides for itself whether the answer is
// right, so no expected output has to be written down.
//
// Everything here is deterministic: the same seed produces the same program,
// on any machine, forever. That is what makes a failure reproducible from the
// one number printed with it.
//
// The grammar starts where the bugs were -- integer and float arithmetic,
// comparisons, and/or, if/while, local shadowing -- rather than trying to cover
// the language.
#include <cstdint>
#include <string>
#include <vector>

namespace gdscript_test {

// The types a generated expression can have. Keeping them apart is what makes
// the output type-correct: GDScript will not compare a bool with an int the way
// C will, and a division by an integer zero is an error rather than a value.
enum class GenType {
	INT,
	FLOAT,
	BOOL,
};

// A tiny deterministic PRNG (SplitMix64). Not for anything but reproducibility.
class Random {
public:
	explicit Random(uint64_t seed) : m_state(seed + 0x9E3779B97F4A7C15ull) {}

	uint64_t next() {
		uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		return z ^ (z >> 31);
	}

	// A number in [0, bound).
	uint32_t below(uint32_t bound) {
		return static_cast<uint32_t>(next() % bound);
	}

	bool chance(uint32_t percent) {
		return below(100) < percent;
	}

private:
	uint64_t m_state;
};

// Knobs, so that a fuzz run can be made shallower or deeper without editing the
// generator.
struct GenOptions {
	int max_expression_depth = 3;
	int max_statements = 8;
	int max_nesting = 2;
	// Literal magnitudes stay small so that a generated program cannot reach
	// interesting arithmetic by accident and then spend the rest of the run
	// multiplying enormous numbers together. Overflow is well defined -- both
	// the interpreter and the machine wrap -- but a program whose answer is a
	// wrapped product tests less than one whose answer is a small number.
	int max_int_literal = 20;
	// Emit helper functions and calls to them.
	bool allow_functions = true;
	// Emit while loops.
	bool allow_loops = true;
	// Struct programs exercise shape tracking and the raw Dictionary opcodes.
	// Disable for differential runs whose reference interpreter has no host.
	bool allow_structs = true;
	// When a generated struct is present, sometimes provide its fields and
	// concrete method through a trait so splice output reaches the same fuzzers.
	bool allow_traits = true;
};

// One generated program, and the pieces a shrinker can take apart.
struct GeneratedProgram {
	uint64_t seed = 0;
	// The statements of test(), one per element, already indented. The shrinker
	// works by deleting these, so each one has to stand alone: a statement that
	// opens a block carries its whole block.
	std::vector<std::string> statements;
	// Helper functions, whole. The shrinker deletes these too, last, and only
	// when nothing calls them.
	std::vector<std::string> functions;
	std::vector<std::string> declarations;
	// The expression test() returns.
	std::string result_expression;

	std::string source() const {
		std::string out;
		for (const auto& declaration : declarations) {
			out += declaration;
			out += "\n";
		}
		for (const auto& function : functions) {
			out += function;
			out += "\n";
		}
		out += "func test():\n";
		for (const auto& statement : statements) {
			out += statement;
		}
		out += "\treturn " + result_expression + "\n";
		return out;
	}
};

class Generator {
public:
	Generator(uint64_t seed, GenOptions options = {})
		: m_random(seed), m_seed(seed), m_options(options) {}

	GeneratedProgram generate() {
		GeneratedProgram program;
		program.seed = m_seed;
		m_has_struct = m_options.allow_structs && m_random.chance(50);
		if (m_has_struct) {
			program.declarations.push_back(
				"struct FuzzPoint:\n"
				"\tvar x: int = 0\n"
				"\tvar y: int = 0\n"
				"\tfunc total() -> int:\n"
				"\t\treturn self.x + self.y\n"
				"\n"
				"func fuzz_point_round_trip(point: FuzzPoint) -> FuzzPoint:\n"
				"\treturn point\n");
		}
		m_has_trait = m_options.allow_traits && (m_seed % 4 == 0);
		if (m_has_trait) {
			program.declarations.insert(program.declarations.begin(),
				"uses FuzzAccumulator\n"
				"trait FuzzAccumulator:\n"
				"\tvar fuzz_trait_total: int = 0\n"
				"\tfunc fuzz_trait_add(left: int, right: int) -> int:\n"
				"\t\tfuzz_trait_total += left + right\n"
				"\t\treturn fuzz_trait_total\n");
		}

		if (m_options.allow_functions && m_random.chance(50)) {
			const int count = 1 + static_cast<int>(m_random.below(2));
			for (int i = 0; i < count; i++) {
				program.functions.push_back(generate_function(i));
			}
		}

		m_scopes.clear();
		push_scope();
		if (m_has_trait) {
			program.statements.push_back("\tvar fuzz_trait_probe: int = fuzz_trait_add(1, 2)\n");
		}

		const int count = 1 + static_cast<int>(m_random.below(m_options.max_statements));
		for (int i = 0; i < count; i++) {
			program.statements.push_back(generate_statement(1, 0));
		}

		// test() has to return something the checks can compare, so the result
		// is always a value rather than whatever the last statement left.
		program.result_expression = generate_expression(pick_type(), m_options.max_expression_depth);

		pop_scope();
		return program;
	}

private:
	struct Variable {
		std::string name;
		GenType type;
		// A loop counter may be read but not assigned to: the generated loops
		// terminate because the counter counts down, and a generated assignment
		// to it turns the program into one that never ends.
		bool assignable = true;
		// Declared as a union, so its Variant type is only known at run time.
		// Narrowing one into a typed slot needs a host THROW guard, which the
		// reference interpreter has no answer for.
		bool union_hint = false;
	};

	Random m_random;
	uint64_t m_seed;
	GenOptions m_options;
	std::vector<std::vector<Variable>> m_scopes;
	std::vector<std::string> m_function_names;
	int m_next_variable = 0;
	int m_next_loop = 0;
	int m_next_struct = 0;
	bool m_has_struct = false;
	bool m_has_trait = false;
	// Set while generating a value that has to land in a typed slot.
	bool m_proven_types_only = false;

	void push_scope() { m_scopes.emplace_back(); }
	void pop_scope() { m_scopes.pop_back(); }

	GenType pick_type() {
		switch (m_random.below(3)) {
			case 0: return GenType::INT;
			case 1: return GenType::FLOAT;
			default: return GenType::BOOL;
		}
	}

	// Variables of a type, innermost scope last, so that shadowing is generated
	// and then actually exercised: the name a later statement uses has to
	// resolve to the innermost declaration.
	std::vector<std::string> variables_of_type(GenType type, bool assignable_only = false) const {
		std::vector<std::string> found;
		for (const auto& scope : m_scopes) {
			for (const auto& variable : scope) {
				if (variable.type != type) {
					continue;
				}
				if (assignable_only && !variable.assignable) {
					continue;
				}
				if (m_proven_types_only && variable.union_hint) {
					continue;
				}
				found.push_back(variable.name);
			}
		}
		return found;
	}

	std::string indent(int depth) const {
		return std::string(static_cast<size_t>(depth), '\t');
	}

	// -= Expressions =-

	std::string int_literal() {
		return std::to_string(m_random.below(static_cast<uint32_t>(m_options.max_int_literal) + 1));
	}

	std::string float_literal() {
		// Values with an exact binary representation, so that a difference
		// between two runs is a real difference and not the last bit of a
		// decimal that neither side promised to round the same way.
		const uint32_t whole = m_random.below(static_cast<uint32_t>(m_options.max_int_literal) + 1);
		const uint32_t quarters = m_random.below(4);
		return std::to_string(whole) + "." + (quarters == 0 ? "0" : quarters == 1 ? "25" : quarters == 2 ? "5" : "75");
	}

	// A non-zero integer, for the right-hand side of / and %. GDScript reports
	// an error and yields zero for integer division by zero, which is a
	// behaviour worth its own test rather than something to stumble into here.
	std::string nonzero_int_literal() {
		return std::to_string(1 + m_random.below(static_cast<uint32_t>(m_options.max_int_literal)));
	}

	std::string generate_expression(GenType type, int depth) {
		if (depth <= 0) {
			return generate_atom(type);
		}

		switch (type) {
			case GenType::BOOL:
				return generate_bool_expression(depth);
			case GenType::INT:
			case GenType::FLOAT:
				return generate_numeric_expression(type, depth);
		}
		return generate_atom(type);
	}

	std::string generate_atom(GenType type) {
		const auto candidates = variables_of_type(type);
		if (!candidates.empty() && m_random.chance(60)) {
			return candidates[m_random.below(static_cast<uint32_t>(candidates.size()))];
		}
		switch (type) {
			case GenType::INT: return int_literal();
			case GenType::FLOAT: return float_literal();
			case GenType::BOOL: return m_random.chance(50) ? "true" : "false";
		}
		return "0";
	}

	std::string generate_numeric_expression(GenType type, int depth) {
		const uint32_t choice = m_random.below(10);

		// Every sub-expression is generated into a named local before it is
		// concatenated. Writing `a() + " + " + b()` would leave the order the
		// two draws happen in up to the compiler, and the same seed would then
		// produce different programs in different builds -- which is exactly
		// the reproducibility this generator exists to provide.

		// A call to a helper function, when there is one.
		if (choice == 0 && !m_function_names.empty() && type == GenType::INT) {
			const std::string name = m_function_names[m_random.below(static_cast<uint32_t>(m_function_names.size()))];
			const std::string argument = generate_expression(GenType::INT, depth - 1);
			return name + "(" + argument + ")";
		}

		if (choice == 1) {
			const std::string operand = generate_expression(type, depth - 1);
			return "-(" + operand + ")";
		}

		// A conditional expression, which is where the two arms have to agree
		// on a type.
		if (choice == 2) {
			const std::string when_true = generate_expression(type, depth - 1);
			const std::string condition = generate_expression(GenType::BOOL, depth - 1);
			const std::string when_false = generate_expression(type, depth - 1);
			return "(" + when_true + " if " + condition + " else " + when_false + ")";
		}

		// Mixing an int into a float expression: GDScript promotes, and getting
		// that wrong is one of the bugs this is looking for.
		const GenType left_type = type;
		const GenType right_type = (type == GenType::FLOAT && m_random.chance(40)) ? GenType::INT : type;

		const uint32_t op = m_random.below(type == GenType::INT ? 5 : 4);
		if (op == 3) {
			// The divisor is a literal, so that it is never zero.
			const std::string left = generate_expression(left_type, depth - 1);
			const std::string right = (type == GenType::FLOAT) ? float_nonzero_literal() : nonzero_int_literal();
			return "(" + left + " / " + right + ")";
		}
		if (op == 4) {
			const std::string left = generate_expression(GenType::INT, depth - 1);
			return "(" + left + " % " + nonzero_int_literal() + ")";
		}

		const char* symbol = (op == 0) ? " + " : (op == 1) ? " - " : " * ";
		const std::string left = generate_expression(left_type, depth - 1);
		const std::string right = generate_expression(right_type, depth - 1);
		return "(" + left + symbol + right + ")";
	}

	std::string float_nonzero_literal() {
		return std::to_string(1 + m_random.below(static_cast<uint32_t>(m_options.max_int_literal))) + ".5";
	}

	std::string generate_bool_expression(int depth) {
		const uint32_t choice = m_random.below(8);

		if (choice == 0) {
			const std::string operand = generate_expression(GenType::BOOL, depth - 1);
			return "(not " + operand + ")";
		}
		if (choice == 1 || choice == 2) {
			const char* symbol = (choice == 1) ? " and " : " or ";
			const std::string left = generate_expression(GenType::BOOL, depth - 1);
			const std::string right = generate_expression(GenType::BOOL, depth - 1);
			return "(" + left + symbol + right + ")";
		}
		if (choice == 3) {
			return generate_atom(GenType::BOOL);
		}

		// A comparison, over ints or over floats. Comparing an int with a float
		// is legal GDScript and worth generating.
		static const char* const comparisons[] = { "==", "!=", "<", "<=", ">", ">=" };
		const char* comparison = comparisons[m_random.below(6)];
		const GenType left_type = m_random.chance(60) ? GenType::INT : GenType::FLOAT;
		const GenType right_type = m_random.chance(70) ? left_type
			: (left_type == GenType::INT ? GenType::FLOAT : GenType::INT);
		const std::string left = generate_expression(left_type, depth - 1);
		const std::string right = generate_expression(right_type, depth - 1);
		return "(" + left + " " + comparison + " " + right + ")";
	}

	// -= Statements =-

	std::string declare(GenType type, int depth) {
		const std::string name = "v" + std::to_string(m_next_variable++);
		const std::string value = generate_expression(type, m_options.max_expression_depth - 1);
		// The declaration is in scope for everything after it, including the
		// inner scopes an if or a while opens.
		m_scopes.back().push_back({ name, type, /*assignable=*/true });
		std::string hint;
		if (m_random.chance(15)) {
			hint = type == GenType::BOOL ? ": bool | int"
				: ": int | float";
			m_scopes.back().back().union_hint = true;
		}
		return indent(depth) + "var " + name + hint + " = " + value + "\n";
	}

	std::string exercise_nullable(GenType type, int depth) {
		const std::string name = "maybe" + std::to_string(m_next_variable++);
		const char* type_name = type == GenType::BOOL ? "bool"
			: type == GenType::FLOAT ? "float" : "int";
		// Both the value and the fallback land in slots declared `T?` and `T`.
		const bool enclosing_proven = m_proven_types_only;
		m_proven_types_only = true;
		const std::string value = m_random.chance(40)
			? "null" : generate_expression(type, m_options.max_expression_depth - 1);
		std::string out = indent(depth) + "var " + name + ": " + type_name + "? = " + value + "\n";
		if (m_random.chance(40)) {
			// `??` is that same null check written as a value, so the fallback
			// runs exactly when the branch below would not be entered.
			const std::string fallback =
				generate_expression(type, m_options.max_expression_depth - 1);
			m_proven_types_only = enclosing_proven;
			const std::string sink = "v" + std::to_string(m_next_variable++);
			m_scopes.back().push_back({ sink, type, /*assignable=*/true });
			return out + indent(depth) + "var " + sink + ": " + type_name + " = " +
				name + " ?? (" + fallback + ")\n";
		}
		m_proven_types_only = enclosing_proven;
		out += indent(depth) + "if " + name + " != null:\n";
		if (type == GenType::BOOL) {
			out += indent(depth + 1) + name + " = not " + name + "\n";
		} else {
			out += indent(depth + 1) + name + " = " + name +
				(type == GenType::FLOAT ? " + 1.0\n" : " + 1\n");
		}
		return out;
	}

	std::string generate_statement(int depth, int nesting) {
		const uint32_t choice = m_random.below(12);
		if (choice == 11 && m_has_struct) {
			return exercise_struct(depth);
		}

		// A declaration that shadows an existing name, which is where locals
		// beating globals and inner blocks beating outer ones is decided.
		if (choice < 4 || m_scopes.back().empty()) {
			return declare(pick_type(), depth);
		}

		if (choice < 6) {
			// Assignment to a variable already in scope. Loop counters are
			// excluded: assigning to one is how a generated loop stops
			// terminating.
			const GenType type = pick_type();
			const auto candidates = variables_of_type(type, /*assignable_only=*/true);
			if (candidates.empty()) {
				return declare(type, depth);
			}
			const std::string name = candidates[m_random.below(static_cast<uint32_t>(candidates.size()))];
			const std::string value = generate_expression(type, m_options.max_expression_depth - 1);
			return indent(depth) + name + " = " + value + "\n";
		}

		if (choice < 8 && nesting < m_options.max_nesting) {
			return generate_if(depth, nesting);
		}

		if (choice == 10) {
			return exercise_nullable(pick_type(), depth);
		}

		if (m_options.allow_loops && nesting < m_options.max_nesting) {
			return generate_while(depth, nesting);
		}

		return declare(pick_type(), depth);
	}

	std::string exercise_struct(int depth) {
		const std::string suffix = std::to_string(m_next_struct++);
		const std::string point = "point" + suffix;
		const std::string copy = "point_copy" + suffix;
		const std::string x = generate_expression(GenType::INT,
			m_options.max_expression_depth - 1);
		const std::string y = generate_expression(GenType::INT,
			m_options.max_expression_depth - 1);
		std::string out = indent(depth) + "var " + point + ": FuzzPoint = FuzzPoint(" +
			x + ", " + y + ")\n";
		out += indent(depth) + point + ".x += " + nonzero_int_literal() + "\n";
		out += indent(depth) + "var " + copy + " = fuzz_point_round_trip(" +
			point + ".copy())\n";
		out += indent(depth) + "if " + copy + " is FuzzPoint:\n";
		out += indent(depth + 1) + point + ".y = " + copy + ".total()\n";
		return out;
	}

	std::string generate_if(int depth, int nesting) {
		std::string out = indent(depth) + "if " +
			generate_expression(GenType::BOOL, m_options.max_expression_depth - 1) + ":\n";

		push_scope();
		const int body = 1 + static_cast<int>(m_random.below(2));
		for (int i = 0; i < body; i++) {
			out += generate_statement(depth + 1, nesting + 1);
		}
		pop_scope();

		if (m_random.chance(40)) {
			out += indent(depth) + "else:\n";
			push_scope();
			const int else_body = 1 + static_cast<int>(m_random.below(2));
			for (int i = 0; i < else_body; i++) {
				out += generate_statement(depth + 1, nesting + 1);
			}
			pop_scope();
		}
		return out;
	}

	std::string generate_while(int depth, int nesting) {
		// Every generated loop counts down from a fixed bound, so a generated
		// program always terminates. A fuzzer that can emit an infinite loop
		// spends its time hanging rather than finding anything.
		const std::string counter = "i" + std::to_string(m_next_loop++);
		const int iterations = 1 + static_cast<int>(m_random.below(8));

		std::string out = indent(depth) + "var " + counter + " = " + std::to_string(iterations) + "\n";
		out += indent(depth) + "while " + counter + " > 0:\n";
		out += indent(depth + 1) + counter + " = " + counter + " - 1\n";

		push_scope();
		m_scopes.back().push_back({ counter, GenType::INT, /*assignable=*/false });
		const int body = 1 + static_cast<int>(m_random.below(2));
		for (int i = 0; i < body; i++) {
			out += generate_statement(depth + 1, nesting + 1);
		}

		// break and continue after the decrement, so the loop still ends.
		if (m_random.chance(25)) {
			const std::string condition = generate_expression(GenType::BOOL, 1);
			const bool use_break = m_random.chance(50);
			out += indent(depth + 1) + "if " + condition + ":\n";
			out += indent(depth + 2) + (use_break ? "break\n" : "continue\n");
		}
		pop_scope();

		// The counter stays in scope after the loop, as it does in GDScript,
		// and stays un-assignable: a later assignment cannot make the loop that
		// already ran misbehave, but keeping the rule in one place is simpler
		// than reasoning about which side of the loop a statement is on.
		m_scopes.back().push_back({ counter, GenType::INT, /*assignable=*/false });
		return out;
	}

	std::string generate_function(int index) {
		const std::string name = "helper" + std::to_string(index);

		// The helper's parameter is the only thing in scope inside it, so its
		// body cannot reach a variable of test().
		std::vector<std::vector<Variable>> saved_scopes;
		saved_scopes.swap(m_scopes);
		push_scope();
		m_scopes.back().push_back({ "a", GenType::INT, /*assignable=*/true });

		std::string out = "func " + name + "(a):\n";
		out += "\treturn " + generate_expression(GenType::INT, m_options.max_expression_depth - 1) + "\n";

		pop_scope();
		m_scopes.swap(saved_scopes);

		// Registered after the body is generated, so a helper cannot call
		// itself and recurse forever.
		m_function_names.push_back(name);
		return out;
	}
};

// -= Shrinking =-
//
// A failing generated program is usually mostly irrelevant. The shrinker
// repeatedly tries a smaller program and keeps it whenever it still fails, so
// what gets reported is the smallest program the shrinker could still break --
// which is usually small enough to read.

// `still_fails` returns true when a source still reproduces the failure.
template <typename Predicate>
GeneratedProgram shrink(const GeneratedProgram& original, Predicate still_fails) {
	GeneratedProgram best = original;

	bool progress = true;
	while (progress) {
		progress = false;

		// Delete one statement at a time, from the back, so that a deletion
		// which removes a declaration a later statement needs is tried before
		// the statement that needs it.
		for (size_t i = best.statements.size(); i-- > 0; ) {
			GeneratedProgram candidate = best;
			candidate.statements.erase(candidate.statements.begin() + static_cast<long>(i));
			if (still_fails(candidate.source())) {
				best = candidate;
				progress = true;
			}
		}

		// Simplify the returned expression to something trivial. If the program
		// still fails, whatever it was computing did not matter.
		if (best.result_expression != "0") {
			GeneratedProgram candidate = best;
			candidate.result_expression = "0";
			if (still_fails(candidate.source())) {
				best = candidate;
				progress = true;
			}
		}

		// Drop helper functions.
		for (size_t i = best.functions.size(); i-- > 0; ) {
			GeneratedProgram candidate = best;
			candidate.functions.erase(candidate.functions.begin() + static_cast<long>(i));
			if (still_fails(candidate.source())) {
				best = candidate;
				progress = true;
			}
		}
	}

	return best;
}

} // namespace gdscript_test
