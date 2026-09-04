#include "script_instance_safegdscript.h"
#include "sgd_timing.h"
#include <functional>

#include "../elf/script_elf.h"
#include "../elf/script_instance.h"
#include "../elf/script_instance_helper.h"
#include "../fast_cast.hpp"
#include "../sandbox.h"
#include "../scoped_tree_base.h"
#include "../variant_coerce.h"
#include "call_arguments.h"
#include "script_safegdscript.h"
#include "script_language_safegdscript.h"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
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
	ScopedCallContext ctx(sandbox, tree_base_id, script_instance_owner_id, this->current_instance_base());
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
	ScopedCallContext ctx(sandbox, tree_base_id, script_instance_owner_id, this->current_instance_base());
	if (sandbox->get_property(p_name, r_ret)) {
		return true;
	}
	if (script.is_valid() && script->_has_script_signal(p_name)) {
		r_ret = Signal(this->owner, p_name);
		return true;
	}
	// Folded `const` and `enum`: no guest storage backs them, so they are answered
	// off the script. Last, so a member of the same name still wins -- the guest
	// resolves its own names at compile time and never reaches here.
	if (script.is_valid()) {
		const Variant *constant = script->constants.getptr(p_name);
		if (constant != nullptr) {
			r_ret = *constant;
			return true;
		}
	}
	return false;
}

godot::String SafeGDScriptInstance::to_string(bool *r_is_valid) {
	return "<SafeGDScript>";
}

void SafeGDScriptInstance::notification(int32_t p_what, bool p_reversed) {
	static const StringName s_notification("_notification");
	// Called for every NOTIFICATION_*, so the script that declares none pays a
	// name comparison per notification and nothing else.
	uint64_t address = 0;
	const MethodInfo *method = script->find_method_info(s_notification, current_sandbox, &address);
	if (method == nullptr) {
		return;
	}
	Variant what = int64_t(p_what);
	const Variant *args[] = { &what };
	Variant result;
	GDExtensionCallError error;
	this->call_method(method, address, args, 1, result, error);
}

void SafeGDScriptInstance::callp(
		const StringName &p_method,
		const Variant **p_args, const int p_argument_count,
		Variant &r_return, GDExtensionCallError &r_error)
{
	Sandbox *sandbox = current_sandbox;
	uint64_t address = 0;
	const MethodInfo *method = script->find_method_info(p_method, sandbox, &address);
	if (method == nullptr) address = sandbox->cached_address_of(p_method);
	if (address == 0) {
		const bool found = sandbox->is_sandbox_function(p_method);
		if (!found) {
			r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
			r_return = Variant();
			return;
		}
		Array args;
		for (int i = 0; i < p_argument_count; i++) {
			args.push_back(*p_args[i]);
		}
		r_error.error = GDEXTENSION_CALL_OK;
		ScopedCallContext ctx(sandbox, tree_base_id, script_instance_owner_id, this->current_instance_base());
		r_return = sandbox->callv(p_method, args);
		return;
	}

	this->call_method(method, address,
			p_args, p_argument_count, r_return, r_error);
}

void SafeGDScriptInstance::call_method(const MethodInfo *p_method, uint64_t p_address,
		const Variant **p_args, int p_argcount, Variant &r_return,
		GDExtensionCallError &r_error) {
	Sandbox *const sandbox = current_sandbox;
	CompletedArguments completed;
	if (!completed.complete(p_method, p_args, p_argcount, r_error)) {
		r_return = Variant();
		return;
	}

	ScopedCallContext ctx(sandbox, tree_base_id, script_instance_owner_id, this->current_instance_base());
	sandbox->vmcall_address(p_address, completed.args(), completed.argcount(), r_return, r_error);
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

	// Script properties are compiler metadata. This keeps inspector shape
	// identical to placeholders and avoids asking the running guest for facts.
	const std::vector<PropertyInfo> script_properties = script->member_property_infos();
	*r_count = uint32_t(script_properties.size() + prop_list.size());
	GDExtensionPropertyInfo *list = memnew_arr(GDExtensionPropertyInfo, *r_count + 2);
	const GDExtensionPropertyInfo *list_ptr = list;

	for (const PropertyInfo &property : script_properties) {
		set_property_info(*list, property.name, property.class_name,
				GDExtensionVariantType(property.type), property.hint,
				property.hint_string, property.usage);
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
	if (const gdscript::PropertySignature *prop = script->find_property_signature(p_name)) {
		*r_is_valid = true;
		return prop->type < 0 ? Variant::NIL : Variant::Type(prop->type);
	}
	*r_is_valid = false;
	return Variant::NIL;
}

void SafeGDScriptInstance::get_property_state(GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) {
	for (const gdscript::PropertySignature &property : script->properties) {
		if (!property.is_member || (property.usage & PROPERTY_USAGE_STORAGE) == 0) {
			continue;
		}
		const StringName name = String::utf8(property.name.c_str(), property.name.size());
		Variant value;
		if (get(name, value)) {
			p_add_func(reinterpret_cast<GDExtensionConstStringNamePtr>(&name),
					reinterpret_cast<GDExtensionConstVariantPtr>(&value), p_userdata);
		}
	}
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
	return script->property_defaults.has(p_name);
}

bool SafeGDScriptInstance::property_get_revert(const StringName &p_name, Variant &r_ret) const {
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScriptInstance::property_get_revert " + p_name);
	}
	return script->property_default(p_name, r_ret);
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
	{
		SGD_TIME_LOAD();
		sandbox->load_buffer(p_elf_data);
	}
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

static Sandbox *create_sandbox(Object *p_owner, const Ref<SafeGDScript> &p_script,
		bool p_restricted) {
	auto it = sandbox_instances.find(p_script.ptr());
	if (it != sandbox_instances.end()) {
		it->second.count++;
		return it->second.sandbox;
	}

	Sandbox *sandbox_ptr = memnew(Sandbox());
	sandbox_ptr->set_tree_base(fast_cast_to<Node>(p_owner));
	sandbox_ptr->set_unboxed_arguments(false);
	sandbox_ptr->set_memory_max(SGD_MEMORY_MAX);
	if (p_restricted) {
		sandbox_ptr->set_restrictions(true);
	}
	// Set before the program runs: a nested class binds during a guest call, and
	// the bind syscall reaches its Script resources through this.
	sandbox_ptr->set_script_owner_id(godot::ObjectID(p_script->get_instance_id()));
	// Registered before the program runs: a member declared with a nested class
	// type is constructed at startup, and the bind that follows has to find this
	// machine rather than build a second one -- which would run startup again.
	sandbox_instances.insert_or_assign(p_script.ptr(), SandboxAndCount{sandbox_ptr, 1});
	{
		SGD_TIME_LOAD();
		sandbox_ptr->load_buffer(p_script->get_content());
	}

	return sandbox_ptr;
}

// A nested class's instance keeps the same shared machine alive as an outer one:
// it counts here, and the last instance out -- nested or outer -- frees it.
Sandbox *safegdscript_acquire_sandbox(Object *p_owner, const Ref<SafeGDScript> &p_script,
		bool p_restricted) {
	return create_sandbox(p_owner, p_script, p_restricted);
}

void safegdscript_release_sandbox(SafeGDScript *p_script, Object *p_owner) {
	auto it = sandbox_instances.find(p_script);
	if (it == sandbox_instances.end()) {
		return;
	}
	it->second.count--;
	if (it->second.count == 0) {
		Sandbox *sandbox = it->second.sandbox;
		sandbox_instances.erase(it);
		if (Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop()) != nullptr) {
			sandbox->queue_free();
		} else {
			memdelete(sandbox);
		}
	} else if (Node *owner_node = fast_cast_to<Node>(p_owner)) {
		if (it->second.sandbox->get_tree_base_id() == godot::ObjectID(owner_node->get_instance_id())) {
			it->second.sandbox->set_tree_base(nullptr);
		}
	}
}

SafeGDScriptInstance::SafeGDScriptInstance(Object *p_owner, const Ref<SafeGDScript> p_script) :
		owner(p_owner), script(p_script)
{
	Node *const owner_node = fast_cast_to<Node>(owner);
	tree_base_id = owner_node != nullptr ? ObjectID(owner_node->get_instance_id()) : ObjectID();
	script_instance_owner_id = owner_node == nullptr && owner != nullptr
			? ObjectID(owner->get_instance_id()) : ObjectID();
	this->current_sandbox = create_sandbox(p_owner, p_script, p_script->compiled_restricted);
	this->current_sandbox->set_tree_base_id(tree_base_id);
	this->current_sandbox->set_script_instance_owner_id(script_instance_owner_id);
	// A script-level `var` is a member: this instance gets its own, initialized
	// the way the program initialized its own at startup.
	this->instance_base = this->current_sandbox->create_instance_record();
	this->instance_generation = this->current_sandbox->get_program_generation();
	this->call_init();
}

void SafeGDScriptInstance::call_init() {
	static const StringName init_name("_init");
	Sandbox *sandbox = this->current_sandbox;
	if (sandbox == nullptr) {
		return;
	}
	uint64_t address = 0;
	const MethodInfo *method = script->find_method_info(init_name, sandbox, &address);
	if (address == 0) return;
	const Variant **args = script->pending_init_args;
	const int argcount = script->pending_init_argcount;
	if (args == nullptr) {
		if (method != nullptr) {
			if (method->arguments.size() != method->default_arguments.size()) {
				ERR_PRINT("SafeGDScript: " + script->get_path() + ": _init() takes arguments, so it "
						"cannot run for a script attached to a node.");
				return;
			}
		}
	}
	Variant result;
	GDExtensionCallError error;
	this->call_method(method, address, args, argcount, result, error);
	if (error.error != GDEXTENSION_CALL_OK) {
		ERR_PRINT("SafeGDScript: " + script->get_path() + ": _init() failed with call error " +
				itos(int(error.error)));
	}
}

SafeGDScriptInstance::~SafeGDScriptInstance() {
	auto it = sandbox_instances.find(script.ptr());
	if (it != sandbox_instances.end()) {
		it->second.count--;
		// Retire frames tied to this record before the owner and its signals go
		// away. The last instance used to skip this and leave a pending coroutine
		// connected until the queued Sandbox deletion happened.
		if (this->instance_base != 0 &&
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

SafeGDScriptStaticInstance::SafeGDScriptStaticInstance(SafeGDScript *p_script) :
		script(p_script) {}

SafeGDScriptStaticInstance::~SafeGDScriptStaticInstance() {
	release_sandbox();
}

Sandbox *SafeGDScriptStaticInstance::acquire() {
	if (this->sandbox == nullptr) {
		if (this->script->compiled_restricted && sandbox_for_safegdscript(this->script) == nullptr) {
			return nullptr;
		}
		this->sandbox = create_sandbox(nullptr, Ref<SafeGDScript>(this->script),
				this->script->compiled_restricted);
	}
	return this->sandbox;
}

void SafeGDScriptStaticInstance::release_sandbox() {
	if (this->sandbox != nullptr) {
		this->sandbox = nullptr;
		safegdscript_release_sandbox(this->script, nullptr);
	}
}

bool SafeGDScriptStaticInstance::set(const StringName &p_name, const Variant &p_value) {
	return false;
}

bool SafeGDScriptStaticInstance::get(const StringName &p_name, Variant &r_ret) const {
	return false;
}

const GDExtensionPropertyInfo *SafeGDScriptStaticInstance::get_property_list(uint32_t *r_count) const {
	*r_count = 0;
	return nullptr;
}

void SafeGDScriptStaticInstance::free_property_list(const GDExtensionPropertyInfo *p_list, uint32_t p_count) const {
}

Variant::Type SafeGDScriptStaticInstance::get_property_type(const StringName &p_name, bool *r_is_valid) const {
	*r_is_valid = false;
	return Variant::NIL;
}

bool SafeGDScriptStaticInstance::validate_property(GDExtensionPropertyInfo &p_property) const {
	return false;
}

bool SafeGDScriptStaticInstance::get_class_category(GDExtensionPropertyInfo &r_class_category) const {
	return false;
}

bool SafeGDScriptStaticInstance::property_can_revert(const StringName &p_name) const {
	return false;
}

bool SafeGDScriptStaticInstance::property_get_revert(const StringName &p_name, Variant &r_ret) const {
	return false;
}

Object *SafeGDScriptStaticInstance::get_owner() {
	return this->script;
}

void SafeGDScriptStaticInstance::get_property_state(GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) {
}

const GDExtensionMethodInfo *SafeGDScriptStaticInstance::get_method_list(uint32_t *r_count) const {
	std::vector<const godot::MethodInfo *> statics;
	for (const godot::MethodInfo &method_info : script->methods_info) {
		if (method_info.flags & METHOD_FLAG_STATIC) {
			statics.push_back(&method_info);
		}
	}
	GDExtensionMethodInfo *list = memnew_arr(GDExtensionMethodInfo, statics.size());
	for (size_t i = 0; i < statics.size(); i++) {
		list[i] = create_method_info(*statics[i]);
	}
	*r_count = uint32_t(statics.size());
	return list;
}

void SafeGDScriptStaticInstance::free_method_list(const GDExtensionMethodInfo *p_list, uint32_t p_count) const {
	if (p_list) {
		for (uint32_t i = 0; i < p_count; i++) {
			free_method_info(p_list[i]);
		}
		memdelete_arr(p_list);
	}
}

bool SafeGDScriptStaticInstance::has_method(const StringName &p_method) const {
	return script->_has_static_method(p_method);
}

GDExtensionInt SafeGDScriptStaticInstance::get_method_argument_count(const StringName &p_method, bool &r_valid) const {
	uint64_t address = 0;
	const godot::MethodInfo *method = script->find_method_info(p_method, sandbox, &address);
	if (method == nullptr || !(method->flags & METHOD_FLAG_STATIC)) {
		r_valid = false;
		return 0;
	}
	r_valid = true;
	return GDExtensionInt(method->arguments.size());
}

void SafeGDScriptStaticInstance::callp(
		const StringName &p_method,
		const Variant **p_args, const int p_argument_count,
		Variant &r_return, GDExtensionCallError &r_error)
{
	Sandbox *sandbox = acquire();
	if (sandbox == nullptr) {
		ERR_PRINT("SafeGDScript: " + script->get_path() + ": a restricted script's static "
				"function needs a live instance, which owns the machine it runs in.");
		r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		r_return = Variant();
		return;
	}
	uint64_t address = 0;
	const godot::MethodInfo *method = script->find_method_info(p_method, sandbox, &address);
	if (method == nullptr || !(method->flags & METHOD_FLAG_STATIC)) {
		r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		r_return = Variant();
		return;
	}
	if (address == 0) {
		r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		r_return = Variant();
		return;
	}
	CompletedArguments completed;
	if (!completed.complete(method, p_args, p_argument_count, r_error)) {
		r_return = Variant();
		return;
	}
	r_error.error = GDEXTENSION_CALL_OK;
	ScopedCallContext ctx(sandbox, nullptr, sandbox->get_default_instance_base());
	sandbox->vmcall_address(address, completed.args(), completed.argcount(), r_return, r_error);
}

void SafeGDScriptStaticInstance::notification(int p_notification, bool p_reversed) {
}

String SafeGDScriptStaticInstance::to_string(bool *r_valid) {
	*r_valid = false;
	return String();
}

void SafeGDScriptStaticInstance::refcount_incremented() {
}

bool SafeGDScriptStaticInstance::refcount_decremented() {
	return true;
}

Ref<Script> SafeGDScriptStaticInstance::get_script() const {
	return Ref<Script>(this->script);
}

bool SafeGDScriptStaticInstance::is_placeholder() const {
	return false;
}

void SafeGDScriptStaticInstance::property_set_fallback(const StringName &p_name, const Variant &p_value, bool *r_valid) {
	*r_valid = false;
}

Variant SafeGDScriptStaticInstance::property_get_fallback(const StringName &p_name, bool *r_valid) {
	*r_valid = false;
	return Variant();
}

ScriptLanguage *SafeGDScriptStaticInstance::_get_language() {
	return SafeGDScriptLanguage::get_singleton();
}
