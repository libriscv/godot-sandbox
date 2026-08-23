#include "guest_datatypes.h"
#include "sandbox_function_state.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>

// Stackless coroutines: suspend copies the Variant slot array out, promoting scoped
// handles to permanent; resume copies it back. Guest stack does not survive.

namespace {

static constexpr uint32_t MAX_COROUTINE_FRAME = 64u * 1024u;

inline GuestVariant *frame_slot(std::vector<uint8_t> &frame, size_t offset) {
	return reinterpret_cast<GuestVariant *>(frame.data() + offset);
}

} // namespace

Sandbox::Coroutine *Sandbox::find_coroutine(uint64_t id) noexcept {
	if (id == 0) {
		return nullptr;
	}
	for (const std::unique_ptr<Coroutine> &co : m_coroutines) {
		if (co->id == id) {
			return co.get();
		}
	}
	return nullptr;
}

void Sandbox::retire_coroutine(uint64_t id, bool invalidate_state) {
	for (size_t i = 0; i < m_coroutines.size(); i++) {
		if (m_coroutines[i]->id != id) {
			continue;
		}
		std::unique_ptr<Coroutine> co = std::move(m_coroutines[i]);
		m_coroutines.erase(m_coroutines.begin() + i);
		if (m_resuming_coroutine_id == id) {
			m_resuming_coroutine_id = 0;
		}
		// A re-suspension leaves these set for coroutine_resume() to consume; a dead id
		// in them answers the next unrelated vmcall with Variant().
		if (m_pending_suspend == id) {
			m_pending_suspend = 0;
		}
		if (m_resume_entry_id == id) {
			m_resume_entry_id = 0;
		}
		// Release permanent Variants on retirement.
		for (int32_t perm : co->promoted) {
			this->release_permanent_variant(perm);
		}
		// Emit completed(null) for waiting callers.
		if (invalidate_state && co->state_object.is_valid()) {
			co->state_object->invalidate();
			co->state_object->emit_signal("completed", Variant());
		}
		return;
	}
}

void Sandbox::reap_coroutines_internal(bool notify) {
	// Drain before invalidation: user code must not see a half-dismantled table.
	std::vector<std::unique_ptr<Coroutine>> dying = std::move(m_coroutines);
	m_coroutines.clear();
	m_resuming_coroutine_id = 0;
	m_resume_entry_id = 0;
	m_pending_suspend = 0;
	for (std::unique_ptr<Coroutine> &co : dying) {
		for (int32_t perm : co->promoted) {
			this->release_permanent_variant(perm);
		}
		if (co->state_object.is_valid()) {
			co->state_object->invalidate();
			// Suppressed from ~Sandbox() to avoid running user code during destruction.
			if (notify) {
				co->state_object->emit_signal("completed", Variant());
			}
		}
	}
}

// Promote scoped handles to permanent so they survive across calls.
void Sandbox::promote_frame_handles(Coroutine &co) {
	const size_t slot_size = sizeof(GuestVariant);
	const size_t slots = co.frame.size() / slot_size;

	std::vector<int32_t> kept;
	std::vector<Coroutine::FrameObject> objects;
	std::vector<Ref<RefCounted>> refs;

	for (size_t i = 0; i < slots; i++) {
		const size_t offset = i * slot_size;
		GuestVariant *gv = frame_slot(co.frame, offset);

		if (gv->is_scoped_variant()) {
			const int32_t idx = int32_t(gv->v.i);
			if (this->is_permanent_variant(idx)) {
				// Already promoted by an earlier suspension of this same frame.
				if (this->permanent_index_valid(idx)) {
					kept.push_back(idx);
					continue;
				}
				gv->type = Variant::NIL;
				gv->v.i = 0;
				continue;
			}
			std::optional<const Variant *> var = this->get_scoped_variant(idx);
			if (!var.has_value()) {
				gv->type = Variant::NIL;
				gv->v.i = 0;
				continue;
			}
			// Copy, not duplicate: containers are references.
			const int32_t perm = this->create_permanent_variant_from(Variant(*var.value()));
			if (perm == 0) {
				// Fail the await; resuming with a local silently nulled is worse.
				ERR_PRINT("await: permanent Variant pool full (raise references_max); the coroutine cannot suspend");
				// The retire in coroutine_suspend()'s catch releases co.promoted, so this
				// pass's slots have to be in it.
				for (int32_t taken : kept) {
					if (std::find(co.promoted.begin(), co.promoted.end(), taken) == co.promoted.end()) {
						co.promoted.push_back(taken);
					}
				}
				throw std::runtime_error("await: permanent Variant pool full");
			}
			gv->v.i = perm;
			kept.push_back(perm);
			continue;
		}

		if (gv->type == Variant::OBJECT) {
			// Preserve as ObjectID; addresses can be reused across calls.
			const uintptr_t handle = uintptr_t(gv->v.i);
			godot::Object *obj = nullptr;
			if (CurrentState::ScopedObject *so = this->find_scoped_object(handle)) {
				obj = Sandbox::resolve_scoped_object(*so);
			}
			const uint64_t object_id = Sandbox::engine_object_id(obj);
			gv->v.i = 0;
			if (object_id == 0) {
				// Not recorded in objects, so restore never revisits it: NIL rather than
				// an OBJECT tag on handle 0.
				gv->type = Variant::NIL;
				continue;
			}
			objects.push_back(Coroutine::FrameObject{ uint32_t(offset), object_id });
			if (RefCounted *rc = Object::cast_to<RefCounted>(obj)) {
				refs.push_back(Ref<RefCounted>(rc));
			}
		}
	}

	// Release slots no longer referenced by this suspension.
	for (int32_t perm : co.promoted) {
		if (std::find(kept.begin(), kept.end(), perm) == kept.end()) {
			this->release_permanent_variant(perm);
		}
	}
	co.promoted = std::move(kept);
	co.objects = std::move(objects);
	co.refs = std::move(refs);
}

bool Sandbox::coroutine_suspend(gaddr_t operand_addr, gaddr_t frame_base, uint32_t frame_size,
		int32_t state_index, gaddr_t resume_address, int32_t result_offset) {
	machine_t &m = this->machine();

	if (frame_size == 0 || frame_size > MAX_COROUTINE_FRAME || (frame_size % sizeof(GuestVariant)) != 0) {
		ERR_PRINT("await: invalid coroutine frame size " + itos(frame_size));
		throw std::runtime_error("await: invalid coroutine frame size");
	}
	if (result_offset >= 0 && uint32_t(result_offset) + sizeof(GuestVariant) > frame_size) {
		ERR_PRINT("await: result slot lies outside the coroutine frame");
		throw std::runtime_error("await: result slot lies outside the coroutine frame");
	}
	// The promotion walk steps on slot boundaries; a straddling slot splices two records.
	if (result_offset >= 0 && (uint32_t(result_offset) % sizeof(GuestVariant)) != 0) {
		ERR_PRINT("await: result slot is not on a Variant boundary");
		throw std::runtime_error("await: result slot is not on a Variant boundary");
	}

	GuestVariant *operand = m.memory.memarray<GuestVariant>(operand_addr, 1);

	// Non-Signal: copy operand into result slot and report no suspension.
	const auto hand_the_operand_back = [&]() {
		if (result_offset >= 0) {
			GuestVariant *dst = m.memory.memarray<GuestVariant>(frame_base + result_offset, 1);
			if (dst != operand) {
				*dst = *operand;
			}
		}
		return false;
	};

	Variant awaited = operand->toVariant(*this);
	if (awaited.get_type() != Variant::SIGNAL) {
		return hand_the_operand_back();
	}

	Signal signal = awaited;
	godot::Object *target = signal.get_object();
	if (target == nullptr) {
		ERR_PRINT("await: the awaited Signal has no object");
		return hand_the_operand_back();
	}
	if (!this->is_allowed_object(target)) {
		ERR_PRINT("await: not allowed to await a signal on " + target->get_class());
		throw std::runtime_error("await: object is not allowed");
	}
	if (!this->is_allowed_class(target->get_class())) {
		ERR_PRINT("await: not allowed to await a signal on class " + target->get_class());
		throw std::runtime_error("await: class is not allowed");
	}
	// Before a frame is taken. connect() reports this as ERR_INVALID_PARAMETER, which the
	// already-connected case below tolerates.
	if (!target->has_signal(signal.get_name())) {
		ERR_PRINT("await: " + target->get_class() + " has no signal " + String(signal.get_name()));
		throw std::runtime_error("await: the awaited Signal does not exist");
	}

	Coroutine *co = this->find_coroutine(m_resuming_coroutine_id);
	if (co == nullptr) {
		if (m_coroutines.size() >= m_max_coroutines) {
			ERR_PRINT("await: too many live coroutines (" + itos(int64_t(m_max_coroutines)) +
					"); raise max_coroutines or let some complete");
			throw std::runtime_error("await: too many live coroutines");
		}
		auto fresh = std::make_unique<Coroutine>();
		fresh->id = m_next_coroutine_id++;
		fresh->generation = m_program_generation;
		fresh->state_object.instantiate();
		fresh->state_object->initialize(this, fresh->id);
		co = fresh.get();
		m_coroutines.push_back(std::move(fresh));
	}

	co->resume_address = resume_address;
	co->state_index = state_index;
	co->result_offset = result_offset;
	co->sent = Variant();
	co->frame.resize(frame_size);
	// A bad frame_base throws; retire the record so it does not pin max_coroutines.
	try {
		m.copy_from_guest(co->frame.data(), frame_base, frame_size);
		this->promote_frame_handles(*co);
	} catch (...) {
		this->retire_coroutine(co->id, true);
		throw;
	}

	const Error err = Error(signal.connect(
			Callable(co->state_object.ptr(), "resume_from_signal"),
			Object::CONNECT_ONE_SHOT));
	// The signal exists, so ERR_INVALID_PARAMETER means already connected: armed either way.
	if (err != OK && err != ERR_INVALID_PARAMETER) {
		ERR_PRINT("await: could not connect to " + String(signal.get_name()));
		this->retire_coroutine(co->id, true);
		throw std::runtime_error("await: could not connect to the awaited signal");
	}

	m_pending_suspend = co->id;
	return true;
}

int32_t Sandbox::coroutine_restore(gaddr_t frame_base, uint32_t frame_size) {
	Coroutine *co = this->find_coroutine(m_resuming_coroutine_id);
	if (co == nullptr) {
		ERR_PRINT("await: frame restore outside a resume");
		throw std::runtime_error("await: frame restore outside a resume");
	}
	if (frame_size != co->frame.size()) {
		ERR_PRINT("await: resume frame is " + itos(frame_size) + " bytes, the suspension recorded " +
				itos(int64_t(co->frame.size())));
		throw std::runtime_error("await: coroutine frame size mismatch");
	}

	// Re-resolve ObjectIDs into this call's scoped objects; freed objects become null.
	for (const Coroutine::FrameObject &fo : co->objects) {
		GuestVariant *gv = frame_slot(co->frame, fo.offset);
		GDExtensionObjectPtr live = internal::gdextension_interface_object_get_instance_from_id(fo.object_id);
		if (live == nullptr) {
			gv->type = Variant::NIL;
			gv->v.i = 0;
			continue;
		}
		gv->v.i = int64_t(this->add_scoped_engine_object(uintptr_t(live)));
	}

	if (co->result_offset >= 0) {
		GuestVariant *dst = frame_slot(co->frame, uint32_t(co->result_offset));
		*dst = GuestVariant{};
		dst->set(*this, co->sent, true);
	}

	machine().copy_to_guest(frame_base, co->frame.data(), co->frame.size());
	return co->state_index;
}

bool Sandbox::coroutine_resume(uint64_t id, const Variant &sent) {
	Coroutine *co = this->find_coroutine(id);
	if (co == nullptr) {
		return false;
	}
	if (co->generation != m_program_generation) {
		this->retire_coroutine(id, true);
		return false;
	}
	if (co->running) {
		ERR_PRINT("await: coroutine " + itos(int64_t(id)) + " is already running");
		return false;
	}
	if (co->resume_address == 0) {
		return false;
	}

	co->sent = sent;
	co->running = true;

	const gaddr_t resume_address = co->resume_address;
	m_resume_entry_id = id;
	Variant result = this->vmcall_internal(resume_address, nullptr, 0);

	co = this->find_coroutine(id);
	if (co == nullptr) {
		return true;
	}
	co->running = false;

	if (m_pending_suspend == id) {
		m_pending_suspend = 0;
		return true;
	}

	Ref<SandboxFunctionState> state = co->state_object;
	this->retire_coroutine(id, false);
	if (state.is_valid()) {
		state->invalidate();
		state->emit_signal("completed", result);
	}
	return true;
}

//-- SandboxFunctionState --//

void SandboxFunctionState::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_valid"), &SandboxFunctionState::is_valid);
	ClassDB::bind_method(D_METHOD("get_coroutine_id"), &SandboxFunctionState::get_coroutine_id);

	// Vararg: the awaited signal's arity is not known here.
	{
		MethodInfo mi;
		mi.name = "resume_from_signal";
		ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "resume_from_signal",
				&SandboxFunctionState::resume_from_signal, mi);
	}

	ADD_SIGNAL(MethodInfo("completed", PropertyInfo(Variant::NIL, "result",
			PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT)));
}

String SandboxFunctionState::_to_string() const {
	return "[SandboxFunctionState:" + itos(get_instance_id()) + "]";
}

bool SandboxFunctionState::is_valid() const {
	if (m_sandbox == nullptr || m_coroutine_id == 0) {
		return false;
	}
	return m_sandbox->find_coroutine(m_coroutine_id) != nullptr;
}

Variant SandboxFunctionState::resume_from_signal(const Variant **args, GDExtensionInt arg_count,
		GDExtensionCallError &error) {
	error.error = GDEXTENSION_CALL_OK;

	// 0 args → null, 1 → the argument, N → Array.
	Variant sent;
	if (arg_count == 1) {
		sent = *args[0];
	} else if (arg_count > 1) {
		Array packed;
		for (GDExtensionInt i = 0; i < arg_count; i++) {
			packed.push_back(*args[i]);
		}
		sent = packed;
	}

	if (m_sandbox == nullptr || m_coroutine_id == 0) {
		return Variant();
	}
	m_sandbox->coroutine_resume(m_coroutine_id, sent);
	return Variant();
}
