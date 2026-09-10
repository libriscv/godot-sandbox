#include "c_codegen.h"
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
// Flow-insensitive union of every definition, including loop backedges. Only
// singleton scalar sets are promoted; entry arguments remain unknown. The IR
// verifier establishes definite assignment before any read. Do not trust hints
// on dynamic arithmetic or globals, which can change across reentrant calls.
std::vector<int> scalar_locals(const IRFunction &f) {
    constexpr unsigned unknown = ~0u;
    std::vector<unsigned> types(std::max(1, f.max_registers), 0);
    for (size_t j = 0; j < f.parameters.size(); ++j) types[j] = unknown;
    auto propagate = [&]() {
        bool changed = false;
        for (const auto &i : f.instructions) {
            int d = ir_destination_register(i);
            if (d < 0) continue;
            auto src = [&](int n) { return types[i.operands[n].reg_index()]; };
            unsigned t = unknown;
            switch (i.opcode) {
            case IROpcode::LOAD_IMM: case IROpcode::TYPE_OF: t = 4; break;
            case IROpcode::LOAD_FLOAT_IMM: t = 8; break;
            case IROpcode::LOAD_BOOL: case IROpcode::TYPE_TEST: case IROpcode::TYPE_TEST_MASK:
            case IROpcode::CMP_EQ: case IROpcode::CMP_NEQ: case IROpcode::CMP_LT:
            case IROpcode::CMP_LTE: case IROpcode::CMP_GT: case IROpcode::CMP_GTE:
            case IROpcode::AND: case IROpcode::OR: case IROpcode::NOT: t = 2; break;
            case IROpcode::LOAD_NIL: t = 1; break;
            case IROpcode::CONSTRUCT:
                t = i.operands[1].immediate() >= 1 && i.operands[1].immediate() <= 3 ? 1u << i.operands[1].immediate() : unknown; break;
            case IROpcode::MOVE: t = src(1); break;
            case IROpcode::GLOBAL_CALL:
                if (static_cast<GlobalFn>(i.operands[1].immediate()) == GlobalFn::TO_FLOAT ||
                    static_cast<GlobalFn>(i.operands[1].immediate()) == GlobalFn::FLOAT_IDENTITY) t = 8;
                if (static_cast<GlobalFn>(i.operands[1].immediate()) == GlobalFn::TO_INT) t = 4;
                break;
            case IROpcode::CONVERT: case IROpcode::COERCE:
                t = i.type_hint >= 1 && i.type_hint <= 3 ? 1u << i.type_hint : unknown; break;
            case IROpcode::ADD: case IROpcode::SUB: case IROpcode::MUL: case IROpcode::DIV:
                if (!src(1) || !src(2)) t = 0;
                else if (!((src(1) | src(2)) & ~12u))
                    t = ((src(1) & src(2) & 4) ? 4 : 0) | (((src(1) | src(2)) & 8) ? 8 : 0);
                break;
            case IROpcode::MOD: case IROpcode::BIT_AND: case IROpcode::BIT_OR:
            case IROpcode::BIT_XOR: case IROpcode::SHL: case IROpcode::SHR:
                t = !src(1) || !src(2) ? 0 : src(1) == 4 && src(2) == 4 ? 4 : unknown; break;
            case IROpcode::NEG: case IROpcode::BIT_NOT:
                t = src(1) == 4 ? 4 : src(1) == 0 ? 0 : unknown; break;
            default: break;
            }
            unsigned merged = types[d] | t;
            if (merged != types[d]) { types[d] = merged; changed = true; }
        }
        return changed;
    };
    while (propagate()) {}
    // Unseeded cycles/unreachable definitions are not evidence of a type.
    for (auto &t : types) if (!t) t = unknown;
    while (propagate()) {}
    std::vector<int> result(types.size(), -1);
    for (size_t j = 0; j < types.size(); ++j)
        for (int t = 1; t <= 3; ++t) if (types[j] == (1u << t)) result[j] = t;
    return result;
}
struct Emitter {
    const IRProgram &p;
    std::ostringstream out;
    std::unordered_map<std::string, size_t> functions;
    std::vector<int> scalars;
    std::vector<bool> boxes;
    bool debug = false;
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
        if (address.size() > 2 && address[0] == '&' && scalar(address.substr(1)) >= 0) {
            auto r = address.substr(1); int t = scalar(r);
            out << "  " << r << (t == 1 ? ".data.b" : t == 2 ? ".data.i" : ".data.f") << " = " << payload(r,t) << ";\n";
        }
    }
    void unbox(const std::string &address) {
        require_box(address);
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
        else { require_box("&" + d); require_box("&" + s); out << "  gj_move(&" << d << ",&" << s << ");\n"; unbox("&" + d); }
    }
    std::string r(const IRValue &v) { return "r" + std::to_string(v.reg_index()); }
    std::string label(const IRValue &v) { return "L" + std::to_string(v.string_id); }
    std::string constant(const IRValue &v) { return quote(p.string_constants.at(v.immediate())); }
    std::string name(const IRValue &v) { return quote(p.strings[v.string_id]); }
    void op(const std::string &kind, const std::string &dst, const std::string &self = "0",
            const std::string &n = "0", int detail = -1, const std::vector<std::string> &args = {}) {
        box(self);
        for (const auto &a : args) box(a);
        out << "  { const GJVariant *a[" << std::max<size_t>(1, args.size()) << "] = {";
        for (size_t i = 0; i < args.size(); ++i) out << (i ? "," : "") << args[i];
        if (args.empty()) out << "0";
        out << "}; if (!gj_op(ctx," << kind << ',' << dst << ',' << self << ',' << n << ','
            << detail << ",a," << args.size() << ")) goto cleanup; }\n";
        unbox(dst);
    }
    void method(const IRInstruction &i) {
        const auto &a = i.operands;
        const auto d = r(a[0]), x = r(a[1]);
        const auto &method_name = p.strings[a[2].string_id];
        const auto av = args(i, 4);
        if (!i.super_call && method_name == "distance_squared_to" && av.size() == 1) {
            const auto y = r(a[4]);
            box("&" + x); box("&" + y);
            out << "  if (" << x << ".type == 5 && " << y << ".type == 5) {\n"
                << "    GJReal dx = " << x << ".data.real[0] - " << y << ".data.real[0];\n"
                << "    GJReal dy = " << x << ".data.real[1] - " << y << ".data.real[1];\n";
            // Round in engine real precision before boxing as a Variant double.
            set(d, 3, "(GJReal)(dx * dx + dy * dy)");
            out << "  } else {\n";
            op("GJ_CALL", "&" + d, "&" + x, name(a[2]), -1, av);
            out << "  }\n";
        } else {
            const char *kind = i.super_call ? "GJ_SUPER_CALL" :
                method_name == "size" && av.empty() ? "GJ_ARRAY_SIZE" :
                method_name == "normalized" && av.empty() ? "GJ_VECTOR2_NORMALIZED" : "GJ_CALL";
            op(kind, "&" + d, "&" + x, name(a[2]), -1, av);
        }
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
    bool array_step(const IRFunction &f, size_t at, const std::vector<int> &reads,
                    const std::vector<int> &writes) {
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
        require_box(item); box(container);
        out << "  { int step = gj_array_next(ctx," << item << ',' << container << ','
            << payload(r(cmp.operands[1]), 2) << ");\n"
            << "    if (step < 0) goto cleanup;\n"
            << "    if (!step) goto " << label(branch.operands[1]) << "; }\n";
        unbox(item);
        return true;
    }
    void function(const IRFunction &f, size_t index) {
        scalars = scalar_locals(f);
        // Every coroutine local must survive suspension as an owned Variant.
        if (debug || f.is_coroutine) std::fill(scalars.begin(), scalars.end(), -1);
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
        out << "  GJVariant temp = {0};\n";
        if (f.is_coroutine) {
            out << "  if (resuming) { ctx->resuming = 0; switch (gj_await_restore(ctx,resuming,slots," << registers << ")) {\n";
            for (size_t at = 0; at < f.instructions.size(); ++at)
                if (f.instructions[at].opcode == IROpcode::AWAIT)
                    out << "    case " << at << ": goto resume" << at << ";\n";
            out << "    default: gj_fail(ctx,\"Invalid coroutine resume state\"); goto cleanup;\n  }}\n";
        }
        out << "  if (count != " << f.parameters.size() << ") {"; fail("Wrong argument count for " + f.name); out << "  }\n";
        // Unreferenced parameters need no local ownership (notably unused
        // container arguments). Arity and runtime signature checks still apply.
        for (size_t j = 0; j < f.parameters.size(); ++j)
            if (used[j]) { require_box("&r" + std::to_string(j)); out << "  gj_move(&r" << j << ",args[" << j << "]);\n"; }
        for (size_t at = 0; at < f.instructions.size(); ++at) {
            if (!debug && array_step(f, at, reads, writes)) { at += 3; continue; }
            const auto &i = f.instructions[at];
            const auto &a = i.operands;
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
            case IROpcode::MOVE: move(r(a[0]),r(a[1])); break;
            case IROpcode::LOAD_GLOBAL: out << "  gj_move(" << reg(0) << "," << global(a[1].immediate()) << ");\n"; break;
            case IROpcode::STORE_GLOBAL: box(reg(1)); out << "  gj_move(" << global(a[0].immediate()) << "," << reg(1) << ");\n"; break;
            case IROpcode::CONVERT: case IROpcode::COERCE:
                if (scalar(r(a[1])) >= 0 && scalar(r(a[1])) == i.type_hint) move(r(a[0]),r(a[1]));
                else if (scalar(r(a[1])) >= 0 && i.type_hint == 3)
                    set(r(a[0]),3,payload(r(a[1]),scalar(r(a[1]))));
                else {
                    box(reg(1));
                    out << "  if (" << r(a[1]) << ".type == " << i.type_hint << ") {\n";
                    if (i.type_hint >= 1 && i.type_hint <= 3)
                        set(r(a[0]),i.type_hint,payload(r(a[1]),i.type_hint));
                    else out << "  gj_move(" << reg(0) << ',' << reg(1) << ");\n";
                    out << "  } else {\n";
                    op("GJ_CONSTRUCT", reg(0), "0", "0", i.type_hint, {reg(1)});
                    out << "  }\n";
                }
                break;
            case IROpcode::TYPE_OF: reg(1); out << "  gj_int(" << reg(0) << ',' << r(a[1]) << ".type);\n"; break;
            case IROpcode::TYPE_TEST: reg(1); out << "  gj_bool(" << reg(0) << ',' << r(a[1]) << ".type == " << a[2].immediate() << ");\n"; break;
            case IROpcode::TYPE_TEST_MASK: reg(1); out << "  gj_bool(" << reg(0) << ",(" << integer(a[2].immediate()) << " & (1ULL << " << r(a[1]) << ".type)) != 0);\n"; break;
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
                box(reg(1)); box(reg(2));
                out << "  gj_bool(" << reg(0) << ",gj_boolean(" << reg(1) << ')' << (i.opcode == IROpcode::AND ? " && " : " || ") << "gj_boolean(" << reg(2) << "));\n"; break;
            case IROpcode::NOT: box(reg(1)); out << "  gj_bool(" << reg(0) << ",!gj_boolean(" << reg(1) << "));\n"; break;
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
                auto av = args(i, 3);
                for (const auto &arg : av) box(arg);
                out << "  { const GJVariant *a[" << std::max<size_t>(1,av.size()) << "] = {";
                for (size_t j = 0; j < av.size(); ++j) out << (j ? "," : "") << av[j];
                if (av.empty()) out << '0';
                out << "}; if (!f" << it->second << "(ctx," << reg(1) << ",a," << av.size() << ")) goto cleanup; }\n"; break;
            }
            case IROpcode::RETURN: out << "  goto cleanup;\n"; break;
            case IROpcode::VCALL: method(i); break;
            case IROpcode::VGET: op("GJ_GET_NAMED", reg(0), reg(1), constant(a[2])); break;
            case IROpcode::VSET: op("GJ_SET_NAMED", "&temp", reg(0), constant(a[1]), -1, {reg(3)}); break;
            case IROpcode::VGET_INLINE: op("GJ_GET_NAMED", reg(0), reg(1), name(a[2])); break;
            case IROpcode::VSET_INLINE: op("GJ_SET_NAMED", "&temp", reg(0), name(a[1]), -1, {reg(3)}); break;
            case IROpcode::ARRAY_GET: op("GJ_GET", reg(0), reg(1), "0", -1, {reg(2)}); break;
            case IROpcode::VARIANT_SET: case IROpcode::ARRAY_SET: case IROpcode::DICT_SET:
                op("GJ_SET", "&temp", reg(0), "0", -1, {reg(1), reg(2)}); break;
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
                op("GJ_CONSTRUCT", reg(0), "0", "0", types[int(i.opcode)-int(IROpcode::MAKE_VECTOR2)], args(i,1)); break;
            }
            case IROpcode::GLOBAL_CALL:
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
            case IROpcode::TYPE_OF: case IROpcode::TYPE_TEST: case IROpcode::TYPE_TEST_MASK:
            case IROpcode::AND: case IROpcode::OR: case IROpcode::NOT:
            case IROpcode::NEG: case IROpcode::BIT_NOT:
                unbox(reg(0)); break;
            default: break;
            }
        }
        out << "cleanup:\n";
        if (debug) out << "  if (ctx->debug) ctx->debug(ctx,&debug_frame,GJ_DEBUG_EXIT);\n";
        box("&r0");
        out << "  if (!ctx->failed" << (f.is_coroutine ? " && !suspended" : "") << ") gj_move(result,&r0);\n";
        for (int j = 0; j < registers; ++j) if (used[j] && scalars[j] < 0) { boxes[j] = true; out << "  gj_clear(&r" << j << ");\n"; }
        out << "  gj_clear(&temp);\n  return !ctx->failed;\n}\n";
        auto body = out.str();
        out = std::move(prefix);
        out << "static int f" << index << "(GJContext *ctx, GJVariant *result, const GJVariant *const *args, int count) {\n";
        for (int j = 0; j < registers; ++j) {
            if (boxes[j])
                out << "  GJVariant r" << j << " = {" << std::max(0,scalars[j]) << ",0,{0}};\n";
            if (scalars[j] >= 0)
                out << "  " << (scalars[j] == 3 ? "double" : "GJInt") << " s" << j << " = 0; (void)s" << j << ";\n";
        }
        out << body;
    }
    std::string generate() {
        ir_verify(p);
        out << "/* Generated native C99. Compile with c_abi.h on the include path. */\n#ifndef GODOT_JIT_C_ABI_H\n#include \"c_abi.h\"\n#endif\n";
        for (size_t j = 0; j < p.functions.size(); ++j) functions.emplace(p.functions[j].name, j);
        for (size_t j = 0; j < p.functions.size() + 2; ++j) {
            if (j == p.functions.size() && !p.has_global_init) continue;
            if (j == p.functions.size()+1 && !p.has_member_init) continue;
            out << "static int f" << j << "(GJContext*,GJVariant*,const GJVariant *const*,int);\n";
        }
        for (size_t j = 0; j < p.functions.size(); ++j) function(p.functions[j], j);
        if (p.has_global_init) function(p.global_init, p.functions.size());
        if (p.has_member_init) function(p.member_init, p.functions.size()+1);
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
        return out.str();
    }
};
}
std::string CCodeGenerator::generate(const IRProgram &program, bool debug_info) {
    ir_verify(program);
    // Keep named registers and lexical live ranges intact for inspection.
    if (debug_info) return Emitter{program, {}, {}, {}, {}, true}.generate();
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
    return Emitter{optimized, {}, {}, {}, {}}.generate();
}
}
