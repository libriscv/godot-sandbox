#include "script_instance_safegdscript.h"
#include <functional>

#include "../elf/script_elf.h"
#include "../elf/script_instance.h"
#include "../elf/script_instance_helper.h"
#include "../fast_cast.hpp"
#include "../sandbox.h"
#include "../scoped_tree_base.h"
#include "../variant_coerce.h"
#include "script_safegdscript.h"
#include "script_language_safegdscript.h"
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/signal.hpp>
static constexpr bool VERBOSE_LOGGING = false;

bool SafeGDScriptInstance::set(const StringName &p_name, const Variant &p_value) {
	static const StringName s_script("script");
	static const StringName s_program("program");
	if (p_name == s_script || p_name == s_program) {
		return false;
	}

	Sandbox *sandbox = current_sandbox;
	ScopedTreeBase stb(sandbox, fast_cast_to<Node>(this->owner));
	ScopedInstanceBase sib(sandbox, this->current_instance_base());
	if (sandbox->set_property(p_name, p_value)) {
		return true;
	}
	return false;
}

bool SafeGDScriptInstance::get(const StringName &p_name, Variant &r_ret) const {
	static const StringName s_script("script");
	if (p_name == s_script) {
		r_ret = this->script;
		return true;
	}
	Sandbox *sandbox = current_sandbox;
	ScopedTreeBase stb(sandbox, fast_cast_to<Node>(this->owner));
	ScopedInstanceBase sib(sandbox, this->current_instance_base());
	if (sandbox->get_property(p_name, r_ret)) {
		return true;
	}
	if (script.is_valid() && script->_has_script_signal(p_name)) {
		r_ret = Signal(this->owner, p_name);
		return true;
	}
	return false;
}

godot::String SafeGDScriptInstance::to_string(bool *r_is_valid) {
	return "<SafeGDScript>";
}

void SafeGDScriptInstance::notification(int32_t p_what, bool p_reversed) {
}

Variant SafeGDScriptInstance::callp(
		const StringName &p_method,
		const Variant **p_args, const int p_argument_count,
		GDExtensionCallError &r_error)
{
	Sandbox *sandbox = current_sandbox;
	const auto address = sandbox->cached_address_of(p_method.hash(), p_method);
	if (address == 0) {
		const bool found = sandbox->is_sandbox_function(p_method);
		if (!found) {
			r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
			return Variant();
		}
		Array args;
		for (int i = 0; i < p_argument_count; i++) {
			args.push_back(*p_args[i]);
		}
		r_error.error = GDEXTENSION_CALL_OK;
		ScopedTreeBase stb(sandbox, fast_cast_to<Node>(this->owner));
		ScopedInstanceBase sib(sandbox, this->current_instance_base());
		return sandbox->callv(p_method, args);
	}

	// The Sandbox ABI passes one Variant pointer per argument and no count, so
	// an argument the caller left out is a null pointer the guest dereferences
	// as soon as it reads the parameter. Nothing downstream can recover from
	// that, and Godot cannot check the arity itself: through a statically-typed
	// Node it has no idea a .sgd script is on the other side. So the call is
	// completed, or refused, here.
	//
	// Defaults are the same problem seen from the callee: it cannot fill one in
	// either, having no way to tell whether it was given the argument. The
	// compiler hands the constant ones over with the signature, and they are
	// appended here exactly as Godot appends its own.
	LocalVector<const Variant *> completed;
	LocalVector<Variant> narrowed;
	if (const MethodInfo *method = script->find_method_info(p_method)) {
		if (!(method->flags & METHOD_FLAG_VARARG)) {
			const int expected = int(method->arguments.size());
			const int required = expected - int(method->default_arguments.size());
			if (p_argument_count < required) {
				r_error.error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
				r_error.argument = p_argument_count;
				r_error.expected = required;
				return Variant();
			}
			if (p_argument_count > expected) {
				r_error.error = GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS;
				r_error.argument = p_argument_count;
				r_error.expected = expected;
				return Variant();
			}
			for (int i = 0; i < p_argument_count; i++) {
				const Variant::Type declared = method->arguments[i].type;
				if (declared == Variant::NIL || p_args[i]->get_type() == declared) {
					continue;
				}
				if (completed.is_empty()) {
					narrowed.resize(p_argument_count);
					completed.reserve(expected);
					for (int j = 0; j < p_argument_count; j++) {
						narrowed[j] = *p_args[j];
						completed.push_back(&narrowed[j]);
					}
				}
				if (!coerce_variant_to(narrowed[i], declared)) {
					r_error.error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
					r_error.argument = i;
					r_error.expected = declared;
					return Variant();
				}
			}
			if (p_argument_count < expected) {
				if (completed.is_empty()) {
					completed.reserve(expected);
					for (int i = 0; i < p_argument_count; i++) {
						completed.push_back(p_args[i]);
					}
				}
				const int first_default = int(method->default_arguments.size()) - (expected - p_argument_count);
				for (int i = first_default; i < int(method->default_arguments.size()); i++) {
					completed.push_back(&method->default_arguments[i]);
				}
			}
			if (!completed.is_empty()) {
				p_args = completed.ptr();
			}
		}
	}

	//WARN_PRINT("SafeGDScriptInstance::callp: Calling method " + p_method + " at address " + itos(address) + " with " + itos(p_argument_count) + " arguments.");
	ScopedTreeBase stb(sandbox, fast_cast_to<Node>(this->owner));
	ScopedInstanceBase sib(sandbox, this->current_instance_base());
	return sandbox->vmcall_address(address, p_args, completed.is_empty() ? p_argument_count : int(completed.size()), r_error);
}

const GDExtensionMethodInfo *SafeGDScriptInstance::get_method_list(uint32_t *r_count) const {
	const int size = script->methods_info.size();
	GDExtensionMethodInfo *list = memnew_arr(GDExtensionMethodInfo, size);
	int i = 0;
	for (const godot::MethodInfo &method_info : script->methods_info) {
		if constexpr (VERBOSE_LOGGING) {
			ERR_PRINT("ELFScriptInstance::get_method_list: method " + String(method_info.name));
		}
		list[i] = create_method_info(method_info);
		i++;
	}
	*r_count = size;

	return list;
}

static void set_property_info(
		GDExtensionPropertyInfo &p_info,
		const StringName &p_name,
		const StringName &p_class_name,
		GDExtensionVariantType p_type,
		uint32_t p_hint,
		const String &p_hint_string,
		uint32_t p_usage)
{
	p_info.name = stringname_alloc(p_name);
	p_info.class_name = stringname_alloc(p_class_name);
	p_info.type = p_type;
	p_info.hint = p_hint;
	p_info.hint_string = string_alloc(p_hint_string);
	p_info.usage = p_usage;
}

const GDExtensionPropertyInfo *SafeGDScriptInstance::get_property_list(uint32_t *r_count) const {
	Sandbox *sandbox = current_sandbox;
	ScopedInstanceBase sib(sandbox, this->current_instance_base());
	std::vector<PropertyInfo> prop_list = sandbox->create_sandbox_property_list();

	// Sandboxed properties
	const std::vector<SandboxProperty> &properties = sandbox->get_properties();

	*r_count = properties.size() + prop_list.size();
	GDExtensionPropertyInfo *list = memnew_arr(GDExtensionPropertyInfo, *r_count + 2);
	const GDExtensionPropertyInfo *list_ptr = list;

	for (const SandboxProperty &property : properties) {
		if constexpr (VERBOSE_LOGGING) {
			printf("SafeGDScriptInstance::get_property_list %s\n", String(property.name()).utf8().ptr());
			fflush(stdout);
		}
		list->name = stringname_alloc(property.name());
		list->class_name = stringname_alloc("Variant");
		list->type = (GDExtensionVariantType)property.type();
		list->hint = property.hint();
		list->hint_string = string_alloc(property.hint_string());
		// NIL_IS_VARIANT goes by type, not by annotation: without it NIL reads as void.
		uint32_t usage = property.usage() != 0
			? property.usage()
			: uint32_t(PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_SCRIPT_VARIABLE);
		if (property.type() == Variant::NIL) {
			usage |= uint32_t(PROPERTY_USAGE_NIL_IS_VARIANT);
		} else {
			usage &= ~uint32_t(PROPERTY_USAGE_NIL_IS_VARIANT);
		}
		list->usage = usage;
		list++;
	}
	for (int i = 0; i < prop_list.size(); i++) {
		const PropertyInfo &prop = prop_list[i];
		if constexpr (VERBOSE_LOGGING) {
			printf("SafeGDScriptInstance::get_property_list %s\n", String(prop.name).utf8().ptr());
			fflush(stdout);
		}
		if (prop.name == StringName("program")) {
			*r_count -= 1;
			continue;
		}
		list->name = stringname_alloc(prop.name);
		list->class_name = stringname_alloc(prop.class_name);
		list->type = (GDExtensionVariantType) int(prop.type);
		list->hint = prop.hint;
		list->hint_string = string_alloc(prop.hint_string);
		list->usage = prop.usage;
		list++;
	}
	return list_ptr;
}
void SafeGDScriptInstance::free_property_list(const GDExtensionPropertyInfo *p_list, uint32_t p_count) const {
	if (p_list) {
		memdelete_arr(p_list);
	}
}

Variant::Type SafeGDScriptInstance::get_property_type(const StringName &p_name, bool *r_is_valid) const {
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScriptInstance::get_property_type " + p_name);
	}
	Sandbox *sandbox = current_sandbox;
	if (const SandboxProperty *prop = sandbox->find_property_or_null(p_name)) {
		*r_is_valid = true;
		return prop->type();
	}
	*r_is_valid = false;
	return Variant::NIL;
}

void SafeGDScriptInstance::get_property_state(GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) {
}

bool SafeGDScriptInstance::validate_property(GDExtensionPropertyInfo &p_property) const {
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScriptInstance::validate_property");
	}
	return true;
}

GDExtensionInt SafeGDScriptInstance::get_method_argument_count(const StringName &p_method, bool &r_valid) const {
	const MethodInfo *method = script->find_method_info(p_method);
	// A vararg entry is one the compiler said nothing about, so the count is
	// unknown rather than zero.
	if (method == nullptr || (method->flags & METHOD_FLAG_VARARG)) {
		r_valid = false;
		return 0;
	}
	r_valid = true;
	return GDExtensionInt(method->arguments.size());
}

bool SafeGDScriptInstance::has_method(const StringName &p_name) const {
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScriptInstance::has_method " + p_name);
	}
	for (const godot::MethodInfo &method_info : script->methods_info) {
		if (method_info.name == p_name) {
			return true;
		}
	}
	return false;
}

void SafeGDScriptInstance::free_method_list(const GDExtensionMethodInfo *p_list, uint32_t p_count) const {
	if (p_list) {
		for (uint32_t i = 0; i < p_count; i++) {
			free_method_info(p_list[i]);
		}
		memdelete_arr(p_list);
	}
}

bool SafeGDScriptInstance::property_can_revert(const StringName &p_name) const {
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScriptInstance::property_can_revert " + p_name);
	}
	return false;
}

bool SafeGDScriptInstance::property_get_revert(const StringName &p_name, Variant &r_ret) const {
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScriptInstance::property_get_revert " + p_name);
	}
	r_ret = Variant();
	return false;
}

void SafeGDScriptInstance::refcount_incremented() {
}

bool SafeGDScriptInstance::refcount_decremented() {
	return false;
}

Object *SafeGDScriptInstance::get_owner() {
	return owner;
}

Ref<Script> SafeGDScriptInstance::get_script() const {
	return script;
}

bool SafeGDScriptInstance::is_placeholder() const {
	return false;
}

void SafeGDScriptInstance::property_set_fallback(const StringName &p_name, const Variant &p_value, bool *r_valid) {
	*r_valid = false;
}

Variant SafeGDScriptInstance::property_get_fallback(const StringName &p_name, bool *r_valid) {
	*r_valid = false;
	return Variant::NIL;
}

ScriptLanguage *SafeGDScriptInstance::_get_language() {
	return SafeGDScriptLanguage::get_singleton();
}

void SafeGDScriptInstance::reset_to(const PackedByteArray &p_elf_data) {
	Sandbox *sandbox = current_sandbox;
	sandbox->load_buffer(p_elf_data);
	// The records went with the old machine. Nothing is released -- that memory
	// is gone -- and every instance takes a fresh one on its next call.
}

uint64_t SafeGDScriptInstance::current_instance_base() const {
	Sandbox *sandbox = current_sandbox;
	const uint64_t generation = sandbox->get_program_generation();
	if (generation != this->instance_generation) {
		this->instance_base = sandbox->create_instance_record();
		this->instance_generation = generation;
	}
	return this->instance_base;
}

static constexpr uint32_t SGD_MEMORY_MAX = 32;

struct SandboxAndCount {
	Sandbox *sandbox = nullptr;
	unsigned count = 0;
};
static std::unordered_map<SafeGDScript *, SandboxAndCount> sandbox_instances;

// One Sandbox per script, shared by every instance of it, so the profiling
// toggle names a script rather than a node. Looked up from the toggle, which
// only knows the Sandbox that was toggled.
SafeGDScript *safegdscript_for_sandbox(const Sandbox *p_sandbox) {
	for (const auto &[script, entry] : sandbox_instances) {
		if (entry.sandbox == p_sandbox) {
			return script;
		}
	}
	return nullptr;
}

Sandbox *sandbox_for_safegdscript(const SafeGDScript *p_script) {
	auto it = sandbox_instances.find(const_cast<SafeGDScript *>(p_script));
	return it == sandbox_instances.end() ? nullptr : it->second.sandbox;
}

void safegdscript_class_restrictions_changed(const Sandbox &p_sandbox) {
	static bool in_progress = false;
	if (in_progress) {
		return;
	}
	if (p_sandbox.is_in_vmcall()) {
		return;
	}
	in_progress = true;
	std::vector<SafeGDScript *> affected;
	for (const auto &[script, entry] : sandbox_instances) {
		if (script != nullptr && entry.sandbox == &p_sandbox) {
			affected.push_back(script);
		}
	}
	for (SafeGDScript *script : affected) {
		script->class_restrictions_changed();
	}
	in_progress = false;
}

void safegdscript_for_each_sandbox(const std::function<void(SafeGDScript &, Sandbox &)> &p_callback) {
	for (const auto &[script, entry] : sandbox_instances) {
		if (script != nullptr && entry.sandbox != nullptr) {
			p_callback(*script, *entry.sandbox);
		}
	}
}

static Sandbox *create_sandbox(Object *p_owner, const Ref<SafeGDScript> &p_script) {
	auto it = sandbox_instances.find(p_script.ptr());
	if (it != sandbox_instances.end()) {
		it->second.count++;
		return it->second.sandbox;
	}

	Sandbox *sandbox_ptr = memnew(Sandbox());
	sandbox_ptr->set_tree_base(fast_cast_to<Node>(p_owner));
	sandbox_ptr->set_unboxed_arguments(false);
	sandbox_ptr->set_memory_max(SGD_MEMORY_MAX);
	sandbox_ptr->load_buffer(p_script->get_content());
	sandbox_instances.insert_or_assign(p_script.ptr(), SandboxAndCount{sandbox_ptr, 1});

	return sandbox_ptr;
}

SafeGDScriptInstance::SafeGDScriptInstance(Object *p_owner, const Ref<SafeGDScript> p_script) :
		owner(p_owner), script(p_script)
{
	this->current_sandbox = create_sandbox(p_owner, p_script);
	this->current_sandbox->set_tree_base(fast_cast_to<godot::Node>(owner));
	// A script-level `var` is a member: this instance gets its own, initialized
	// the way the program initialized its own at startup.
	this->instance_base = this->current_sandbox->create_instance_record();
	this->instance_generation = this->current_sandbox->get_program_generation();
	this->call_init();
}

void SafeGDScriptInstance::call_init() {
	static const StringName init_name("_init");
	Sandbox *sandbox = this->current_sandbox;
	if (sandbox == nullptr || sandbox->cached_address_of(init_name.hash(), init_name) == 0) {
		return;
	}
	if (const MethodInfo *method = script->find_method_info(init_name)) {
		if (method->arguments.size() != method->default_arguments.size()) {
			ERR_PRINT("SafeGDScript: " + script->get_path() + ": _init() takes arguments, so it "
					"cannot run for a script attached to a node.");
			return;
		}
	}
	GDExtensionCallError error;
	this->callp(init_name, nullptr, 0, error);
	if (error.error != GDEXTENSION_CALL_OK) {
		ERR_PRINT("SafeGDScript: " + script->get_path() + ": _init() failed with call error " +
				itos(int(error.error)));
	}
}

SafeGDScriptInstance::~SafeGDScriptInstance() {
	auto it = sandbox_instances.find(script.ptr());
	if (it != sandbox_instances.end()) {
		it->second.count--;
		// Last one out takes the Sandbox with it, records and all; and a record
		// from a machine that has since been replaced is already gone.
		if (it->second.count > 0 && this->instance_base != 0 &&
			this->instance_generation == it->second.sandbox->get_program_generation()) {
			it->second.sandbox->destroy_instance_record(this->instance_base);
		}
		if (it->second.count == 0) {
			it->second.sandbox->queue_free();
			sandbox_instances.erase(it);
		} else if (Node *owner_node = fast_cast_to<Node>(this->owner)) {
			if (it->second.sandbox->get_tree_base_id() == godot::ObjectID(owner_node->get_instance_id())) {
				it->second.sandbox->set_tree_base(nullptr);
			}
		}
	}
	this->current_sandbox = nullptr;
	script->remove_instance(this);
}
