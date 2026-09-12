#include "c_codegen.h"
#include "c_abi.h"
#include "globals.h"
#include "ir_verifier.h"
#include "ir_optimizer.h"
#include "syscall_numbers.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace gdscript {
namespace {
std::string quote(const std::string &s) {
    std::ostringstream o;
    o << '"';
    for (unsigned char c : s) {
        if (c >= 32 && c < 127 && c != '"' && c != '\\' && c != '?') o << c;
        else o << '\\' << std::oct << std::setw(3) << std::setfill('0') << unsigned(c) << std::dec;
    }
    return o.str() + '"';
}
std::string integer(int64_t i) {
    if (i == std::numeric_limits<int64_t>::min()) return "(-9223372036854775807LL-1)";
    return std::to_string(i) + "LL";
}
std::string floating(double d) {
    if (std::isnan(d)) return "(0.0/0.0)";
    if (std::isinf(d)) return d < 0 ? "(-1.0/0.0)" : "(1.0/0.0)";
    std::ostringstream o;
    o << std::scientific << std::setprecision(17) << d;
    return o.str();
}
// Only these functions have a direct, numeric engine ABI. Generic Variant
// utilities still need runtime dispatch (notably vector-valued floor/abs).
const char *engine_math(GlobalFn fn) {
    switch (fn) {
#define GJ_MATH_NAME(id, name, result, count) case GlobalFn::id: return "gj_math_" #name;
        GJ_ENGINE_MATH(GJ_MATH_NAME)
#undef GJ_MATH_NAME
        case GlobalFn::LERP: return "gj_math_lerpf";
        default: return nullptr;
    }
}
bool inline_math(GlobalFn fn) {
    switch (fn) {
        case GlobalFn::ABSI: case GlobalFn::ABSF:
        case GlobalFn::SIGNI: case GlobalFn::SIGNF:
        case GlobalFn::MINI: case GlobalFn::MINF:
        case GlobalFn::MAXI: case GlobalFn::MAXF:
        case GlobalFn::CLAMPI: case GlobalFn::CLAMPF:
        case GlobalFn::INT_IDENTITY: return true;
        default: return false;
    }
}
uint64_t parameter_mask(const IRProgram &p, const IRFunction &f, size_t parameter) {
    if (parameter < f.param_sets.size() && f.param_sets[parameter]) return f.param_sets[parameter];
    for (size_t j = 0; j < p.functions.size(); ++j) {
        if (p.functions[j].name != f.name || j >= p.signatures.size()) continue;
        const auto &signature = p.signatures[j];
        if (!signature.has_declaration || signature.parameters.size() > f.parameters.size()) return 0;
        const size_t implicit = f.parameters.size() - signature.parameters.size();
        if (parameter < implicit) return 0;
        const auto &param = signature.parameters[parameter - implicit];
        // Named script classes can use a different native representation;
        // nullable Object arguments deliberately retain their runtime tag.
        return param.type >= 1 && param.type < 39 && param.type != 24 && param.class_name.empty() ? uint64_t(1) << param.type : 0;
    }
    return 0;
}
// Flow-sensitive type sets, including loop backedges. The IR
// verifier establishes definite assignment before any read. Do not trust hints
// on dynamic arithmetic or globals, which can change across reentrant calls.
std::vector<int> proven_locals(const IRFunction &f, const IRProgram &p) {
    constexpr uint64_t unknown = ~uint64_t(0);
    std::vector<uint64_t> types(std::max(1, f.max_registers), 0);
    for (size_t j = 0; j < f.parameters.size(); ++j) {
        auto mask = parameter_mask(p, f, j);
        types[j] = mask ? mask : unknown;
    }
    // Reaching definitions keep an entry parameter separate from a later r0
    // return assignment. Union definitions only for the storage decision.
    const size_t n = f.instructions.size();
    std::vector<std::vector<uint64_t>> incoming(n, std::vector<uint64_t>(types.size()));
    std::vector<bool> reached(n), queued(n);
    std::vector<size_t> work;
    std::unordered_map<uint32_t, size_t> labels;
    for (size_t j = 0; j < n; ++j)
        if (f.instructions[j].opcode == IROpcode::LABEL) labels[f.instructions[j].operands[0].string_id] = j;
    if (n) {
        incoming[0] = types;
        if (f.parameters.empty()) incoming[0][0] = 1;
        reached[0] = queued[0] = true;
        work.push_back(0);
    }
    while (!work.empty()) {
        const size_t at = work.back(); work.pop_back(); queued[at] = false;
        const auto &i = f.instructions[at];
        auto state = incoming[at];
        std::vector<int> reads;
        ir_collect_read_registers(i, reads);
        for (int r : reads) types[r] |= state[r];
        int d = ir_destination_register(i);
        if (d >= 0) {
            auto src = [&](int n) { return state[i.operands[n].reg_index()]; };
            uint64_t t = unknown;
            switch (i.opcode) {
            case IROpcode::LOAD_IMM: case IROpcode::TYPE_OF: t = 4; break;
            case IROpcode::LOAD_FLOAT_IMM: t = 8; break;
            case IROpcode::LOAD_BOOL: case IROpcode::TYPE_TEST: case IROpcode::TYPE_TEST_MASK:
            case IROpcode::CMP_EQ: case IROpcode::CMP_NEQ: case IROpcode::CMP_LT:
            case IROpcode::CMP_LTE: case IROpcode::CMP_GT: case IROpcode::CMP_GTE:
            case IROpcode::AND: case IROpcode::OR: case IROpcode::NOT: t = 2; break;
            case IROpcode::LOAD_NIL: t = 1; break;
            case IROpcode::CONSTRUCT:
                t = i.operands[1].immediate() >= 1 && i.operands[1].immediate() < 39 ? uint64_t(1) << i.operands[1].immediate() : unknown; break;
            case IROpcode::LOAD_STRING: t = uint64_t(1) << 4; break;
            case IROpcode::MAKE_ARRAY: t = uint64_t(1) << 28; break;
            case IROpcode::MAKE_DICTIONARY: case IROpcode::MAKE_DICTIONARY_KEYED: t = uint64_t(1) << 27; break;
            case IROpcode::MAKE_CALLABLE: t = uint64_t(1) << 25; break;
            case IROpcode::MAKE_VECTOR2: case IROpcode::MAKE_VECTOR3: case IROpcode::MAKE_VECTOR4:
            case IROpcode::MAKE_VECTOR2I: case IROpcode::MAKE_VECTOR3I: case IROpcode::MAKE_VECTOR4I:
            case IROpcode::MAKE_COLOR: case IROpcode::MAKE_RECT2: case IROpcode::MAKE_RECT2I: case IROpcode::MAKE_PLANE: {
                const int ids[] = {5,9,12,6,10,13,20,7,8,14};
                t = uint64_t(1) << ids[int(i.opcode) - int(IROpcode::MAKE_VECTOR2)]; break;
            }
            case IROpcode::CALL: case IROpcode::CALL_HOSTED:
                for (const auto &callee : p.functions)
                    if (callee.name == p.strings[i.operands[0].string_id]) {
                        if (callee.return_set) t = callee.return_set;
                        else if (callee.return_type_hint >= 1 && callee.return_type_hint < 39 && callee.return_type_hint != 24)
                            t = uint64_t(1) << callee.return_type_hint;
                    }
                break;
            case IROpcode::VCALL: {
                const auto &name = p.strings[i.operands[2].string_id];
                const auto v = src(1);
                if (v == (1ULL << 5) || v == (1ULL << 9) || v == (1ULL << 12)) {
                    if (name == "normalized" || name == "lerp") t = v;
                    if (name == "length" || name == "length_squared" || name == "distance_to" ||
                        name == "distance_squared_to" || name == "dot") t = 8;
                } else if (!v) t = 0;
                break;
            }
            case IROpcode::VGET: case IROpcode::VGET_INLINE: {
                const auto &name = i.opcode == IROpcode::VGET ? p.string_constants.at(i.operands[2].immediate()) : p.strings[i.operands[2].string_id];
                const auto v = src(1);
                const int width = v == (1ULL << 5) ? 2 : v == (1ULL << 9) ? 3 : v == (1ULL << 12) ? 4 : 0;
                if (width && name.size() == 1 && std::string("xyzw").substr(0, width).find(name) != std::string::npos) t = 8;
                else if (!v) t = 0;
                break;
            }
            case IROpcode::MOVE: t = src(1); break;
            case IROpcode::GLOBAL_CALL:
                if (engine_math(static_cast<GlobalFn>(i.operands[1].immediate())) ||
                    inline_math(static_cast<GlobalFn>(i.operands[1].immediate()))) {
                    auto result = global_function(static_cast<GlobalFn>(i.operands[1].immediate())).result;
                    t = result == GlobalResult::FLOAT ? 8 : result == GlobalResult::INT ? 4 :
                        result == GlobalResult::BOOL ? 2 : unknown;
                }
                if (static_cast<GlobalFn>(i.operands[1].immediate()) == GlobalFn::TO_FLOAT ||
                    static_cast<GlobalFn>(i.operands[1].immediate()) == GlobalFn::FLOAT_IDENTITY) t = 8;
                if (static_cast<GlobalFn>(i.operands[1].immediate()) == GlobalFn::TO_INT) t = 4;
                break;
            case IROpcode::CONVERT: case IROpcode::COERCE:
                t = i.type_hint >= 1 && i.type_hint < 39 ? uint64_t(1) << i.type_hint : unknown; break;
            case IROpcode::ADD: case IROpcode::SUB: case IROpcode::MUL: case IROpcode::DIV:
                if (!src(1) || !src(2)) t = 0;
                else if (!((src(1) | src(2)) & ~uint64_t(12)))
                    t = ((src(1) & src(2) & 4) ? 4 : 0) | (((src(1) | src(2)) & 8) ? 8 : 0);
                else if (src(1) == src(2) && (src(1) == (1ULL << 5) || src(1) == (1ULL << 9) || src(1) == (1ULL << 12))) t = src(1);
                else if ((i.opcode == IROpcode::MUL || i.opcode == IROpcode::DIV) &&
                    (src(1) == (1ULL << 5) || src(1) == (1ULL << 9) || src(1) == (1ULL << 12)) && !(src(2) & ~uint64_t(12))) t = src(1);
                break;
            case IROpcode::MOD: case IROpcode::BIT_AND: case IROpcode::BIT_OR:
            case IROpcode::BIT_XOR: case IROpcode::SHL: case IROpcode::SHR:
                t = !src(1) || !src(2) ? 0 : src(1) == 4 && src(2) == 4 ? 4 : unknown; break;
            case IROpcode::NEG: case IROpcode::BIT_NOT:
                t = src(1) == 4 ? 4 : src(1) == 0 ? 0 : unknown; break;
            default: break;
            }
            state[d] = t;
            types[d] |= t;
        }
        auto merge = [&](size_t edge) {
            bool changed = !reached[edge];
            reached[edge] = true;
            for (size_t r = 0; r < state.size(); ++r) {
                auto value = incoming[edge][r] | state[r];
                if (value != incoming[edge][r]) { incoming[edge][r] = value; changed = true; }
            }
            if (changed && !queued[edge]) { queued[edge] = true; work.push_back(edge); }
        };
        if (!ir_has_effect(i.opcode, IR_TERMINATOR) && at + 1 < n) merge(at + 1);
        if (i.opcode != IROpcode::LABEL)
            for (const auto &v : i.operands)
                if (v.type == IRValue::Type::LABEL) merge(labels.at(v.string_id));
    }
    std::vector<int> result(types.size(), -1);
    for (size_t j = 0; j < types.size(); ++j)
        for (int t = 0; t < 39; ++t) if (types[j] == (uint64_t(1) << t)) result[j] = t;
    return result;
}
// Backward fixed point over instruction edges. A last use on only one branch
// is not a move; loop backedges and implicit r0 returns participate too.
std::vector<std::vector<bool>> live_after(const IRFunction &f) {
    const size_t n = f.instructions.size(), regs = std::max(1, f.max_registers);
    std::vector<std::vector<bool>> in(n, std::vector<bool>(regs)), after = in;
    std::unordered_map<uint32_t, size_t> labels;
    for (size_t j = 0; j < n; ++j)
        if (f.instructions[j].opcode == IROpcode::LABEL) labels[f.instructions[j].operands[0].string_id] = j;
    bool changed;
    do {
        changed = false;
        for (size_t at = n; at-- > 0;) {
            const auto &i = f.instructions[at];
            std::vector<bool> next(regs);
            auto merge = [&](size_t edge) { for (size_t r = 0; r < regs; ++r) next[r] = next[r] || in[edge][r]; };
            if (!ir_has_effect(i.opcode, IR_TERMINATOR)) {
                if (at + 1 < n) merge(at + 1);
                else next[0] = true;
            }
            if (i.opcode != IROpcode::LABEL)
                for (const auto &v : i.operands)
                    if (v.type == IRValue::Type::LABEL) merge(labels.at(v.string_id));
            after[at] = next;
            for (size_t j = 0; j < i.operands.size(); ++j)
                if (ir_writes_operand(i, j)) next[i.operands[j].reg_index()] = false;
            std::vector<int> reads;
            ir_collect_read_registers(i, reads);
            for (int r : reads) next[r] = true;
            if (next != in[at]) { in[at] = std::move(next); changed = true; }
        }
    } while (changed);
    return after;
}
struct Emitter {
    const IRProgram &p;
    std::ostringstream out;
    std::unordered_map<std::string, size_t> functions;
    std::vector<int> scalars;
    std::vector<bool> boxes;
    bool debug = false;
    std::vector<int> kinds;
    std::vector<std::string> names;
    std::vector<bool> typed_functions;
    std::vector<bool> borrow_safe;
    explicit Emitter(const IRProgram &program, bool debugging = false) : p(program), debug(debugging) {}
    static int return_kind(const IRFunction &f) {
        if (f.return_set) return (f.return_set & (f.return_set - 1)) ? -1 : __builtin_ctzll(f.return_set);
        return f.return_type_hint;
    }
    static const char *ctype(int type) { return type == 3 ? "double" : components(type) ? "GJReal" : "GJInt"; }
    bool typed_abi(const IRFunction &f) {
        const int result = return_kind(f);
        if (debug || f.is_coroutine || !((result >= 1 && result <= 3) || components(result))) return false;
        for (size_t j = 0; j < f.parameters.size(); ++j) {
            const auto mask = parameter_mask(p, f, j);
            if (!mask || (mask & 1) || (mask & (mask - 1))) return false;
        }
        return true;
    }
    std::string typed_signature(const IRFunction &f, size_t index) {
        std::string result = "static int f" + std::to_string(index) + "_t(GJContext *ctx," + ctype(return_kind(f)) + " *result";
        for (size_t j = 0; j < f.parameters.size(); ++j) {
            int type = __builtin_ctzll(parameter_mask(p, f, j));
            result += "," + std::string(components(type) ? "const GJReal *" : type <= 3 ? ctype(type) : "const GJVariant *") + " p" + std::to_string(j);
        }
        return result + ")";
    }
    void typed_wrapper(const IRFunction &f, size_t index, bool validated) {
        const int type = return_kind(f), width = components(type);
        out << "static int f" << index << (validated ? "_v" : "") << "(GJContext *ctx,GJVariant *result,const GJVariant *const *args,int count) {\n";
        out << "  int ok = 0; " << ctype(type) << " value" << (width ? "[" + std::to_string(width) + "]" : "") << ";\n";
        if (!validated) {
            for (size_t j = 0; j < f.parameters.size(); ++j)
                out << "  GJVariant converted" << j << "; converted" << j << ".type = 0;\n";
            out << "  if (count != " << f.parameters.size() << ") { gj_fail(ctx," << quote("Wrong argument count for " + f.name) << "); goto cleanup; }\n";
            for (size_t j = 0; j < f.parameters.size(); ++j) {
                const int t = __builtin_ctzll(parameter_mask(p, f, j));
                out << "  const GJVariant *p" << j << " = args[" << j << "];\n"
                    << "  if (p" << j << "->type != " << t << ") { if (!gj_op(ctx,GJ_COERCE,&converted" << j << ",0,-1," << t
                    << ",&p" << j << ",1)) goto cleanup; p" << j << " = &converted" << j << "; }\n";
            }
        }
        out << "  ok = f" << index << "_t(ctx," << (width ? "value" : "&value");
        for (size_t j = 0; j < f.parameters.size(); ++j) {
            int t = __builtin_ctzll(parameter_mask(p, f, j));
            out << ',' << (validated ? "args[" + std::to_string(j) + "]" : "p" + std::to_string(j))
                << (t == 1 ? "->data.b" : t == 2 ? "->data.i" : t == 3 ? "->data.f" : components(t) ? "->data.real" : "");
        }
        out << ");\n  if (ok) { gj_clear(result); result->type = " << type << ";\n";
        if (width) for (int j = 0; j < width; ++j) out << "    result->data.real[" << j << "] = value[" << j << "];\n";
        else out << "    result->data." << (type == 1 ? "b" : type == 2 ? "i" : "f") << " = value;\n";
        out << "  }\n";
        if (!validated) {
            out << "cleanup:\n";
            for (size_t j = 0; j < f.parameters.size(); ++j) out << "  gj_clear(&converted" << j << ");\n";
        }
        out << "  return ok;\n}\n";
    }
    int kind(const std::string &r) const {
        return r.size() > 1 && r[0] == 'r' && r[1] >= '0' && r[1] <= '9' ? kinds.at(std::stoi(r.substr(1))) : -1;
    }
    static int components(int type) { return type == 5 ? 2 : type == 9 ? 3 : type == 12 ? 4 : 0; }
    static int value_width(int type) {
        switch (type) {
            case 5: case 6: return 2;
            case 9: case 10: return 3;
            case 7: case 8: case 12: case 13: case 14: case 15: case 20: return 4;
            case 23: return 1;
            default: return 0;
        }
    }
    static const char *value_field(int type) {
        return type == 6 || type == 8 || type == 10 || type == 13 ? "integer" : type == 20 ? "color" : type == 23 ? "words" : "real";
    }
    static const char *value_ctype(int type) {
        return type == 6 || type == 8 || type == 10 || type == 13 ? "int" : type == 20 ? "float" : type == 23 ? "GJUInt" : "GJReal";
    }
    std::string component(const std::string &r, int j) const {
        return (value_width(kind(r)) ? "v" + r.substr(1) : r + ".data.real") + "[" + std::to_string(j) + "]";
    }
    void vector_result(const std::string &d, int type, const std::vector<std::string> &values) {
        // Unboxed component-wise writes cannot destroy an operand or affect a
        // later component. Boxed destinations may release reentrant Objects,
        // so snapshot all components before clearing those destinations.
        if (value_width(kind(d))) {
            for (size_t j = 0; j < values.size(); ++j) out << "  " << component(d, j) << " = " << values[j] << ";\n";
            return;
        }
        out << "  {\n";
        for (size_t j = 0; j < values.size(); ++j) out << "    " << value_ctype(type) << " c" << j << " = " << values[j] << ";\n";
        if (!value_width(kind(d))) {
            require_box("&" + d);
            out << "    gj_clear(&" << d << "); " << d << ".type = " << type << ";\n";
        }
        for (size_t j = 0; j < values.size(); ++j)
            out << "    " << (value_width(kind(d)) ? component(d, j) : d + ".data." + value_field(type) + "[" + std::to_string(j) + "]") << " = c" << j << ";\n";
        out << "  }\n";
    }
    bool vector_member(const IRInstruction &i, bool write, const std::string &name) {
        const auto &a = i.operands;
        const auto receiver = r(a[write ? 0 : 1]);
        const int width = components(kind(receiver));
        const auto field = name.size() == 1 ? std::string("xyzw").find(name) : std::string::npos;
        if (!width || field >= size_t(width)) return false;
        if (write) {
            if (scalar(r(a[3])) < 2) return false;
            out << "  " << component(receiver, field) << " = " << payload(r(a[3]), scalar(r(a[3]))) << ";\n";
        } else set(r(a[0]), 3, component(receiver, field));
        return true;
    }
    int scalar(const std::string &r) const {
        return r.size() > 1 && r[0] == 'r' && r[1] >= '0' && r[1] <= '9' ? scalars.at(std::stoi(r.substr(1))) : -1;
    }
    void require_box(const std::string &address) {
        if (address.size() > 2 && address[0] == '&' && address[1] == 'r' &&
            address[2] >= '0' && address[2] <= '9') boxes.at(std::stoi(address.substr(2))) = true;
    }
    const char *setter(int t) { return t == 1 ? "gj_bool" : t == 2 ? "gj_int" : "gj_float"; }
    std::string payload(const std::string &r, int t) {
        return scalar(r) >= 0 ? "s" + r.substr(1) : r + (t == 1 ? ".data.b" : t == 2 ? ".data.i" : ".data.f");
    }
    void box(const std::string &address) {
        require_box(address);
        if (address.size() > 2 && address[0] == '&' && value_width(kind(address.substr(1)))) {
            const auto r = address.substr(1);
            for (int j = 0; j < value_width(kind(r)); ++j)
                out << "  " << r << ".data." << value_field(kind(r)) << "[" << j << "] = " << component(r, j) << ";\n";
        }
        if (address.size() > 2 && address[0] == '&' && scalar(address.substr(1)) >= 0) {
            auto r = address.substr(1); int t = scalar(r);
            out << "  " << r << (t == 1 ? ".data.b" : t == 2 ? ".data.i" : ".data.f") << " = " << payload(r,t) << ";\n";
        }
    }
    void unbox(const std::string &address) {
        require_box(address);
        if (address.size() > 2 && address[0] == '&' && value_width(kind(address.substr(1)))) {
            const auto r = address.substr(1);
            for (int j = 0; j < value_width(kind(r)); ++j)
                out << "  " << component(r, j) << " = " << r << ".data." << value_field(kind(r)) << "[" << j << "];\n";
        }
        if (address.size() > 2 && address[0] == '&' && scalar(address.substr(1)) >= 0) {
            auto r = address.substr(1); int t = scalar(r);
            out << "  " << payload(r,t) << " = " << r << (t == 1 ? ".data.b" : t == 2 ? ".data.i" : ".data.f") << ";\n";
        }
    }
    void set(const std::string &r, int t, const std::string &expression) {
        if (scalar(r) == t) out << "  " << payload(r,t) << " = " << expression << ";\n";
        else { require_box("&" + r); out << "  " << setter(t) << "(&" << r << ',' << expression << ");\n"; }
    }
    void move(const std::string &d, const std::string &s) {
        if (scalar(s) >= 0) set(d, scalar(s), payload(s,scalar(s)));
        else if (value_width(kind(s))) {
            std::vector<std::string> values;
            for (int j = 0; j < value_width(kind(s)); ++j) values.push_back(component(s, j));
            vector_result(d, kind(s), values);
        } else { require_box("&" + d); box("&" + s); out << "  gj_move(&" << d << ",&" << s << ");\n"; unbox("&" + d); }
    }
    std::string truth(const std::string &r) {
        if (scalar(r) >= 0) return "(" + payload(r, scalar(r)) + " != 0)";
        box("&" + r);
        return "gj_boolean(&" + r + ")";
    }
    std::string r(const IRValue &v) { return "r" + std::to_string(v.reg_index()); }
    std::string label(const IRValue &v) { return "L" + std::to_string(v.string_id); }
    std::string constant(const IRValue &v) { return quote(p.string_constants.at(v.immediate())); }
    std::string name(const IRValue &v) { return quote(p.strings[v.string_id]); }
    void op(const std::string &kind, const std::string &dst, const std::string &self = "0",
            const std::string &n = "0", int detail = -1, const std::vector<std::string> &args = {}) {
        int name_id = -1;
        if (n != "0") {
            auto found = std::find(names.begin(), names.end(), n);
            name_id = found - names.begin();
            if (found == names.end()) names.push_back(n);
        }
        box(self);
        for (const auto &a : args) box(a);
        out << "  { const GJVariant *a[" << std::max<size_t>(1, args.size()) << "] = {";
        for (size_t i = 0; i < args.size(); ++i) out << (i ? "," : "") << args[i];
        if (args.empty()) out << "0";
        out << "}; if (!gj_op(ctx," << kind << ',' << dst << ',' << self << ',' << name_id << ','
            << detail << ",a," << args.size() << ")) goto cleanup; }\n";
        unbox(dst);
    }
    void method(const IRInstruction &i) {
        const auto &a = i.operands;
        const auto d = r(a[0]), x = r(a[1]);
        const auto &n = p.strings[a[2].string_id];
        const auto av = args(i, 4);
        const bool normalize = n == "normalized" && av.empty();
        const bool length = (n == "length" || n == "length_squared") && av.empty();
        const bool distance = (n == "distance_to" || n == "distance_squared_to") && av.size() == 1;
        const bool dot = n == "dot" && av.size() == 1;
        const int width = components(kind(x));
        if (!i.super_call && width && (normalize || length || ((distance || dot) && kind(r(a[4])) == kind(x)))) {
            if (normalize) {
                out << "  { GJReal normalized[" << width << "]; gj_vector" << width
                    << "_normalized(v" << x.substr(1) << ",0,normalized,0);\n";
                std::vector<std::string> values;
                for (int j = 0; j < width; ++j) values.push_back("normalized[" + std::to_string(j) + "]");
                vector_result(d, kind(x), values);
                out << "  }\n";
            } else {
                out << "  {\n";
                for (int j = 0; j < width; ++j)
                    out << "    GJReal c" << j << " = " << component(x, j)
                        << (distance ? " - " + component(r(a[4]), j) : "") << ";\n";
                out << "    GJReal length = ";
                for (int j = 0; j < width; ++j)
                    out << (j ? " + " : "") << "c" << j << " * " << (dot ? component(r(a[4]), j) : "c" + std::to_string(j));
                out << ";\n";
                set(d, 3, n == "length" || n == "distance_to" ? "gj_sqrt_real(length)" : "length");
                out << "  }\n";
            }
            return;
        }
        if (!i.super_call && (normalize || length || distance || dot)) {
            box("&" + x);
            const auto y = av.empty() ? x : r(a[4]);
            if (!av.empty()) box("&" + y);
            for (int components = 2; components <= 4; ++components) {
                const int type = components == 2 ? 5 : components == 3 ? 9 : 12;
                out << (components == 2 ? "  if (" : "  } else if (") << x << ".type == " << type;
                if (!av.empty()) out << " && " << y << ".type == " << type;
                out << ") {\n";
                if (normalize) {
                    // TinyCC cannot inline sqrt; one direct engine call beats
                    // spilling each component around a scalar helper call and
                    // preserves 4.6+ zero/underflow/nonfinite behavior exactly.
                    out << "    GJReal normalized[" << components << "];\n"
                        << "    gj_vector" << components << "_normalized(" << x << ".data.real, 0, normalized, 0);\n";
                    require_box("&" + d);
                    out << "    gj_clear(&" << d << "); " << d << ".type = " << type << ";\n";
                    for (int j = 0; j < components; ++j)
                        out << "    " << d << ".data.real[" << j << "] = normalized[" << j << "];\n";
                    continue;
                }
                // Snapshot every component before writing a possibly aliased output.
                for (int j = 0; j < components; ++j) {
                    out << "    GJReal v" << j << " = " << x << ".data.real[" << j << "]";
                    if (distance) out << " - " << y << ".data.real[" << j << "]";
                    out << ";\n";
                }
                out << "    GJReal l = ";
                for (int j = 0; j < components; ++j) {
                    out << (j ? " + " : "") << "v" << j << " * ";
                    if (dot) out << y << ".data.real[" << j << "]";
                    else out << "v" << j;
                }
                out << ";\n";
                if (n == "length" || n == "distance_to") out << "    l = gj_sqrt_real(l);\n";
                set(d, 3, "l");
            }
            out << "  } else {\n";
            op("GJ_CALL", "&" + d, "&" + x, name(a[2]), -1, av);
            out << "  }\n";
            if (normalize && value_width(kind(d))) unbox("&" + d);
        } else {
            const char *kind = i.super_call ? "GJ_SUPER_CALL" :
                n == "size" && av.empty() ? "GJ_ARRAY_SIZE" : "GJ_CALL";
            op(kind, "&" + d, "&" + x, name(a[2]), -1, av);
        }
    }
    void numeric_math(const std::string &d, GlobalFn fn, const std::vector<std::string> &values) {
        const auto &info = global_function(fn);
        const int t = info.result == GlobalResult::INT ? 2 : info.result == GlobalResult::BOOL ? 1 : 3;
        out << "    {\n";
        // Force the same input precision as Godot before arithmetic; locals also
        // make selection expressions safe when the destination aliases an input.
        for (size_t j = 0; j < values.size(); ++j)
            out << "      " << (t == 2 && info.kind == GlobalKind::INT_OP ? "GJInt" : "double")
                << " m" << j << " = " << values[j] << ";\n";
        std::string expression;
        switch (fn) {
        case GlobalFn::INT_IDENTITY: expression = "m0"; break;
        case GlobalFn::ABSI: expression = "m0 < 0 ? (GJInt)(0ULL - (GJUInt)m0) : m0"; break;
        case GlobalFn::ABSF:
            // Clear the sign bit, including negative zero and signed NaNs.
            out << "      union { double f; GJUInt u; } bits; bits.f = m0; bits.u &= 0x7fffffffffffffffULL;\n";
            expression = "bits.f"; break;
        case GlobalFn::SIGNI: case GlobalFn::SIGNF: expression = "m0 < 0 ? -1 : m0 > 0 ? 1 : 0"; break;
        case GlobalFn::MINI: case GlobalFn::MINF: expression = "m0 < m1 ? m0 : m1"; break;
        case GlobalFn::MAXI: case GlobalFn::MAXF: expression = "m0 > m1 ? m0 : m1"; break;
        case GlobalFn::CLAMPI: case GlobalFn::CLAMPF: expression = "m0 < m1 ? m1 : m0 > m2 ? m2 : m0"; break;
        default:
            expression = std::string(engine_math(fn)) + "(";
            for (size_t j = 0; j < values.size(); ++j) expression += (j ? ",m" : "m") + std::to_string(j);
            expression += ")";
        }
        set(d, t, expression);
        out << "    }\n";
    }
    bool math(const IRInstruction &i) {
        const auto &a = i.operands;
        const auto fn = static_cast<GlobalFn>(a[1].immediate());
        const auto &info = global_function(fn);
        const bool generic = info.kind == GlobalKind::NUMERIC;
        // Generic clamp performs two Variant comparisons (also with reversed
        // bounds), and preserves the chosen operand's type. Keep its engine ABI.
        if (fn == GlobalFn::CLAMP) return false;
        auto supported = [](GlobalFn f) { return inline_math(f) || engine_math(f); };
        if (generic ? !supported(info.int_form) || !supported(info.float_form) : !supported(fn)) return false;
        if (a.size() - 4 < info.min_args || a.size() - 4 > info.max_args) return false;
        // Variadic min/max are normally lowered pairwise by the frontend.
        if (generic && a.size() > 6) return false;
        std::string numbers, ints;
        std::vector<std::string> floats, integers;
        bool all_numbers = true, all_ints = true;
        for (size_t j = 4; j < a.size(); ++j) {
            auto x = r(a[j]); const int t = scalar(x);
            if (t < 2) {
                box("&" + x);
                numbers += (numbers.empty() ? "" : " && ") + std::string("(") + x + ".type == 2 || " + x + ".type == 3)";
                all_numbers = false;
            }
            if (t != 2) {
                if (t < 0) ints += (ints.empty() ? "" : " && ") + x + ".type == 2";
                else ints += (ints.empty() ? "" : " && ") + std::string("0");
                all_ints = false;
            }
            floats.push_back(t >= 2 ? payload(x,t) : "gj_number(&" + x + ")");
            integers.push_back(t >= 2 ? payload(x,t) : x + ".data.i");
        }
        // Typed integer primitives accept only integer fast-path inputs. Float
        // conversion, bools, and other Variant coercions retain host semantics.
        const bool integer_only = !generic && info.kind == GlobalKind::INT_OP;
        const bool guarded = integer_only ? !all_ints : !all_numbers;
        if (guarded) out << "  if (" << (integer_only ? ints : numbers) << ") {\n";
        if (fn == GlobalFn::MIN || fn == GlobalFn::MAX) {
            // Generic selection keeps the first operand on ties/NaNs, and
            // returns the selected Variant's original type even for mixed pairs.
            auto select = [&](bool integer) {
                const auto &v = integer ? integers : floats;
                auto cast = [&](const std::string &x) { return integer ? x : "(double)(" + x + ")"; };
                out << "    if (" << cast(v[0]) << (fn == GlobalFn::MIN ? " > " : " < ") << cast(v[1]) << ") {\n";
                move(r(a[0]), r(a[5]));
                out << "    } else {\n";
                move(r(a[0]), r(a[4]));
                out << "    }\n";
            };
            if (!all_ints) out << "    if (" << ints << ") {\n";
            select(true);
            if (!all_ints) {
                out << "    } else {\n";
                select(false);
                out << "    }\n";
            }
        } else if (generic) {
            if (!all_ints) out << "    if (" << ints << ") {\n";
            numeric_math(r(a[0]),info.int_form,integers);
            if (!all_ints) {
                out << "    } else {\n";
                numeric_math(r(a[0]),info.float_form,floats);
                out << "    }\n";
            }
        } else numeric_math(r(a[0]),fn,integer_only ? integers : floats);
        if (guarded) {
            out << "  } else {\n";
            op("GJ_UTILITY", "&" + r(a[0]), "0", quote(info.name), int(fn), args(i,4));
            out << "  }\n";
        }
        return true;
    }
    std::vector<std::string> args(const IRInstruction &i, size_t start) {
        std::vector<std::string> a;
        for (size_t j = start; j < i.operands.size(); ++j) a.push_back("&" + r(i.operands[j]));
        return a;
    }
    void fail(const std::string &message) { out << "  gj_fail(ctx," << quote(message) << "); goto cleanup;\n"; }
    [[noreturn]] void unsupported(const IRFunction &f, const IRInstruction &i) {
        throw std::runtime_error("C backend: " + f.name + ":" + std::to_string(i.line) +
            ": unsupported " + ir_opcode_name(i.opcode) + " (requires script runtime or sandbox batching)");
    }
    void arithmetic(const IRInstruction &i, int operation, const char *symbol, bool comparison = false) {
        const auto &a = i.operands;
        auto d = r(a[0]), x = r(a[1]), y = r(a[2]);
        int tx = scalar(x), ty = scalar(y);
        const int width = components(kind(x));
        if (width && symbol && operation >= 6 && operation <= 9 &&
            (kind(x) == kind(y) || ((operation == 8 || operation == 9) && ty >= 2))) {
            std::vector<std::string> values;
            for (int j = 0; j < width; ++j)
                values.push_back(component(x, j) + symbol + (kind(x) == kind(y) ? component(y, j) : "(GJReal)(" + payload(y, ty) + ")"));
            vector_result(d, kind(x), values);
            return;
        }
        if (symbol && tx >= 2 && ty >= 2 && (operation <= 9 || (tx == 2 && ty == 2))) {
            auto px = payload(x,tx), py = payload(y,ty);
            const int t = comparison ? 1 : tx == 3 || ty == 3 ? 3 : 2;
            if ((operation == 9 || operation == 12) && tx == 2 && ty == 2) {
                out << "  if (!" << py << ") {"; fail("Integer division by zero"); out << "  }\n";
                set(d,t,"(" + px + " == (-9223372036854775807LL-1) && " + py + " == -1) ? " +
                    (operation == 9 ? "(-9223372036854775807LL-1)" : "0") + " : (" + px + symbol + py + ")");
            } else {
                bool wrap = t == 2 && (operation == 6 || operation == 7 || operation == 8 || operation == 14);
                if (operation == 14 || operation == 15) py = "(" + py + " & 63)";
                set(d,t,(wrap ? "(GJInt)((GJUInt)" : "(") + px + symbol + py + ")");
            }
            return;
        }
        if (comparison && symbol && ((tx >= 2) != (ty >= 2))) {
            // Keep a proven scalar operand and the boolean result unboxed even
            // when the other side comes from a dynamic built-in. Integer pairs
            // must compare as integers (including values beyond double precision).
            const auto &dynamic = tx >= 2 ? y : x;
            const int known_type = tx >= 2 ? tx : ty;
            box("&" + dynamic);
            for (int t : {2, 3}) {
                out << (t == 2 ? "  if (" : "  } else if (") << dynamic << ".type == " << t << ") {\n";
                const auto px = tx >= 2 ? payload(x, known_type) : payload(x, t);
                const auto py = ty >= 2 ? payload(y, known_type) : payload(y, t);
                set(d, 1, "(" + px + symbol + py + ")");
            }
            out << "  } else {\n";
            op("GJ_EVALUATE", "&" + d, "0", "0", operation, {"&" + x, "&" + y});
            out << "  }\n";
            return;
        }
        box("&" + x); box("&" + y); require_box("&" + d);
        // Runtime tag tests also cover union/untyped values. Operands and payloads
        // are individual C locals, so even TinyCC has no register-file indexing.
        if (symbol) {
            out << "  if (" << x << ".type == 2 && " << y << ".type == 2) {\n";
            if (operation == 9 || operation == 12) {
                out << "    if (!" << y << ".data.i) {";
                fail("Integer division by zero"); out << "    }\n";
                out << "    if (" << x << ".data.i == (-9223372036854775807LL-1) && " << y << ".data.i == -1) gj_int(&" << d << ','
                    << (operation == 9 ? "(-9223372036854775807LL-1)" : "0") << "); else\n";
            }
            out << "    " << (comparison ? "gj_bool" : "gj_int") << "(&" << d << ",";
            const bool wrap = operation == 6 || operation == 7 || operation == 8 || operation == 14;
            if (wrap) out << "(GJInt)((GJUInt)";
            out << x << ".data.i" << symbol;
            if (operation == 14 || operation == 15) out << "(" << y << ".data.i & 63)";
            else out << y << ".data.i";
            if (wrap) out << ')';
            out << ");\n  }";
            if (operation <= 9) {
                out << " else if ((" << x << ".type == 2 || " << x << ".type == 3) && (" << y << ".type == 2 || " << y << ".type == 3)) {\n"
                    << "    " << (comparison ? "gj_bool" : "gj_float") << "(&" << d << ",gj_number(&" << x << ')' << symbol << "gj_number(&" << y << "));\n  }";
            }
            if (operation == 6 || operation == 7) {
                for (const auto &vector : {std::pair<int,int>{5,2}, {9,3}, {12,4}}) {
                    out << " else if (" << x << ".type == " << vector.first << " && " << y << ".type == " << vector.first << ") {\n";
                    for (int k = 0; k < vector.second; ++k)
                        out << "    GJReal v" << k << " = " << x << ".data.real[" << k << "]" << symbol << y << ".data.real[" << k << "];\n";
                    out << "    gj_clear(&" << d << "); " << d << ".type = " << vector.first << ";\n";
                    for (int k = 0; k < vector.second; ++k)
                        out << "    " << d << ".data.real[" << k << "] = v" << k << ";\n";
                    out << "  }";
                }
            }
            out << " else {\n";
        }
        op("GJ_EVALUATE", "&" + d, "0", "0", operation, {"&" + x, "&" + y});
        if (symbol) out << "  }\n";
        unbox("&" + d);
    }
    std::string global(size_t index) {
        return p.globals.at(index).is_member() ? "&ctx->globals[" + std::to_string(index) + "]" :
            "&(ctx->shared_globals ? ctx->shared_globals : ctx->globals)[" + std::to_string(index) + "]";
    }
    int array_step(const IRFunction &f, size_t at, const std::vector<int> &reads,
                    const std::vector<int> &writes, std::vector<bool> &used) {
        if (at + 3 >= f.instructions.size()) return false;
        const auto &size = f.instructions[at];
        const auto &cmp = f.instructions[at + 1];
        const auto &branch = f.instructions[at + 2];
        const auto &get = f.instructions[at + 3];
        if (size.opcode != IROpcode::CALL_SYSCALL || size.operands[1].immediate() != ECALL_ARRAY_SIZE ||
            cmp.opcode != IROpcode::CMP_LT || branch.opcode != IROpcode::BRANCH_ZERO ||
            get.opcode != IROpcode::CALL_SYSCALL || get.operands[1].immediate() != ECALL_ARRAY_AT) return false;
        const int length = size.operands[0].reg_index(), condition = cmp.operands[0].reg_index();
        const int index = cmp.operands[1].reg_index(), array = size.operands[2].reg_index();
        // Fuse only private size/comparison temporaries, with no intervening
        // instructions or labels. The helper still checks size on every step.
        if (reads[length] != 1 || writes[length] != 1 || reads[condition] != 1 || writes[condition] != 1 ||
            cmp.operands[2].reg_index() != length || branch.operands[0].reg_index() != condition ||
            get.operands[2].reg_index() != array || get.operands[3].reg_index() != index || scalars[index] != 2)
            return false;
        const auto item = "&" + r(get.operands[0]), container = "&" + r(size.operands[2]);
        if (kinds[array] == 28 && at + 4 < f.instructions.size()) {
            const auto &coerce = f.instructions[at + 4];
            const int raw = get.operands[0].reg_index();
            if (coerce.opcode == IROpcode::COERCE && coerce.operands[1].reg_index() == raw &&
                reads[raw] == 1 && writes[raw] == 1 && components(coerce.type_hint) &&
                kind(r(coerce.operands[0])) == coerce.type_hint) {
                box(container);
                out << "  { int step = gj_array_next_vector(ctx,v" << coerce.operands[0].reg_index() << ',' << container << ','
                    << payload(r(cmp.operands[1]), 2) << ',' << coerce.type_hint << ");\n"
                    << "    if (step < 0) goto cleanup;\n    if (!step) goto " << label(branch.operands[1]) << "; }\n";
                used[raw] = false;
                return 5;
            }
        }
        require_box(item); box(container);
        out << "  { int step = gj_array_next(ctx," << item << ',' << container << ','
            << payload(r(cmp.operands[1]), 2) << ");\n"
            << "    if (step < 0) goto cleanup;\n"
            << "    if (!step) goto " << label(branch.operands[1]) << "; }\n";
        unbox(item);
        return 4;
    }
    void function(const IRFunction &f, size_t index) {
        const bool typed = index < typed_functions.size() && typed_functions[index];
        kinds = proven_locals(f, p);
        scalars = kinds;
        for (auto &t : scalars) if (t < 1 || t > 3) t = -1;
        const auto live = live_after(f);
        // Every coroutine local must survive suspension as an owned Variant.
        if (debug || f.is_coroutine) { std::fill(scalars.begin(), scalars.end(), -1); std::fill(kinds.begin(), kinds.end(), -1); }
        boxes.assign(scalars.size(),false);
        std::vector<bool> used(scalars.size(), false);
        std::vector<int> reads(scalars.size(), 0), writes(scalars.size(), 0);
        for (const auto &i : f.instructions) {
            std::vector<int> input;
            ir_collect_read_registers(i, input);
            for (int r : input) ++reads[r];
            for (size_t j = 0; j < i.operands.size(); ++j)
                if (ir_writes_operand(i, j)) ++writes[i.operands[j].reg_index()];
        }
        std::vector<bool> borrowed(scalars.size()), parameter_copies(f.parameters.size(), true);
        std::vector<int> borrowed_from(scalars.size(), -1), entry_coercions(f.instructions.size(), -1);
        if (typed && borrow_safe[index])
            for (size_t j = 0; j < f.parameters.size(); ++j)
                if (writes[j] == 0 && kinds[j] >= 4 && !gj_trivial(kinds[j])) { borrowed[j] = true; borrowed_from[j] = j; }
        if (typed) {
            std::vector<bool> touched(scalars.size());
            for (size_t at = 0; at < f.instructions.size(); ++at) {
                const auto &i = f.instructions[at];
                if (i.opcode == IROpcode::LABEL || ir_has_effect(i.opcode, IR_BRANCH) || ir_has_effect(i.opcode, IR_TERMINATOR)) break;
                if (i.opcode == IROpcode::MOVE || i.opcode == IROpcode::COERCE) {
                    const int dst = i.operands[0].reg_index(), src = i.operands[1].reg_index();
                    if (size_t(src) < f.parameters.size() && !touched[src] && !live[at][src]) {
                        const int type = __builtin_ctzll(parameter_mask(p, f, src));
                        if (i.opcode == IROpcode::COERCE && i.type_hint == type && type <= 3) {
                            entry_coercions[at] = src;
                            parameter_copies[src] = false;
                        } else if (i.opcode == IROpcode::MOVE && borrow_safe[index] && writes[dst] == 1 &&
                            kinds[dst] == type && !gj_trivial(type)) {
                            borrowed[dst] = true;
                            borrowed_from[dst] = src;
                            parameter_copies[src] = false;
                        }
                    }
                }
                for (const auto &v : i.operands) if (v.type == IRValue::Type::REGISTER) touched[v.reg_index()] = true;
            }
        }
        used[0] = true;
        if (debug || f.is_coroutine) std::fill(used.begin(), used.end(), true);
        for (const auto &i : f.instructions)
            for (const auto &v : i.operands)
                if (v.type == IRValue::Type::REGISTER) used[v.reg_index()] = true;
        auto prefix = std::move(out);
        out = std::ostringstream();
        const int registers = std::max(1, f.max_registers);
        if (f.is_coroutine) {
            out << "  int suspended = 0;\n  void *resuming = count == -1 ? ctx->resuming : 0;\n";
            out << "  GJVariant *slots[" << registers << "] = {";
            for (int j = 0; j < registers; ++j) out << (j ? "," : "") << "&r" << j;
            out << "};\n";
        }
        if (debug) {
            out << "  const GJVariant *debug_locals[" << registers << "] = {";
            for (int j = 0; j < registers; ++j) out << (j ? "," : "") << "&r" << j;
            out << "};\n  GJDebugFrame debug_frame = {" << index << ",0,0,debug_locals," << registers << ","
                << (!f.parameters.empty() && f.parameters.front() == "self" ? "count > 0 ? args[0] : ctx->self" : "ctx->self") << "};\n"
                << "  if (ctx->debug) ctx->debug(ctx,&debug_frame,GJ_DEBUG_ENTER);\n";
        }
        if (f.is_coroutine) {
            out << "  if (resuming) { ctx->resuming = 0; switch (gj_await_restore(ctx,resuming,slots," << registers << ")) {\n";
            for (size_t at = 0; at < f.instructions.size(); ++at)
                if (f.instructions[at].opcode == IROpcode::AWAIT)
                    out << "    case " << at << ": goto resume" << at << ";\n";
            out << "    default: gj_fail(ctx,\"Invalid coroutine resume state\"); goto cleanup;\n  }}\n";
        }
        if (!typed) { out << "  if (count != " << f.parameters.size() << ") {"; fail("Wrong argument count for " + f.name); out << "  }\n"; }
        // Unreferenced parameters need no local ownership (notably unused
        // container arguments). Arity and runtime signature checks still apply.
        for (size_t j = 0; j < f.parameters.size(); ++j)
            if (used[j] && !borrowed[j] && parameter_copies[j]) {
                const auto rj = "r" + std::to_string(j), arg = "args[" + std::to_string(j) + "]";
                const auto mask = parameter_mask(p, f, j);
                const int type = mask && !(mask & (mask - 1)) ? __builtin_ctzll(mask) : -1;
                if (typed) {
                    const auto input = "p" + std::to_string(j);
                    if (type >= 1 && type <= 3) set(rj, type, input);
                    else if (components(type)) {
                        std::vector<std::string> values;
                        for (int k = 0; k < components(type); ++k) values.push_back(input + "[" + std::to_string(k) + "]");
                        vector_result(rj, type, values);
                    } else { require_box("&" + rj); out << "  gj_move(&" << rj << ',' << input << ");\n"; unbox("&" + rj); }
                } else if (!debug && !f.is_coroutine && type >= 1 && type != 24) {
                    out << "  if (" << arg << "->type == " << type << ") {\n";
                    if (scalar(rj) == type) set(rj, type, arg + (type == 1 ? "->data.b" : type == 2 ? "->data.i" : "->data.f"));
                    else { require_box("&" + rj); out << "  gj_move(&" << rj << ',' << arg << ");\n"; unbox("&" + rj); }
                    out << "  } else {\n";
                    op("GJ_COERCE", "&" + rj, "0", "0", type, {arg});
                    out << "  }\n";
                } else {
                    require_box("&" + rj);
                    out << "  gj_move(&" << rj << ',' << arg << ");\n";
                    unbox("&" + rj);
                }
            }
        for (size_t at = 0; at < f.instructions.size(); ++at) {
            if (!debug) {
                int step = array_step(f, at, reads, writes, used);
                if (step) { at += step - 1; continue; }
            }
            const auto &i = f.instructions[at];
            const auto &a = i.operands;
            if (entry_coercions[at] >= 0) {
                if (live[at][a[0].reg_index()]) set(r(a[0]), i.type_hint, "p" + std::to_string(entry_coercions[at]));
                continue;
            }
            if (i.opcode == IROpcode::MOVE && borrowed[a[0].reg_index()]) continue;
            out << "  /* " << ir_opcode_name(i.opcode) << " line " << i.line << " */\n";
            // Place line hooks after labels so loop backedges visit them too.
            if (debug && i.opcode != IROpcode::LABEL) {
                out << "  debug_frame.instruction = " << at << ";\n";
                if (i.line > 0) out << "  debug_frame.line = " << i.line << ";\n";
                if (i.line > 0)
                    out << "  if (ctx->debug) ctx->debug(ctx,&debug_frame,"
                        << (i.opcode == IROpcode::BREAKPOINT ? "GJ_DEBUG_BREAKPOINT" : "GJ_DEBUG_LINE") << ");\n";
            }
            auto reg = [&](size_t j) { auto address = "&" + r(a[j]); require_box(address); return address; };
            switch (i.opcode) {
            case IROpcode::LOAD_IMM: set(r(a[0]),2,integer(a[1].immediate())); break;
            case IROpcode::LOAD_FLOAT_IMM: set(r(a[0]),3,floating(a[1].float_number())); break;
            case IROpcode::LOAD_BOOL: set(r(a[0]),1,std::to_string(a[1].immediate())); break;
            case IROpcode::LOAD_NIL: out << "  gj_clear(" << reg(0) << ");\n"; break;
            case IROpcode::LOAD_STRING: case IROpcode::LOAD_STRING_AS:
                op("GJ_STRING", reg(0), "0", constant(a[1]), p.string_constants.at(a[1].immediate()).size());
                if (i.opcode == IROpcode::LOAD_STRING_AS) op("GJ_CONSTRUCT", reg(0), "0", "0", a[2].immediate(), {reg(0)});
                break;
            case IROpcode::MOVE:
                if (!debug && !f.is_coroutine && scalar(r(a[0])) < 0 && scalar(r(a[1])) < 0 &&
                    !value_width(kind(r(a[1]))) && !value_width(kind(r(a[0]))) &&
                    !borrowed[a[1].reg_index()] &&
                    a[0].reg_index() != a[1].reg_index() && !live[at][a[1].reg_index()])
                    out << "  gj_take(" << reg(0) << ',' << reg(1) << ");\n";
                else move(r(a[0]),r(a[1]));
                break;
            case IROpcode::LOAD_GLOBAL: out << "  gj_move(" << reg(0) << "," << global(a[1].immediate()) << ");\n"; break;
            case IROpcode::STORE_GLOBAL: box(reg(1)); out << "  gj_move(" << global(a[0].immediate()) << "," << reg(1) << ");\n"; break;
            case IROpcode::CONVERT: case IROpcode::COERCE:
                if (kind(r(a[1])) >= 1 && kind(r(a[1])) == i.type_hint) move(r(a[0]),r(a[1]));
                else if (scalar(r(a[1])) >= 0 && i.type_hint == 3)
                    set(r(a[0]),3,payload(r(a[1]),scalar(r(a[1]))));
                else {
                    box(reg(1));
                    out << "  if (" << r(a[1]) << ".type == " << i.type_hint << ") {\n";
                    if (i.type_hint >= 1 && i.type_hint <= 3)
                        set(r(a[0]),i.type_hint,payload(r(a[1]),i.type_hint));
                    else { out << "  gj_move(" << reg(0) << ',' << reg(1) << ");\n"; unbox(reg(0)); }
                    out << "  } else {\n";
                    op(i.opcode == IROpcode::COERCE ? "GJ_COERCE" : "GJ_CONSTRUCT", reg(0), "0", "0", i.type_hint, {reg(1)});
                    out << "  }\n";
                }
                break;
            case IROpcode::TYPE_OF: case IROpcode::TYPE_TEST: case IROpcode::TYPE_TEST_MASK: {
                const auto input = r(a[1]);
                if (kind(input) < 0) require_box("&" + input);
                const auto type = kind(input) >= 0 ? std::to_string(kind(input)) : input + ".type";
                if (i.opcode == IROpcode::TYPE_OF) set(r(a[0]), 2, type);
                else if (i.opcode == IROpcode::TYPE_TEST)
                    set(r(a[0]), 1, type + " == " + std::to_string(a[2].immediate()) +
                        (a[2].immediate() == 24 ? " && " + truth(input) : ""));
                else set(r(a[0]), 1, "(" + integer(a[2].immediate()) + " & (1ULL << " + type + ")) != 0");
                break;
            }
            case IROpcode::SCOPE_MARK: case IROpcode::SCOPE_RELEASE: break;
            case IROpcode::BREAKPOINT: break; // Handled by the debug hook above.
            case IROpcode::ADD: arithmetic(i, 6, "+"); break;
            case IROpcode::SUB: arithmetic(i, 7, "-"); break;
            case IROpcode::MUL: arithmetic(i, 8, "*"); break;
            case IROpcode::DIV: arithmetic(i, 9, "/"); break;
            case IROpcode::MOD: arithmetic(i, 12, "%"); break;
            case IROpcode::POW: arithmetic(i, 13, nullptr); break;
            case IROpcode::CMP_EQ: arithmetic(i, 0, "==", true); break;
            case IROpcode::CMP_NEQ: arithmetic(i, 1, "!=", true); break;
            case IROpcode::CMP_LT: arithmetic(i, 2, "<", true); break;
            case IROpcode::CMP_LTE: arithmetic(i, 3, "<=", true); break;
            case IROpcode::CMP_GT: arithmetic(i, 4, ">", true); break;
            case IROpcode::CMP_GTE: arithmetic(i, 5, ">=", true); break;
            case IROpcode::BIT_AND: arithmetic(i, 16, "&"); break;
            case IROpcode::BIT_OR: arithmetic(i, 17, "|"); break;
            case IROpcode::BIT_XOR: arithmetic(i, 18, "^"); break;
            case IROpcode::SHL: arithmetic(i, 14, "<<"); break;
            case IROpcode::SHR: arithmetic(i, 15, ">>"); break;
            case IROpcode::IN: arithmetic(i, 24, nullptr); break;
            case IROpcode::AND: case IROpcode::OR:
                set(r(a[0]), 1, truth(r(a[1])) + (i.opcode == IROpcode::AND ? " && " : " || ") + truth(r(a[2]))); break;
            case IROpcode::NOT: set(r(a[0]), 1, "!" + truth(r(a[1]))); break;
            case IROpcode::NEG: case IROpcode::BIT_NOT:
                box(reg(1));
                out << "  if (" << r(a[1]) << ".type == 2) gj_int(" << reg(0) << ", (GJInt)" << (i.opcode == IROpcode::NEG ? "(0ULL - (GJUInt)" : "(~(GJUInt)") << r(a[1]) << ".data.i)); else {\n";
                op("GJ_EVALUATE", reg(0), "0", "0", i.opcode == IROpcode::NEG ? 10 : 19, {reg(1)}); out << "  }\n"; break;
            case IROpcode::LABEL: out << label(a[0]) << ":;\n"; break;
            case IROpcode::JUMP: out << "  goto " << label(a[0]) << ";\n"; break;
            case IROpcode::BRANCH_ZERO: case IROpcode::BRANCH_NOT_ZERO:
                out << "  if (" << (i.opcode == IROpcode::BRANCH_ZERO ? "!" : "") << (scalar(r(a[0])) >= 0 ? "(" + payload(r(a[0]),scalar(r(a[0]))) + ")" : "gj_boolean(" + reg(0) + ")") << ") goto " << label(a[1]) << ";\n"; break;
            case IROpcode::BRANCH_EQ: case IROpcode::BRANCH_NEQ: case IROpcode::BRANCH_LT:
            case IROpcode::BRANCH_LTE: case IROpcode::BRANCH_GT: case IROpcode::BRANCH_GTE:
                op("GJ_EVALUATE", "&temp", "0", "0", int(i.opcode) - int(IROpcode::BRANCH_EQ), {reg(0), reg(1)});
                out << "  if (gj_boolean(&temp)) goto " << label(a[2]) << ";\n"; break;
            case IROpcode::SWITCH:
                box(reg(0));
                out << "  if (" << r(a[0]) << ".type == 2) switch (" << r(a[0]) << ".data.i) {\n";
                for (size_t j = 3; j < a.size(); ++j) out << "    case " << integer(int64_t(uint64_t(a[1].immediate()) + j - 3)) << ": goto " << label(a[j]) << ";\n";
                out << "  }\n"; break;
            // Native coroutine entries return their completion Signal directly;
            // the sandbox-only host trampoline is unnecessary here.
            case IROpcode::CALL:
            case IROpcode::CALL_HOSTED: {
                auto it = functions.find(p.strings[a[0].string_id]);
                if (it == functions.end()) throw std::runtime_error("C backend: unknown function " + p.strings[a[0].string_id]);
                const auto &callee = p.functions[it->second];
                bool typed_call = typed_functions[it->second] && a.size() - 3 == callee.parameters.size();
                for (size_t j = 3; typed_call && j < a.size(); ++j) {
                    int type = __builtin_ctzll(parameter_mask(p, callee, j - 3));
                    typed_call = kind(r(a[j])) == type || (type == 3 && scalar(r(a[j])) == 2);
                }
                if (typed_call) {
                    int result_type = return_kind(callee), width = components(result_type);
                    const auto dst = r(a[1]);
                    const bool direct = kind(dst) == result_type;
                    for (size_t j = 3; j < a.size(); ++j)
                        if (value_width(kind(r(a[j]))) && !components(kind(r(a[j])))) box("&" + r(a[j]));
                    out << "  {\n";
                    if (!direct) out << "    " << ctype(result_type) << " value" << (width ? "[" + std::to_string(width) + "]" : "") << ";\n";
                    out << "    if (!f" << it->second << "_t(ctx," << (direct ? width ? "v" + dst.substr(1) : "&" + payload(dst, result_type) : width ? "value" : "&value");
                    for (size_t j = 3; j < a.size(); ++j) {
                        const int type = __builtin_ctzll(parameter_mask(p, callee, j - 3));
                        const auto input = r(a[j]);
                        if (components(type)) out << ",v" << input.substr(1);
                        else if (type <= 3) out << ',' << payload(input, scalar(input));
                        else { require_box("&" + input); out << ",&" << input; }
                    }
                    out << ")) goto cleanup;\n";
                    if (!direct) {
                        if (width) {
                            std::vector<std::string> values;
                            for (int j = 0; j < width; ++j) values.push_back("value[" + std::to_string(j) + "]");
                            vector_result(dst, result_type, values);
                        } else set(dst, result_type, "value");
                    }
                    out << "  }\n";
                    break;
                }
                auto av = args(i, 3);
                for (const auto &arg : av) box(arg);
                out << "  { const GJVariant *a[" << std::max<size_t>(1,av.size()) << "] = {";
                for (size_t j = 0; j < av.size(); ++j) out << (j ? "," : "") << av[j];
                if (av.empty()) out << '0';
                out << "}; if (!f" << it->second << "(ctx," << reg(1) << ",a," << av.size() << ")) goto cleanup; }\n"; unbox(reg(1)); break;
            }
            case IROpcode::RETURN:
                if (typed && kind("r0") != return_kind(f)) {
                    const bool assigned = at > 0 && f.instructions[at - 1].opcode == IROpcode::MOVE &&
                        f.instructions[at - 1].operands[0].reg_index() == 0 &&
                        kind(r(f.instructions[at - 1].operands[1])) == return_kind(f);
                    if (!assigned) {
                        box("&r0");
                        out << "  if (r0.type != " << return_kind(f) << ") {\n";
                        op("GJ_COERCE", "&r0", "0", "0", return_kind(f), {"&r0"});
                        out << "  }\n";
                    }
                }
                out << "  goto cleanup;\n"; break;
            case IROpcode::VCALL: method(i); break;
            case IROpcode::VGET:
                if (!vector_member(i, false, p.string_constants.at(a[2].immediate()))) op("GJ_GET_NAMED", reg(0), reg(1), constant(a[2])); break;
            case IROpcode::VSET:
                if (!vector_member(i, true, p.string_constants.at(a[1].immediate()))) { op("GJ_SET_NAMED", "&temp", reg(0), constant(a[1]), -1, {reg(3)}); unbox(reg(0)); } break;
            case IROpcode::VGET_INLINE:
                if (!vector_member(i, false, p.strings[a[2].string_id])) op("GJ_GET_NAMED", reg(0), reg(1), name(a[2])); break;
            case IROpcode::VSET_INLINE:
                if (!vector_member(i, true, p.strings[a[1].string_id])) { op("GJ_SET_NAMED", "&temp", reg(0), name(a[1]), -1, {reg(3)}); unbox(reg(0)); } break;
            case IROpcode::ARRAY_GET: op("GJ_GET", reg(0), reg(1), "0", -1, {reg(2)}); break;
            case IROpcode::VARIANT_SET: case IROpcode::ARRAY_SET: case IROpcode::DICT_SET:
                op("GJ_SET", "&temp", reg(0), "0", -1, {reg(1), reg(2)}); unbox(reg(0)); break;
            case IROpcode::ARRAY_APPEND: op("GJ_CALL", reg(0), reg(1), "\"append\"", -1, {reg(2)}); break;
            case IROpcode::DICT_GET_CONST: case IROpcode::DICT_HAS_CONST:
                op(i.opcode == IROpcode::DICT_GET_CONST ? "GJ_GET_NAMED" : "GJ_DICTIONARY_HAS", reg(0), reg(1), constant(a[2])); break;
            case IROpcode::DICT_SET_CONST: case IROpcode::DICT_SET_CONST_STR:
                op("GJ_SET_NAMED", "&temp", reg(0), constant(a[1]), i.opcode == IROpcode::DICT_SET_CONST_STR ? 4 : 21, {reg(2)}); break;
            case IROpcode::MAKE_ARRAY: case IROpcode::MAKE_DICTIONARY:
                op(i.opcode == IROpcode::MAKE_ARRAY ? "GJ_ARRAY" : "GJ_DICTIONARY", reg(0), "0", "0", -1, args(i, 2)); break;
            case IROpcode::MAKE_DICTIONARY_KEYED: case IROpcode::STRUCT_CHECK: {
                size_t start = i.opcode == IROpcode::STRUCT_CHECK ? 3 : 2;
                out << "  {\n";
                // Constant keys go directly to the native Dictionary.
                if (i.opcode == IROpcode::MAKE_DICTIONARY_KEYED) op("GJ_DICTIONARY", reg(0));
                else out << "  gj_bool(" << reg(0) << ",1);\n";
                for (size_t j = start; j < a.size(); j += i.opcode == IROpcode::STRUCT_CHECK ? 1 : 2) {
                    if (i.opcode == IROpcode::MAKE_DICTIONARY_KEYED) op("GJ_SET_NAMED", "&temp", reg(0), constant(a[j]), 21, {reg(j+1)});
                    else {
                        op("GJ_DICTIONARY_HAS", "&temp", reg(1), constant(a[j]));
                        out << "  gj_bool(" << reg(0) << ",gj_boolean(" << reg(0) << ") && gj_boolean(&temp));\n";
                    }
                }
                if (i.opcode == IROpcode::STRUCT_CHECK) {
                    op("GJ_STRUCT_CHECK", "&temp", reg(1), "0", a[2].immediate());
                    out << "  gj_bool(" << reg(0) << ",gj_boolean(" << reg(0) << ") && gj_boolean(&temp));\n";
                }
                out << "  }\n"; break;
            }
            case IROpcode::MAKE_PACKED_BYTE_ARRAY: case IROpcode::MAKE_PACKED_INT32_ARRAY:
            case IROpcode::MAKE_PACKED_INT64_ARRAY: case IROpcode::MAKE_PACKED_FLOAT32_ARRAY:
            case IROpcode::MAKE_PACKED_FLOAT64_ARRAY: case IROpcode::MAKE_PACKED_STRING_ARRAY:
            case IROpcode::MAKE_PACKED_VECTOR2_ARRAY: case IROpcode::MAKE_PACKED_VECTOR3_ARRAY:
            case IROpcode::MAKE_PACKED_COLOR_ARRAY: case IROpcode::MAKE_PACKED_VECTOR4_ARRAY:
                op("GJ_PACKED_ARRAY", reg(0), "0", "0", 29 + int(i.opcode) - int(IROpcode::MAKE_PACKED_BYTE_ARRAY), args(i, 2)); break;
            case IROpcode::CONSTRUCT:
                if (a.size() == 4 && scalar(r(a[3])) >= 0 &&
                    (scalar(r(a[3])) == a[1].immediate() || a[1].immediate() == 3))
                    set(r(a[0]),a[1].immediate(),payload(r(a[3]),scalar(r(a[3]))));
                else op("GJ_CONSTRUCT", reg(0), "0", "0", a[1].immediate(), args(i,3));
                break;
            case IROpcode::MAKE_VECTOR2: case IROpcode::MAKE_VECTOR3: case IROpcode::MAKE_VECTOR4:
            case IROpcode::MAKE_VECTOR2I: case IROpcode::MAKE_VECTOR3I: case IROpcode::MAKE_VECTOR4I:
            case IROpcode::MAKE_COLOR: case IROpcode::MAKE_RECT2: case IROpcode::MAKE_RECT2I: case IROpcode::MAKE_PLANE: {
                const int types[] = {5,9,12,6,10,13,20,7,8,14};
                const int type = types[int(i.opcode)-int(IROpcode::MAKE_VECTOR2)];
                const int components = type == 5 ? 2 : type == 9 ? 3 : type == 12 ? 4 : 0;
                bool numeric = components && a.size() == size_t(components + 1);
                for (size_t j = 1; numeric && j < a.size(); ++j) numeric = scalar(r(a[j])) >= 2;
                if (numeric) {
                    std::vector<std::string> values;
                    // Godot converts integer arguments through double first.
                    for (int j = 0; j < components; ++j) values.push_back("(double)(" + payload(r(a[j+1]), scalar(r(a[j+1]))) + ")");
                    vector_result(r(a[0]), type, values);
                } else op("GJ_CONSTRUCT", reg(0), "0", "0", type, args(i,1));
                break;
            }
            case IROpcode::GLOBAL_CALL:
                if (math(i)) break;
                if (a.size() == 5 && static_cast<GlobalFn>(a[1].immediate()) == GlobalFn::TO_INT) {
                    box(reg(4));
                    out << "  if (" << r(a[4]) << ".type == 2) {\n";
                    set(r(a[0]),2,r(a[4]) + ".data.i");
                    out << "  } else {\n";
                    op("GJ_UTILITY",reg(0),"0", "0", a[1].immediate(),args(i,4));
                    out << "  }\n";
                    break;
                }
                if (a.size() == 5 && scalar(r(a[4])) >= 0 &&
                    (static_cast<GlobalFn>(a[1].immediate()) == GlobalFn::TO_FLOAT ||
                     static_cast<GlobalFn>(a[1].immediate()) == GlobalFn::FLOAT_IDENTITY)) {
                    set(r(a[0]),3,payload(r(a[4]),scalar(r(a[4])))); break;
                }
                op("GJ_UTILITY", reg(0), "0", quote(global_function(static_cast<GlobalFn>(a[1].immediate())).name), a[1].immediate(), args(i,4)); break;
            case IROpcode::PRINT: op("GJ_PRINT", reg(0), "0", "0", a[1].immediate(), args(i,3)); break;
            case IROpcode::THROW: fail(p.strings[a[0].string_id] + ": " + p.strings[a[1].string_id]); break;
            case IROpcode::LOAD_RESOURCE: op("GJ_LOAD", reg(0), "0", name(a[1])); break;
            case IROpcode::LOAD_RESOURCE_VAR: op("GJ_LOAD", reg(0), "0", "0", -1, {reg(1)}); break;
            case IROpcode::GET_NODE: op("GJ_GET_NODE", reg(0), "ctx->self", name(a[1])); break;
            case IROpcode::MAKE_CALLABLE:
                op("GJ_CALLABLE", reg(0), reg(2), name(a[1]),
                    functions.count(p.strings[a[1].string_id]) ? int(functions.at(p.strings[a[1].string_id])) : -1); break;
            case IROpcode::CALL_SYSCALL: {
                const int number = a[1].immediate();
                switch (number) {
                case ECALL_CLASS_BIND:
                    op("GJ_CLASS_BIND", reg(4), reg(4), constant(a[2])); break;
                case ECALL_ARRAY_SIZE: case ECALL_STRING_SIZE:
                    op(number == ECALL_ARRAY_SIZE ? "GJ_ARRAY_SIZE" : "GJ_CALL", reg(0), reg(2), number == ECALL_ARRAY_SIZE ? "\"size\"" : "\"length\""); break;
                case ECALL_ARRAY_AT: case ECALL_STRING_AT: case ECALL_VARIANT_GET:
                    op("GJ_GET", reg(0), reg(2), "0", -1, {reg(3)}); break;
                case ECALL_NODE_CREATE: case ECALL_GET_OBJ:
                    op(number == ECALL_NODE_CREATE ? "GJ_NEW_OBJECT" : "GJ_GET_OBJECT", reg(0), "ctx->self", constant(a[2])); break;
                case ECALL_DICTIONARY_OPS: {
                    const int dop = a[2].immediate();
                    const char *methods[] = {"get", "set", "erase", "has", "keys", "values", "size", "clear", "merge", "get_or_add"};
                    if (dop >= 0 && dop < 10) op("GJ_CALL", reg(0), reg(3), quote(methods[dop]), -1, args(i,4));
                    else if (dop == int(Dictionary_Op::GET_OR_DEFAULT)) op("GJ_CALL", reg(0), reg(3), "\"get\"", -1, args(i,4));
                    else throw std::runtime_error("C backend: unsupported dictionary syscall " + std::to_string(dop));
                    break;
                }
                default: throw std::runtime_error("C backend: " + f.name + ": unsupported syscall " + std::to_string(number));
                }
                break;
            }
            case IROpcode::TRAIT_TEST:
                op("GJ_TRAIT_TEST", reg(0), reg(1), "0", a[2].immediate()); break;
            case IROpcode::AWAIT:
                out << "  suspended = gj_await(ctx,resuming," << index << "," << at
                    << ",result," << reg(1) << ",slots," << registers << "," << a[0].reg_index() << ");\n"
                    << "  if (suspended) goto cleanup;\nresume" << at << ":;\n";
                break;
            case IROpcode::MAKE_SCOPED: case IROpcode::BATCH_GET: case IROpcode::CODEPOINT_GET:
                unsupported(f, i);
            }
            switch (i.opcode) {
            case IROpcode::NEG: case IROpcode::BIT_NOT:
                unbox(reg(0)); break;
            default: break;
            }
        }
        out << "cleanup:\n";
        if (debug) out << "  if (ctx->debug) ctx->debug(ctx,&debug_frame,GJ_DEBUG_EXIT);\n";
        if (typed) {
            const int type = return_kind(f);
            out << "  if (!ctx->failed) {\n";
            if (components(type)) {
                if (!components(kind("r0"))) require_box("&r0");
                for (int j = 0; j < components(type); ++j) out << "    result[" << j << "] = " << component("r0", j) << ";\n";
            } else {
                if (scalar("r0") < 0) require_box("&r0");
                out << "    *result = " << payload("r0", type) << ";\n";
            }
            out << "  }\n";
        } else {
            box("&r0");
            out << "  if (!ctx->failed" << (f.is_coroutine ? " && !suspended" : "") << ") "
                << (scalars[0] < 0 ? "gj_take" : "gj_move") << "(result,&r0);\n";
        }
        for (int j = 0; j < registers; ++j) if (used[j] && !borrowed[j] && (kinds[j] < 0 || !gj_trivial(kinds[j]))) { boxes[j] = true; out << "  gj_clear(&r" << j << ");\n"; }
        const bool needs_temp = out.str().find("&temp") != std::string::npos;
        if (needs_temp) out << "  gj_clear(&temp);\n";
        out << "  return !ctx->failed;\n}\n";
        auto body = out.str();
        out = std::move(prefix);
        if (typed) out << typed_signature(f, index) << " {\n";
        else out << "static int f" << index << "(GJContext *ctx, GJVariant *result, const GJVariant *const *args, int count) {\n";
        if (needs_temp) out << "  GJVariant temp; temp.type = 0; temp.reserved = 0; temp.data.i = 0;\n";
        for (int j = 0; j < registers; ++j) {
            if (borrowed[j]) {
                out << "#define r" << j << " (*(GJVariant *)p" << borrowed_from[j] << ")\n";
                continue;
            }
            if (boxes[j])
                // TinyCC lowers aggregate zero initializers to memset calls.
                // A nil/scalar Variant only needs its tag and scalar payload;
                // the remaining union bytes are inactive storage.
                out << "  GJVariant r" << j << "; r" << j << ".type = " << (kinds[j] >= 0 && gj_trivial(kinds[j]) ? kinds[j] : 0)
                    << "; r" << j << ".reserved = 0; r" << j << ".data.i = 0;\n";
            if (scalars[j] >= 0)
                out << "  " << (scalars[j] == 3 ? "double" : "GJInt") << " s" << j << " = 0; (void)s" << j << ";\n";
            if (value_width(kinds[j]))
                out << "  " << value_ctype(kinds[j]) << " v" << j << "[" << value_width(kinds[j]) << "];\n";
        }
        out << body;
        for (int j = 0; j < registers; ++j) if (borrowed[j]) out << "#undef r" << j << "\n";
        if (typed) { typed_wrapper(f, index, false); typed_wrapper(f, index, true); }
    }
    std::string generate() {
        ir_verify(p);
        out << "/* Generated native C99. Compile with c_abi.h on the include path. */\n#ifndef GODOT_JIT_C_ABI_H\n#include \"c_abi.h\"\n#endif\n";
        for (size_t j = 0; j < p.functions.size(); ++j) functions.emplace(p.functions[j].name, j);
        for (const auto &f : p.functions) typed_functions.push_back(typed_abi(f));
        // Borrow only across code that cannot reenter scripts. A caller may
        // pass a member slot, and a callback could replace that slot mid-call.
        borrow_safe.assign(p.functions.size(), true);
        for (size_t j = 0; j < p.functions.size(); ++j) {
            const auto known = proven_locals(p.functions[j], p);
            for (const auto &i : p.functions[j].instructions) {
                const auto &a = i.operands;
                if ((i.opcode == IROpcode::CONSTRUCT && a[1].immediate() == 4) ||
                    ((i.opcode == IROpcode::COERCE || i.opcode == IROpcode::CONVERT) && i.type_hint == 4)) {
                    borrow_safe[j] = false; continue;
                }
                if (i.opcode == IROpcode::CALL || i.opcode == IROpcode::CALL_HOSTED) continue;
                if (i.opcode == IROpcode::VCALL) {
                    const int type = known[a[1].reg_index()];
                    const auto &name = p.strings[a[2].string_id];
                    if (components(type) || ((type == 28 || type == 4) && (name == "size" || name == "length" || name == "is_empty"))) continue;
                } else if (i.opcode == IROpcode::CALL_SYSCALL) {
                    const int op = a[1].immediate();
                    if (op == ECALL_ARRAY_SIZE || op == ECALL_ARRAY_AT || op == ECALL_STRING_SIZE || op == ECALL_STRING_AT) continue;
                } else if (i.opcode == IROpcode::GLOBAL_CALL &&
                    (engine_math(static_cast<GlobalFn>(a[1].immediate())) || inline_math(static_cast<GlobalFn>(a[1].immediate())))) continue;
                else if (!ir_has_effect(i.opcode, IR_CALL) && !ir_has_effect(i.opcode, IR_SIDE_EFFECTS)) continue;
                else if (i.opcode == IROpcode::RETURN || i.opcode == IROpcode::JUMP || i.opcode == IROpcode::LABEL ||
                    i.opcode == IROpcode::SCOPE_MARK || i.opcode == IROpcode::SCOPE_RELEASE ||
                    ir_has_effect(i.opcode, IR_BRANCH) || i.opcode == IROpcode::ARRAY_GET) continue;
                borrow_safe[j] = false;
            }
        }
        bool changed;
        do {
            changed = false;
            for (size_t j = 0; j < p.functions.size(); ++j) {
                if (!borrow_safe[j]) continue;
                for (const auto &i : p.functions[j].instructions) {
                    if (i.opcode != IROpcode::CALL && i.opcode != IROpcode::CALL_HOSTED) continue;
                    if (!borrow_safe[functions.at(p.strings[i.operands[0].string_id])]) { borrow_safe[j] = false; changed = true; break; }
                }
            }
        } while (changed);
        for (size_t j = 0; j < p.functions.size() + 2; ++j) {
            if (j == p.functions.size() && !p.has_global_init) continue;
            if (j == p.functions.size()+1 && !p.has_member_init) continue;
            out << "static int f" << j << "(GJContext*,GJVariant*,const GJVariant *const*,int);\n";
            if (j < typed_functions.size() && typed_functions[j]) out << typed_signature(p.functions[j], j) << ";\n";
        }
        for (size_t j = 0; j < p.functions.size(); ++j) function(p.functions[j], j);
        if (p.has_global_init) function(p.global_init, p.functions.size());
        if (p.has_member_init) function(p.member_init, p.functions.size()+1);
        // Native ScriptInstances resolve the function once, then enter its body
        // directly. Keep gj_entry for standalone hosts and initializer dispatch.
        if (!p.functions.empty() || p.has_global_init || p.has_member_init) {
            out << "int (*const gj_functions[])(GJContext*,GJVariant*,const GJVariant *const*,int) = {";
            for (size_t j = 0; j < p.functions.size(); ++j) out << (j ? "," : "") << "f" << j << (typed_functions[j] ? "_v" : "");
            if (p.has_global_init) out << (p.functions.empty() ? "" : ",") << "f" << p.functions.size();
            if (p.has_member_init) out << (p.functions.empty() && !p.has_global_init ? "" : ",") << "f" << p.functions.size() + 1;
            out << "};\n";
        }
        out << "const void *const gj_profile_functions[] = {";
        for (size_t j = 0; j < p.functions.size(); ++j) {
            out << (j ? "," : "") << "(const void*)f" << j;
            if (typed_functions[j]) out << ",(const void*)f" << j << "_t,(const void*)f" << j << "_v";
            else out << ",0,0";
        }
        if (p.functions.empty()) out << "0";
        out << "};\n";
        scalars.clear();
        out << "int gj_entry(GJContext *ctx, int function, GJVariant *result, const GJVariant *const *args, int count) {\n"
            << "  if (ctx->failed) return 0;\n  switch (function) {\n";
        for (size_t j = 0; j < p.functions.size(); ++j) out << "    case " << j << ": return f" << j << "(ctx,result,args,count);\n";
        out << "    case -1: case -2: case -3: { GJVariant temp = {0};\n";
        for (size_t j = 0; j < p.globals.size(); ++j) {
            const auto &g = p.globals[j];
            auto d = global(j);
            out << "if (function != " << (g.is_member() ? -2 : -3) << ") {\n";
            switch (g.init_type) {
            case IRGlobalVar::InitType::INT: out << "gj_int(" << d << ',' << integer(std::get<int64_t>(g.init_value)) << ");\n"; break;
            case IRGlobalVar::InitType::FLOAT: out << "gj_float(" << d << ',' << floating(std::get<double>(g.init_value)) << ");\n"; break;
            case IRGlobalVar::InitType::BOOL: out << "gj_bool(" << d << ',' << std::get<bool>(g.init_value) << ");\n"; break;
            case IRGlobalVar::InitType::STRING: op("GJ_STRING", d, "0", quote(std::get<std::string>(g.init_value)), std::get<std::string>(g.init_value).size()); break;
            case IRGlobalVar::InitType::EMPTY_ARRAY: op("GJ_ARRAY",d); break;
            case IRGlobalVar::InitType::EMPTY_DICT: op("GJ_DICTIONARY",d); break;
            case IRGlobalVar::InitType::NONE: case IRGlobalVar::InitType::NULL_VAL: case IRGlobalVar::InitType::RUNTIME: break;
            }
            out << "}\n";
        }
        if (p.has_global_init) out << "if (function != -3 && !f" << p.functions.size() << "(ctx,&temp,0,0)) goto cleanup;\n";
        if (p.has_member_init) out << "if (function != -2 && !f" << p.functions.size()+1 << "(ctx,&temp,0,0)) goto cleanup;\n";
        out << "cleanup: gj_clear(&temp); return !ctx->failed; }\n"
            << "    default: return gj_fail(ctx,\"Unknown C function index\");\n  }\n}\n";
        out << "void gj_code_end(void) {}\n";
        out << "const int gj_name_count = " << names.size() << ";\nconst char *const gj_names[] = {";
        for (size_t j = 0; j < names.size(); ++j) out << (j ? "," : "") << names[j];
        if (names.empty()) out << "0";
        out << "};\n";
        return out.str();
    }
};
}
std::string CCodeGenerator::generate(const IRProgram &program, bool debug_info) {
    ir_verify(program);
    // Keep named registers and lexical live ranges intact for inspection.
    if (debug_info) return Emitter(program, true).generate();
    auto optimized = program;
    IROptimizer copies;
    // Use only native-safe local copy propagation, not sandbox ownership,
    // batching or register allocation. In particular, retain fallible operations
    // even when their result is unused so native diagnostics remain observable.
    copies.set_enabled_passes({"enhanced-copy-propagation"});
    copies.optimize(optimized);
    auto prune = [](IRFunction &f) {
        bool changed;
        do {
            std::vector<bool> read(std::max(1,f.max_registers),false);
            read[0] = true;
            for (const auto &i : f.instructions)
                for (size_t j = 0; j < i.operands.size(); ++j)
                    if (ir_reads_operand(i,j)) read[i.operands[j].reg_index()] = true;
            const auto size = f.instructions.size();
            f.instructions.erase(std::remove_if(f.instructions.begin(),f.instructions.end(),[&](const auto &i) {
                return i.opcode == IROpcode::MOVE && !read[i.operands[0].reg_index()];
            }),f.instructions.end());
            changed = size != f.instructions.size();
        } while (changed);
    };
    for (auto &f : optimized.functions) prune(f);
    if (optimized.has_global_init) prune(optimized.global_init);
    if (optimized.has_member_init) prune(optimized.member_init);
    bool simplified = false;
    for (auto &f : optimized.functions) {
        if (f.is_coroutine) continue;
        const auto known = proven_locals(f, optimized);
        for (auto &i : f.instructions) {
            if (i.opcode == IROpcode::COERCE && known[i.operands[1].reg_index()] == i.type_hint) {
                i.opcode = IROpcode::MOVE;
                simplified = true;
            }
        }
    }
    if (simplified) {
        copies.optimize(optimized);
        for (auto &f : optimized.functions) prune(f);
    }
    return Emitter(optimized).generate();
}
}
