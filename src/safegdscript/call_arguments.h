#pragma once

#include "../variant_coerce.h"
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/local_vector.hpp>

using namespace godot;

// The Sandbox ABI passes one Variant pointer per argument and no count, so an
// argument the caller left out is a null pointer the guest dereferences as soon
// as it reads the parameter. Nothing downstream can recover from that, and Godot
// cannot check the arity itself: through a statically-typed Node it has no idea
// a .sgd script is on the other side. So the call is completed, or refused, here.
//
// Defaults are the same problem seen from the callee: it cannot fill one in
// either, having no way to tell whether it was given the argument. The compiler
// hands the constant ones over with the signature, and they are appended here
// exactly as Godot appends its own.
class CompletedArguments {
public:
	// False means the call is refused; r_error says why. A null or vararg
	// signature is nothing known about the arguments, so nothing is checked.
	bool complete(const MethodInfo *p_method, const Variant **p_args, int p_argcount,
			GDExtensionCallError &r_error) {
		m_args = p_args;
		m_argcount = p_argcount;
		if (p_method == nullptr || (p_method->flags & METHOD_FLAG_VARARG)) {
			return true;
		}

		const int expected = int(p_method->arguments.size());
		const int required = expected - int(p_method->default_arguments.size());
		if (p_argcount < required) {
			r_error.error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
			r_error.argument = p_argcount;
			r_error.expected = required;
			return false;
		}
		if (p_argcount > expected) {
			r_error.error = GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS;
			r_error.argument = p_argcount;
			r_error.expected = expected;
			return false;
		}

		for (int i = 0; i < p_argcount; i++) {
			const Variant::Type declared = p_method->arguments[i].type;
			if (declared == Variant::NIL || p_args[i]->get_type() == declared) {
				continue;
			}
			if (m_completed.is_empty()) {
				m_narrowed.resize(p_argcount);
				m_completed.reserve(expected);
				for (int j = 0; j < p_argcount; j++) {
					m_narrowed[j] = *p_args[j];
					m_completed.push_back(&m_narrowed[j]);
				}
			}
			if (!coerce_variant_to(m_narrowed[i], declared)) {
				r_error.error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
				r_error.argument = i;
				r_error.expected = declared;
				return false;
			}
		}

		if (p_argcount < expected) {
			if (m_completed.is_empty()) {
				m_completed.reserve(expected);
				for (int i = 0; i < p_argcount; i++) {
					m_completed.push_back(p_args[i]);
				}
			}
			const int first_default = int(p_method->default_arguments.size()) - (expected - p_argcount);
			for (int i = first_default; i < int(p_method->default_arguments.size()); i++) {
				m_completed.push_back(&p_method->default_arguments[i]);
			}
		}
		if (!m_completed.is_empty()) {
			m_args = m_completed.ptr();
			m_argcount = int(m_completed.size());
		}
		return true;
	}

	const Variant **args() const { return m_args; }
	int argcount() const { return m_argcount; }

private:
	LocalVector<const Variant *> m_completed;
	LocalVector<Variant> m_narrowed;
	const Variant **m_args = nullptr;
	int m_argcount = 0;
};
