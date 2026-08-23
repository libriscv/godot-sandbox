// .sgd fault backtraces and breakpoint stops. Line table (every build) maps
// addresses to lines; shadow stack (debug builds) gives call depth. Breaks
// block the host thread inside the syscall; continue = return from it.
#include "../gdscript/compiler/debug_layout.h"
#include "../sandbox.h"
#include "script_language_safegdscript.h"
#include "script_safegdscript.h"
#include <godot_cpp/classes/engine_debugger.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <functional>
#include <map>

// Defined in script_instance_safegdscript.cpp, which owns the map.
SafeGDScript *safegdscript_for_sandbox(const Sandbox *p_sandbox);
void safegdscript_for_each_sandbox(const std::function<void(SafeGDScript &, Sandbox &)> &p_callback);

namespace {

// How often the blocked thread looks at the flag. Short enough that a continue
// from another thread is not noticeably late, long enough not to spin a core.
constexpr int32_t BREAK_POLL_USEC = 1000;

// True when the game is running under a debugger: the editor's, --remote-debug,
// or -d. Every EngineDebugger breakpoint call reports an error when it is not,
// so nothing below may be asked without this.
bool engine_debugger_attached() {
	EngineDebugger *debugger = EngineDebugger::get_singleton();
	return debugger != nullptr && debugger->is_active();
}

template <typename T>
T read_guest(Sandbox &p_sandbox, gaddr_t p_address) {
	T value {};
	p_sandbox.machine().copy_from_guest(&value, p_address, sizeof(T));
	return value;
}

struct Frame {
	uint64_t function_index = 0;
	uint64_t return_address = 0;
	uint64_t frame_sp = 0;
};

// Empty when the program carries no shadow stack, which is every build that was
// not compiled for debugging.
std::vector<Frame> read_shadow_stack(Sandbox &p_sandbox, gaddr_t &r_base) {
	std::vector<Frame> frames;
	r_base = 0;
	if (!p_sandbox.has_program_loaded()) {
		return frames;
	}
	const gaddr_t base = p_sandbox.address_of(gdscript::DEBUG_SYMBOL);
	if (base == 0) {
		return frames;
	}
	if (read_guest<uint32_t>(p_sandbox, base + gdscript::DebugLayout::MAGIC_OFF) != gdscript::DebugLayout::MAGIC ||
			read_guest<uint32_t>(p_sandbox, base + gdscript::DebugLayout::VERSION_OFF) != gdscript::DebugLayout::LAYOUT_VERSION) {
		return frames;
	}
	r_base = base;

	const uint64_t depth = read_guest<uint64_t>(p_sandbox, base + gdscript::DebugLayout::DEPTH_OFF);
	// Frames past MAX_DEPTH unrecorded; unsigned underflow handled here too.
	const uint64_t recorded = depth < gdscript::DebugLayout::MAX_DEPTH
			? depth
			: uint64_t(gdscript::DebugLayout::MAX_DEPTH);
	for (uint64_t i = 0; i < recorded; i++) {
		const gaddr_t frame = base + gdscript::DebugLayout::frame_offset(uint32_t(i));
		Frame entry;
		entry.function_index = read_guest<uint64_t>(p_sandbox, frame + gdscript::DebugLayout::FUNCTION_INDEX_OFF);
		entry.return_address = read_guest<uint64_t>(p_sandbox, frame + gdscript::DebugLayout::RETURN_ADDRESS_OFF);
		entry.frame_sp = read_guest<uint64_t>(p_sandbox, frame + gdscript::DebugLayout::FRAME_SP_OFF);
		frames.push_back(entry);
	}
	return frames;
}

// Resource path, or empty. Runtime scripts need take_over_path() for the editor.
String script_source_path(const SafeGDScript &p_script) {
	const String path = p_script.get_path();
	return path.is_empty() ? static_cast<const Resource &>(p_script).get_path() : path;
}

String function_name(const SafeGDScript &p_script, uint64_t p_index) {
	const std::vector<gdscript::FunctionSignature> &signatures = p_script.get_signatures();
	if (p_index >= signatures.size()) {
		return String("<unknown>");
	}
	const std::string &name = signatures[p_index].name;
	return String::utf8(name.c_str(), name.size());
}

struct StackFrame {
	String source;
	String function;
	uint32_t line = 0;
};

String location(const StackFrame &p_frame) {
	String where = p_frame.source;
	if (p_frame.line != 0) {
		where += String(":") + itos(p_frame.line);
	}
	return where + String(" in ") + p_frame.function + String("()");
}

// Innermost first. Empty for non-.sgd; single PC-derived entry without shadow stack.
std::vector<StackFrame> build_backtrace(SafeGDScript &p_script, Sandbox &p_sandbox,
		gaddr_t p_pc, gaddr_t &r_area) {
	const String path = script_source_path(p_script);
	const String source = path.is_empty() ? String("<built-in>") : path;
	std::vector<StackFrame> stack;
	const gdscript::LineTable &lines = p_script.get_line_table();
	const std::vector<Frame> frames = read_shadow_stack(p_sandbox, r_area);

	if (frames.empty()) {
		// No shadow stack; fall back to ELF symbol name.
		const auto callsite = p_sandbox.machine().memory.lookup(p_pc);
		const String name = callsite.name.empty()
				? String("<unknown>")
				: String::utf8(callsite.name.c_str(), callsite.name.size());
		stack.push_back(StackFrame{ source, name, lines.line_for_address(uint32_t(p_pc)) });
		return stack;
	}

	for (size_t i = frames.size(); i-- > 0;) {
		// Outer frame line from return address - 4 (backs over the jal).
		const uint32_t line = (i + 1 < frames.size())
				? lines.line_for_address(uint32_t(frames[i + 1].return_address - 4))
				: lines.line_for_address(uint32_t(p_pc));
		stack.push_back(StackFrame{ source, function_name(p_script, frames[i].function_index), line });
	}
	return stack;
}

// One program stops at a time; the break blocks the guest's thread.
struct BreakState {
	bool stopped = false;
	bool inside = false; // true while handler is on the stack; suppresses nested breaks
	SafeGDScript *script = nullptr;
	uint32_t line = 0;
	std::vector<StackFrame> frames;
};

// Function-local: engine Strings need the GDExtension interface, unavailable at static init.
BreakState &g_break() {
	static BreakState state;
	return state;
}

} // namespace

bool safegdscript_print_backtrace(Sandbox &p_sandbox, gaddr_t p_pc) {
	SafeGDScript *script = safegdscript_for_sandbox(&p_sandbox);
	if (script == nullptr) {
		return false;
	}
	gaddr_t area = 0;
	const std::vector<StackFrame> stack = build_backtrace(*script, p_sandbox, p_pc, area);
	for (const StackFrame &frame : stack) {
		UtilityFunctions::print("-> ", location(frame));
	}
	if (area == 0) {
		UtilityFunctions::print("   (no call stack: compile with debug info for one)");
	}

	// A fault skips exits; zero the depth so the next call starts clean.
	if (area != 0) {
		const uint64_t zero = 0;
		p_sandbox.machine().copy_to_guest(area + gdscript::DebugLayout::DEPTH_OFF,
				&zero, sizeof(zero));
	}
	return true;
}

// ECALL_BREAKPOINT handler. a0 = reported line; cross-checked against the line
// table. Returning = continue; guest registers are preserved.
void safegdscript_breakpoint(Sandbox &p_sandbox, uint32_t p_reported_line) {
	SafeGDScript *script = safegdscript_for_sandbox(&p_sandbox);
	if (script == nullptr) {
		// Not a .sgd program; ignore.
		return;
	}
	if (g_break().inside) {
		// Nested break from the signal handler; skip to avoid deadlock.
		return;
	}

	const gaddr_t pc = p_sandbox.machine().cpu.pc();
	gaddr_t area = 0;
	const std::vector<StackFrame> stack = build_backtrace(*script, p_sandbox, pc, area);
	const uint32_t table_line = script->get_line_table().line_for_address(uint32_t(pc));

	g_break().inside = true;
	g_break().stopped = true;
	g_break().script = script;
	g_break().line = table_line != 0 ? table_line : p_reported_line;
	g_break().frames = stack;

	if (table_line != 0 && table_line != p_reported_line) {
		ERR_PRINT("SafeGDScript: the line table and the program disagree about the "
				  "breakpoint at line " + itos(p_reported_line) + "; using " + itos(table_line));
	}

	// Project debugger (connected signal) takes priority over the editor.
	const bool listening = script->has_connections("breakpoint_hit");
	script->emit_signal("breakpoint_hit", script, int64_t(g_break().line));

	if (listening) {
		// Block until debug_continue() clears the flag.
		while (g_break().stopped) {
			OS::get_singleton()->delay_usec(BREAK_POLL_USEC);
		}
	} else if (engine_debugger_attached()) {
		// script_debug() blocks until the editor continues.
		if (!EngineDebugger::get_singleton()->is_skipping_breakpoints()) {
			EngineDebugger::get_singleton()->script_debug(
					SafeGDScriptLanguage::get_singleton(), true, false);
		}
		g_break().stopped = false;
	} else {
		// No debugger; report and continue.
		UtilityFunctions::print("Breakpoint at ", g_break().frames.empty()
						? String("line ") + itos(g_break().line)
						: location(g_break().frames.front()),
				" (no debugger attached; continuing)");
		g_break().stopped = false;
	}

	g_break().inside = false;
	g_break().script = nullptr;
}

bool safegdscript_is_stopped() {
	return g_break().stopped;
}

// Null when not stopped. A stopped script must not be rebuilt.
const SafeGDScript *safegdscript_stopped_script() {
	return g_break().stopped ? g_break().script : nullptr;
}

int64_t safegdscript_stopped_line() {
	return g_break().stopped ? int64_t(g_break().line) : -1;
}

PackedStringArray safegdscript_stopped_backtrace() {
	PackedStringArray stack;
	if (g_break().stopped) {
		for (const StackFrame &frame : g_break().frames) {
			stack.push_back(location(frame));
		}
	}
	return stack;
}

void safegdscript_debug_continue() {
	g_break().stopped = false;
}

// -= Editor breakpoint sync =-
//
// No toggle callback for GDExtension; poll the core debugger's set per frame.
// Only editor-changed lines are applied; script-set breakpoints are preserved.

namespace {

constexpr uint32_t SYNC_FRAME_INTERVAL = 6;

// Previous poll state per script; rebuilt each poll so stale entries vanish.
std::map<const SafeGDScript *, PackedInt32Array> &synced_lines() {
	static std::map<const SafeGDScript *, PackedInt32Array> lines;
	return lines;
}

PackedInt32Array editor_breakpoints(const SafeGDScript &p_script) {
	PackedInt32Array lines;
	const String path = script_source_path(p_script);
	if (path.is_empty()) {
		return lines;
	}
	// One StringName conversion for the whole script, not per line.
	const StringName source = path;
	EngineDebugger *debugger = EngineDebugger::get_singleton();
	const int32_t last = int32_t(p_script._get_source_code().count("\n")) + 1;
	for (int32_t line = 1; line <= last; line++) {
		if (debugger->is_breakpoint(line, source)) {
			lines.push_back(line);
		}
	}
	return lines;
}

} // namespace

// Editor breakpoints for the initial build (before the poll loop starts).
PackedInt32Array safegdscript_engine_breakpoints(const SafeGDScript &p_script) {
	if (!engine_debugger_attached()) {
		return PackedInt32Array();
	}
	return editor_breakpoints(p_script);
}

void safegdscript_sync_engine_breakpoints() {
	if (!engine_debugger_attached() || g_break().inside) {
		return;
	}
	static uint32_t countdown = 0;
	if (countdown > 0) {
		countdown--;
		return;
	}
	countdown = SYNC_FRAME_INTERVAL;

	std::map<const SafeGDScript *, PackedInt32Array> polled;
	safegdscript_for_each_sandbox([&polled](SafeGDScript &script, Sandbox &) {
		const PackedInt32Array now = editor_breakpoints(script);
		polled[&script] = now;
		const auto previous = synced_lines().find(&script);
		const bool known = previous != synced_lines().end();
		if (known && previous->second == now) {
			return;
		}
		PackedInt32Array wanted = script.get_breakpoints();
		if (known) {
			// Remove lines the editor cleared; leave script-set lines alone.
			for (int i = 0; i < previous->second.size(); i++) {
				const int32_t line = previous->second[i];
				const int64_t at = now.has(line) ? -1 : wanted.find(line);
				if (at >= 0) {
					wanted.remove_at(at);
				}
			}
		}
		for (int i = 0; i < now.size(); i++) {
			if (!wanted.has(now[i])) {
				wanted.push_back(now[i]);
			}
		}
		script.set_breakpoints(wanted);
	});
	synced_lines().swap(polled);
}

// -= Editor debugger virtuals (innermost level first) =-

String SafeGDScriptLanguage::_debug_get_error() const {
	return String();
}

int32_t SafeGDScriptLanguage::_debug_get_stack_level_count() const {
	return int32_t(g_break().frames.size());
}

int32_t SafeGDScriptLanguage::_debug_get_stack_level_line(int32_t p_level) const {
	const std::vector<StackFrame> &frames = g_break().frames;
	if (p_level < 0 || size_t(p_level) >= frames.size()) {
		return 0;
	}
	return int32_t(frames[p_level].line);
}

String SafeGDScriptLanguage::_debug_get_stack_level_function(int32_t p_level) const {
	const std::vector<StackFrame> &frames = g_break().frames;
	if (p_level < 0 || size_t(p_level) >= frames.size()) {
		return String();
	}
	return frames[p_level].function;
}

String SafeGDScriptLanguage::_debug_get_stack_level_source(int32_t p_level) const {
	const std::vector<StackFrame> &frames = g_break().frames;
	if (p_level < 0 || size_t(p_level) >= frames.size()) {
		return String();
	}
	return frames[p_level].source;
}

TypedArray<Dictionary> SafeGDScriptLanguage::_debug_get_current_stack_info() {
	TypedArray<Dictionary> info;
	for (const StackFrame &frame : g_break().frames) {
		Dictionary level;
		level["file"] = frame.source;
		level["line"] = int32_t(frame.line);
		level["func"] = frame.function;
		info.push_back(level);
	}
	return info;
}

// No variable table; values live in guest-stack Variants without name info.
Dictionary SafeGDScriptLanguage::_debug_get_stack_level_locals(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}

Dictionary SafeGDScriptLanguage::_debug_get_stack_level_members(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}

void *SafeGDScriptLanguage::_debug_get_stack_level_instance(int32_t p_level) {
	return nullptr;
}

Dictionary SafeGDScriptLanguage::_debug_get_globals(int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}

String SafeGDScriptLanguage::_debug_parse_stack_level_expression(int32_t p_level, const String &p_expression, int32_t p_max_subitems, int32_t p_max_depth) {
	return String();
}
