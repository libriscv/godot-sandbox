#pragma once
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/binder_common.hpp>

using namespace godot;

class Sandbox;

/// One object per coroutine invocation. The caller awaits Signal(state, "completed");
/// the VM rejects GDExtension objects in place of GDScriptFunctionState.
/// The Sandbox holds a reference for the coroutine's lifetime.
class SandboxFunctionState : public RefCounted {
	GDCLASS(SandboxFunctionState, RefCounted);

protected:
	static void _bind_methods();

	String _to_string() const;

public:
	/// True while the coroutine is suspended and resumable.
	bool is_valid() const;

	/// Coroutine id into the Sandbox's table; zero once retired.
	int64_t get_coroutine_id() const { return int64_t(m_coroutine_id); }

	Sandbox *get_sandbox() const { return m_sandbox; }

	void initialize(Sandbox *sandbox, uint64_t coroutine_id) {
		m_sandbox = sandbox;
		m_coroutine_id = coroutine_id;
	}

	/// Detach from the Sandbox; is_valid() returns false afterwards.
	void invalidate() {
		m_sandbox = nullptr;
		m_coroutine_id = 0;
	}

	/// One-shot signal callback. Vararg: 0 → null, 1 → value, N → Array.
	Variant resume_from_signal(const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error);

private:
	Sandbox *m_sandbox = nullptr;
	uint64_t m_coroutine_id = 0;
};
