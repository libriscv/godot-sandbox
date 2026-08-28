// .sgd fault backtraces and breakpoint stops. Line table (every build) maps
// addresses to lines; shadow stack (debug builds) gives call depth. Breaks
// block the host thread inside the syscall; continue = return from it.
#include "../gdscript/compiler/debug_layout.h"
#include "../sandbox.h"
#include "../guest_datatypes.h"
#include "script_instance_safegdscript.h"
#include "script_language_safegdscript.h"
#include "script_safegdscript.h"
#include <godot_cpp/classes/engine_debugger.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <functional>
#include <algorithm>
#include <map>
#include <vector>
#include <climits>

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
	uint64_t instance_base = 0;
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
		entry.instance_base = read_guest<uint64_t>(p_sandbox, frame + gdscript::DebugLayout::INSTANCE_BASE_OFF);
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
	uint64_t function_index = 0;
	uint64_t frame_sp = 0;
	uint64_t instance_base = 0;
	uint64_t pc = 0;
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
		stack.push_back(StackFrame{ source, name, lines.line_for_address(uint32_t(p_pc)), 0, 0, 0, uint64_t(p_pc) });
		return stack;
	}

	for (size_t i = frames.size(); i-- > 0;) {
		// Outer frame line from return address - 4 (backs over the jal).
		const uint32_t line = (i + 1 < frames.size())
				? lines.line_for_address(uint32_t(frames[i + 1].return_address - 4))
				: lines.line_for_address(uint32_t(p_pc));
		const uint64_t frame_pc = (i + 1 < frames.size()) ? frames[i + 1].return_address - 4 : p_pc;
		stack.push_back(StackFrame{ source, function_name(p_script, frames[i].function_index), line,
				frames[i].function_index, frames[i].frame_sp, frames[i].instance_base, frame_pc });
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
	Sandbox *sandbox = nullptr;
	String error;
	// Godot expresses stepping as lines_left plus a relative call depth. Keep
	// the last shadow-stack observation so calls and returns can update that
	// depth exactly like the interpreted GDScript VM does.
	bool step_active = false;
	SafeGDScript *poll_script = nullptr;
	int32_t poll_depth = 0;
	std::vector<SafeGDScript *> step_scripts;
	SafeGDScript *last_stop_script = nullptr;
	uint32_t last_stop_function = UINT32_MAX;
	uint32_t last_stop_line = 0;
	int32_t last_stop_depth = 0;
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
	g_break().script = script;
	g_break().sandbox = &p_sandbox;
	g_break().frames = stack;
	g_break().line = stack.empty() ? 0 : stack.front().line;
	g_break().error = "Guest runtime fault";
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

void safegdscript_report_runtime_error(Sandbox &p_sandbox, const String &p_message) {
	if (g_break().sandbox != &p_sandbox || g_break().script == nullptr) {
		return;
	}
	g_break().error = p_message;
	if (engine_debugger_attached()) {
		EngineDebugger::get_singleton()->script_debug(
				SafeGDScriptLanguage::get_singleton(), false, true);
	}
	g_break().sandbox = nullptr;
}

String safegdscript_source_location(Sandbox &p_sandbox, gaddr_t p_pc) {
	SafeGDScript *script = safegdscript_for_sandbox(&p_sandbox);
	if (script == nullptr) {
		return String();
	}
	const uint32_t line = script->get_line_table().line_for_address(uint32_t(p_pc));
	const String path = script_source_path(*script);
	if (line == 0 || path.is_empty()) {
		return String();
	}
	return path + String(":") + itos(line) + String(": ");
}

// ECALL_BREAKPOINT handler. a0 = reported line; cross-checked against the line
// table. Returning = continue; guest registers are preserved.
void safegdscript_breakpoint(Sandbox &p_sandbox, uint32_t p_reported_line, bool p_user_stop,
		bool p_source_stop) {
	SafeGDScript *script = safegdscript_for_sandbox(&p_sandbox);
	if (script == nullptr) {
		// Not a .sgd program; ignore.
		return;
	}
	if (g_break().inside) {
		// Nested break from the signal handler; skip to avoid deadlock.
		return;
	}
	EngineDebugger *engine_debugger = EngineDebugger::get_singleton();
	if (!p_source_stop) {
		// Ours, plus whatever the editor holds for this line right now. Asking per
		// line keeps a cleared breakpoint from stopping once more; asking for the
		// whole set here would cost the engine one query per line of source.
		p_user_stop = script->get_project_breakpoints().has(int32_t(p_reported_line));
		if (!p_user_stop && engine_debugger != nullptr && engine_debugger->is_active()) {
			const String path = script_source_path(*script);
			p_user_stop = !path.is_empty() && engine_debugger->is_breakpoint(
					int32_t(p_reported_line), StringName(path));
		}
	}
	const gaddr_t pc = p_sandbox.machine().cpu.pc();
	gaddr_t area = 0;
	const std::vector<StackFrame> stack = build_backtrace(*script, p_sandbox, pc, area);
	const uint32_t table_line = script->get_line_table().line_for_address(uint32_t(pc));
	const int32_t current_depth = int32_t(stack.size());
	const uint32_t current_function = stack.empty() ? UINT32_MAX : stack.front().function_index;
	const uint32_t current_line = table_line != 0 ? table_line : p_reported_line;

	if (!p_user_stop) {
		if (engine_debugger == nullptr || !engine_debugger->is_active() ||
				engine_debugger->get_lines_left() <= 0) {
			g_break().step_active = false;
			g_break().poll_script = script;
			g_break().poll_depth = current_depth;
			return;
		}

		if (!g_break().step_active) {
			g_break().step_active = true;
			g_break().step_scripts.clear();
			if (g_break().poll_script != nullptr) {
				g_break().step_scripts.push_back(g_break().poll_script);
			}
		}

		int32_t depth_delta = 0;
		if (g_break().poll_script == script) {
			depth_delta = current_depth - g_break().poll_depth;
		} else {
			auto found = std::find(g_break().step_scripts.begin(),
					g_break().step_scripts.end(), script);
			if (found == g_break().step_scripts.end()) {
				g_break().step_scripts.push_back(script);
				depth_delta = 1;
			} else {
				depth_delta = -int32_t(g_break().step_scripts.end() - found - 1);
				g_break().step_scripts.erase(found + 1, g_break().step_scripts.end());
			}
		}
		g_break().poll_script = script;
		g_break().poll_depth = current_depth;

		if (engine_debugger->get_depth() >= 0 && depth_delta != 0) {
			engine_debugger->set_depth(engine_debugger->get_depth() + depth_delta);
		}
		const bool same_target = g_break().last_stop_script == script &&
				g_break().last_stop_function == current_function &&
				g_break().last_stop_line == current_line &&
				g_break().last_stop_depth == current_depth;
		if (engine_debugger->get_depth() > 0 || same_target) {
			return;
		}
		engine_debugger->set_lines_left(engine_debugger->get_lines_left() - 1);
		if (engine_debugger->get_lines_left() > 0) {
			return;
		}
	}

	g_break().inside = true;
	g_break().stopped = true;
	g_break().script = script;
	g_break().line = current_line;
	g_break().frames = stack;
	g_break().sandbox = &p_sandbox;
	g_break().error = String();
	g_break().poll_script = script;
	g_break().poll_depth = current_depth;
	g_break().last_stop_script = script;
	g_break().last_stop_function = current_function;
	g_break().last_stop_line = current_line;
	g_break().last_stop_depth = current_depth;

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
		if (!p_user_stop || !EngineDebugger::get_singleton()->is_skipping_breakpoints()) {
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
	g_break().sandbox = nullptr;
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
	std::vector<SafeGDScript *> changed;
	safegdscript_for_each_sandbox([&polled, &changed](SafeGDScript &script, Sandbox &) {
		const PackedInt32Array now = editor_breakpoints(script);
		polled[&script] = now;
		const auto previous = synced_lines().find(&script);
		const bool known = previous != synced_lines().end();
		if (known && previous->second == now) {
			return;
		}
		changed.push_back(&script);
	});
	synced_lines().swap(polled);
	// Outside the walk: an update may rebuild the program, and a rebuild is free
	// to add or drop the sandbox registrations the walk iterates.
	for (SafeGDScript *script : changed) {
		// Script-owned lines plus the editor's as they stand now; a line the
		// editor cleared is already gone from it.
		script->update_runtime_breakpoints(script->get_breakpoints());
	}
}

// -= Editor debugger virtuals (innermost level first) =-

String SafeGDScriptLanguage::_debug_get_error() const {
	return g_break().error;
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

static bool debug_guest_variant(uint64_t p_address, Variant &r_value) {
	if (g_break().sandbox == nullptr || p_address == 0) {
		return false;
	}
	try {
		const GuestVariant value = read_guest<GuestVariant>(*g_break().sandbox, p_address);
		if (value.type >= Variant::VARIANT_MAX) {
			return false;
		}
		r_value = value.toVariant(*g_break().sandbox);
		return true;
	} catch (const std::exception &) {
		return false;
	}
}

static Variant bounded_debug_value(const Variant &p_value, int32_t p_max_subitems,
		int32_t p_max_depth, int32_t p_depth = 0) {
	if (p_max_depth >= 0 && p_depth >= p_max_depth) {
		return String("<max depth>");
	}
	const int32_t limit = p_max_subitems < 0 ? INT32_MAX : p_max_subitems;
	if (p_value.get_type() == Variant::ARRAY) {
		const Array source = p_value;
		Array result;
		for (int32_t i = 0; i < source.size() && i < limit; i++) {
			result.push_back(bounded_debug_value(source[i], limit, p_max_depth, p_depth + 1));
		}
		return result;
	}
	if (p_value.get_type() == Variant::DICTIONARY) {
		const Dictionary source = p_value;
		Dictionary result;
		const Array keys = source.keys();
		for (int32_t i = 0; i < keys.size() && i < limit; i++) {
			result[keys[i]] = bounded_debug_value(source[keys[i]], limit, p_max_depth, p_depth + 1);
		}
		return result;
	}
	return p_value;
}

static bool debug_record_live(const gdscript::DebugVariableRecord &p_record,
		const StackFrame &p_frame) {
	return (p_record.function_index == UINT32_MAX ||
			p_record.function_index == p_frame.function_index) &&
			p_frame.pc >= p_record.pc_begin && p_frame.pc <= p_record.pc_end;
}

Dictionary SafeGDScriptLanguage::_debug_get_stack_level_locals(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	Dictionary values;
	if (p_level < 0 || size_t(p_level) >= g_break().frames.size() || g_break().script == nullptr) {
		return values;
	}
	const StackFrame &frame = g_break().frames[size_t(p_level)];
	if (frame.frame_sp == 0) {
		return values;
	}
	for (const gdscript::DebugVariableRecord &record : g_break().script->get_debug_variables()) {
		if (record.storage != gdscript::DebugStorage::FRAME || !debug_record_live(record, frame)) continue;
		Variant value;
		if (debug_guest_variant(frame.frame_sp + uint64_t(record.offset), value)) {
			values[String::utf8(record.name.c_str(), record.name.size())] =
					bounded_debug_value(value, p_max_subitems, p_max_depth);
		}
	}
	if (g_break().script->get_debug_variables().empty()) {
		const auto &signatures = g_break().script->get_signatures();
		if (frame.function_index < signatures.size()) {
			const auto &signature = signatures[size_t(frame.function_index)];
			for (size_t i = 0; i < signature.parameters.size(); i++) {
				Variant value;
				if (debug_guest_variant(frame.frame_sp + 16 + i * sizeof(GuestVariant), value)) {
					values[String::utf8(signature.parameters[i].name.c_str(), signature.parameters[i].name.size())] =
							bounded_debug_value(value, p_max_subitems, p_max_depth);
				}
			}
		}
	}
	return values;
}

Dictionary SafeGDScriptLanguage::_debug_get_stack_level_members(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	Dictionary values;
	if (p_level < 0 || size_t(p_level) >= g_break().frames.size() || g_break().script == nullptr) {
		return values;
	}
	const StackFrame &frame = g_break().frames[size_t(p_level)];
	for (const gdscript::DebugVariableRecord &record : g_break().script->get_debug_variables()) {
		if (record.storage != gdscript::DebugStorage::MEMBER || !debug_record_live(record, frame)) continue;
		Variant value;
		if (debug_guest_variant(frame.instance_base + uint64_t(record.offset), value)) {
			values[String::utf8(record.name.c_str(), record.name.size())] =
					bounded_debug_value(value, p_max_subitems, p_max_depth);
		}
	}
	if (g_break().script->get_debug_variables().empty()) {
		uint64_t member_index = 0;
		for (const gdscript::PropertySignature &property : g_break().script->get_property_signatures()) {
			if (!property.is_member) continue;
			Variant value;
			if (debug_guest_variant(frame.instance_base + member_index * sizeof(GuestVariant), value)) {
				values[String::utf8(property.name.c_str(), property.name.size())] =
						bounded_debug_value(value, p_max_subitems, p_max_depth);
			}
			member_index++;
		}
	}
	return values;
}

void *SafeGDScriptLanguage::_debug_get_stack_level_instance(int32_t p_level) {
	if (g_break().script == nullptr) return nullptr;
	if (p_level < 0 || size_t(p_level) >= g_break().frames.size()) return nullptr;
	return g_break().script->owner_for_instance_base(g_break().frames[size_t(p_level)].instance_base);
}

Dictionary SafeGDScriptLanguage::_debug_get_globals(int32_t p_max_subitems, int32_t p_max_depth) {
	Dictionary values;
	if (g_break().script == nullptr || g_break().sandbox == nullptr || g_break().frames.empty()) return values;
	const uint64_t base = g_break().sandbox->address_of(gdscript::DEBUG_GLOBALS_SYMBOL);
	if (base == 0) return values;
	const StackFrame &frame = g_break().frames.front();
	for (const gdscript::DebugVariableRecord &record : g_break().script->get_debug_variables()) {
		if (record.storage != gdscript::DebugStorage::GLOBAL || !debug_record_live(record, frame)) continue;
		Variant value;
		if (debug_guest_variant(base + uint64_t(record.offset), value)) {
			values[String::utf8(record.name.c_str(), record.name.size())] =
					bounded_debug_value(value, p_max_subitems, p_max_depth);
		}
	}
	return values;
}

String SafeGDScriptLanguage::_debug_parse_stack_level_expression(int32_t p_level, const String &p_expression, int32_t p_max_subitems, int32_t p_max_depth) {
	const String expression = p_expression.strip_edges();
	if (expression.is_empty()) return "Unsupported empty expression";
	Dictionary locals = _debug_get_stack_level_locals(p_level, p_max_subitems, p_max_depth);
	Dictionary members = _debug_get_stack_level_members(p_level, p_max_subitems, p_max_depth);
	Dictionary globals = _debug_get_globals(p_max_subitems, p_max_depth);
	const String name = expression.get_slice(".", 0);
	Variant value;
	if (name == "self") {
		value = static_cast<Object *>(_debug_get_stack_level_instance(p_level));
	} else {
		value = locals.get(name, members.get(name, globals.get(name, Variant())));
	}
	if (expression.contains(".")) {
		const String member = expression.get_slice(".", 1);
		if (value.get_type() == Variant::DICTIONARY) value = Dictionary(value).get(member, Variant());
		else if (value.get_type() == Variant::OBJECT && value.operator Object *() != nullptr) value = value.operator Object *()->get(member);
		else return "Unsupported member expression";
	}
	if (expression.get_slice_count(".") > 2) return "Unsupported expression";
	return UtilityFunctions::var_to_str(value);
}
