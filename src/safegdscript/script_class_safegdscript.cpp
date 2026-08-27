#include "script_class_safegdscript.h"

#include "../elf/script_instance_helper.h"
#include "../fast_cast.hpp"
#include "../sandbox.h"
#include "../scoped_tree_base.h"
#include "call_arguments.h"
#include "script_dicts.h"
#include "script_language_safegdscript.h"
#include "script_safegdscript.h"
#include "signature_info.h"
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <stdexcept>
#include <unordered_map>

Sandbox *safegdscript_acquire_sandbox(Object *p_owner, const Ref<SafeGDScript> &p_script);
void safegdscript_release_sandbox(SafeGDScript *p_script, Object *p_owner);

static String utf8_of(const std::string &p_text) {
	return String::utf8(p_text.c_str(), int64_t(p_text.size()));
}

// GDExtension hands out no way back from an Object to its script instance, so
// the instances keep a register of themselves. Only the super bypass reads it.
static std::unordered_map<uint64_t, SafeGDScriptClassInstance *> bound_instances;

void SafeGDScriptClass::configure(SafeGDScript *p_outer,
		const gdscript::ClassSignature &p_signature,
		const std::vector<gdscript::FunctionSignature> &p_signatures)
{
	outer_id = ObjectID(p_outer->get_instance_id());
	class_name = StringName(utf8_of(p_signature.name));
	native_base = StringName(utf8_of(p_signature.native_base));
	fields = p_signature.fields;
	line = p_signature.line;
	base = Ref<SafeGDScriptClass>();
	methods_info.clear();
	method_lines.clear();

	// The lifted method's parameters live in the function table under
	// '@Class.method'; the synthetic self slot was never part of them.
	HashMap<String, const gdscript::FunctionSignature *> by_name;
	for (const gdscript::FunctionSignature &signature : p_signatures) {
		by_name.insert(utf8_of(signature.name), &signature);
	}
	const String prefix = "@" + String(class_name) + ".";
	for (const gdscript::ClassMethod &declared : p_signature.methods) {
		const String name = utf8_of(declared.name);
		MethodInfo method(name);
		method.flags = METHOD_FLAG_VARARG;
		method.return_val.usage = PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT;
		if (HashMap<String, const gdscript::FunctionSignature *>::Iterator it =
					by_name.find(prefix + name);
				it != by_name.end()) {
			method = method_info_from_signature(*it->value, name);
			method_lines.insert(method.name, it->value->line);
		}
		if (declared.is_static) {
			method.flags |= METHOD_FLAG_STATIC;
		}
		methods_info.push_back(std::move(method));
	}
	valid = true;
}

void SafeGDScriptClass::invalidate() {
	methods_info.clear();
	method_lines.clear();
	fields.clear();
	base = Ref<SafeGDScriptClass>();
	valid = false;
}

SafeGDScript *SafeGDScriptClass::get_outer_script() const {
	if (outer_id == ObjectID()) {
		return nullptr;
	}
	return fast_cast_to<SafeGDScript>(ObjectDB::get_instance(outer_id));
}

Ref<Script> SafeGDScriptClass::_get_base_script() const {
	return base;
}

bool SafeGDScriptClass::_inherits_script(const Ref<Script> &p_script) const {
	if (p_script.ptr() == static_cast<const Script *>(this)) {
		return true;
	}
	for (Ref<SafeGDScriptClass> at = base; at.is_valid(); at = at->base) {
		if (at == p_script) {
			return true;
		}
	}
	return false;
}

bool SafeGDScriptClass::_is_valid() const {
	return valid && get_outer_script() != nullptr;
}

ScriptLanguage *SafeGDScriptClass::_get_language() const {
	return SafeGDScriptLanguage::get_singleton();
}

const MethodInfo *SafeGDScriptClass::find_method_info(const StringName &p_method) const {
	for (const SafeGDScriptClass *at = this; at != nullptr; at = at->base.ptr()) {
		for (const MethodInfo &method : at->methods_info) {
			if (method.name == p_method) {
				return &method;
			}
		}
	}
	return nullptr;
}

StringName SafeGDScriptClass::lifted_symbol(const StringName &p_method) const {
	for (const SafeGDScriptClass *at = this; at != nullptr; at = at->base.ptr()) {
		for (const MethodInfo &method : at->methods_info) {
			if (method.name == p_method) {
				return StringName("@" + String(at->class_name) + "." + String(p_method));
			}
		}
	}
	return StringName();
}

bool SafeGDScriptClass::_has_method(const StringName &p_method) const {
	return find_method_info(p_method) != nullptr;
}

bool SafeGDScriptClass::_has_static_method(const StringName &p_method) const {
	const MethodInfo *method = find_method_info(p_method);
	return method != nullptr && (method->flags & METHOD_FLAG_STATIC);
}

Dictionary SafeGDScriptClass::_get_method_info(const StringName &p_method) const {
	if (const MethodInfo *method = find_method_info(p_method)) {
		return method_dict(*method);
	}
	return Dictionary();
}

TypedArray<Dictionary> SafeGDScriptClass::_get_script_method_list() const {
	TypedArray<Dictionary> list;
	for (const SafeGDScriptClass *at = this; at != nullptr; at = at->base.ptr()) {
		for (const MethodInfo &method : at->methods_info) {
			list.push_back(method_dict(method));
		}
	}
	return list;
}

TypedArray<Dictionary> SafeGDScriptClass::_get_script_property_list() const {
	TypedArray<Dictionary> list;
	for (const gdscript::ClassField &field : fields) {
		PropertyInfo info;
		info.name = utf8_of(field.name);
		info.type = variant_type_or_nil(field.type);
		info.class_name = StringName("Variant");
		info.usage = PROPERTY_USAGE_SCRIPT_VARIABLE |
				(info.type == Variant::NIL ? PROPERTY_USAGE_NIL_IS_VARIANT : 0);
		list.push_back(property_dict(info));
	}
	return list;
}

TypedArray<StringName> SafeGDScriptClass::_get_members() const {
	TypedArray<StringName> members;
	for (const gdscript::ClassField &field : fields) {
		members.push_back(StringName(utf8_of(field.name)));
	}
	return members;
}

int32_t SafeGDScriptClass::_get_member_line(const StringName &p_member) const {
	// -1, not 0: a caller opens the script at the answer, and 0 is the top of
	// some other file rather than "look elsewhere".
	for (const SafeGDScriptClass *at = this; at != nullptr; at = at->base.ptr()) {
		if (const int32_t *found = at->method_lines.getptr(p_member)) {
			return *found;
		}
	}
	return p_member == class_name ? line : -1;
}

bool SafeGDScriptClass::_instance_has(Object *p_object) const {
	for (const SafeGDScriptClassInstance *instance : instances) {
		if (instance->get_owner_object() == p_object) {
			return true;
		}
	}
	return false;
}

void *SafeGDScriptClass::_instance_create(Object *p_for_object) const {
	// The guest builds the instance; the bind syscall parks its Dictionary here
	// around set_script(). Nothing parked means someone attached this Script by
	// hand, and there is no instance behind it to attach to.
	if (pending_self.is_empty()) {
		ERR_PRINT("SafeGDScript: the nested class '" + String(class_name) +
				"' is constructed by the script that declares it, not from the host.");
		return nullptr;
	}
	SafeGDScriptClassInstance *instance = memnew(SafeGDScriptClassInstance(
			p_for_object, Ref<SafeGDScriptClass>(this), pending_self));
	instances.insert(instance);
	return ScriptInstanceExtension::create_native_instance(instance);
}

// -= The instance =-

SafeGDScriptClassInstance::SafeGDScriptClassInstance(Object *p_owner,
		const Ref<SafeGDScriptClass> &p_script, const Dictionary &p_self) :
		owner(p_owner), script(p_script), self(p_self)
{
	// Counts in the shared machine exactly like an outer instance, so it cannot
	// go away underneath this one. The tree base stays whatever the outer
	// instance set; a call sets its own for the duration.
	outer = Ref<SafeGDScript>(p_script->get_outer_script());
	if (outer.is_valid()) {
		sandbox = safegdscript_acquire_sandbox(p_owner, outer);
	}
	if (p_owner != nullptr) {
		bound_instances.insert_or_assign(uint64_t(p_owner->get_instance_id()), this);
	}
}

SafeGDScriptClassInstance::~SafeGDScriptClassInstance() {
	if (owner != nullptr) {
		auto it = bound_instances.find(uint64_t(owner->get_instance_id()));
		if (it != bound_instances.end() && it->second == this) {
			bound_instances.erase(it);
		}
	}
	if (script.is_valid()) {
		script->remove_instance(this);
	}
	if (outer.is_valid()) {
		safegdscript_release_sandbox(outer.ptr(), owner);
		outer = Ref<SafeGDScript>();
	}
	sandbox = nullptr;
}

ScriptLanguage *SafeGDScriptClassInstance::_get_language() {
	return SafeGDScriptLanguage::get_singleton();
}

Variant SafeGDScriptClassInstance::callp(const StringName &p_method, const Variant **p_args,
		int p_argcount, GDExtensionCallError &r_error)
{
	// `super.method()` on the native base: refuse once so Object::callp falls
	// through to the engine's MethodBind instead of re-entering the caller.
	if (!bypass.is_empty() && bypass == p_method) {
		bypass = StringName();
		r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return Variant();
	}

	const StringName symbol = script->lifted_symbol(p_method);
	if (sandbox == nullptr || symbol.is_empty()) {
		r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return Variant();
	}
	const gaddr_t address = sandbox->cached_address_of(symbol);
	if (address == 0) {
		r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return Variant();
	}

	const MethodInfo *method = script->find_method_info(p_method);
	CompletedArguments completed;
	if (!completed.complete(method, p_args, p_argcount, r_error)) {
		return Variant();
	}

	// self first: the compiler lifts a method to a free function taking the
	// instance Dictionary in the first parameter slot.
	const bool takes_self = method == nullptr || !(method->flags & METHOD_FLAG_STATIC);
	LocalVector<const Variant *> forwarded;
	Variant self_value = self;
	forwarded.reserve(completed.argcount() + 1);
	if (takes_self) {
		forwarded.push_back(&self_value);
	}
	for (int i = 0; i < completed.argcount(); i++) {
		forwarded.push_back(completed.args()[i]);
	}

	// $Node and get_node() inside the class resolve against the object itself.
	// An inner class cannot see the outer instance's members in GDScript, so the
	// default record is right and this instance allocates none of its own.
	ScopedTreeBase stb(sandbox, fast_cast_to<Node>(owner));
	ScopedInstanceBase sib(sandbox, sandbox->get_default_instance_base());
	return sandbox->vmcall_address(address, forwarded.ptr(), int(forwarded.size()), r_error);
}

bool SafeGDScriptClassInstance::has_method(const StringName &p_method) const {
	return script->find_method_info(p_method) != nullptr;
}

GDExtensionInt SafeGDScriptClassInstance::get_method_argument_count(const StringName &p_method,
		bool &r_valid) const
{
	const MethodInfo *method = script->find_method_info(p_method);
	if (method == nullptr || (method->flags & METHOD_FLAG_VARARG)) {
		r_valid = false;
		return 0;
	}
	r_valid = true;
	return GDExtensionInt(method->arguments.size());
}

const GDExtensionMethodInfo *SafeGDScriptClassInstance::get_method_list(uint32_t *r_count) const {
	std::vector<const MethodInfo *> all;
	for (const SafeGDScriptClass *at = script.ptr(); at != nullptr; at = at->get_base_class()) {
		for (const MethodInfo &method : at->get_methods_info()) {
			all.push_back(&method);
		}
	}
	GDExtensionMethodInfo *list = memnew_arr(GDExtensionMethodInfo, all.size());
	for (size_t i = 0; i < all.size(); i++) {
		list[i] = create_method_info(*all[i]);
	}
	*r_count = uint32_t(all.size());
	return list;
}

void SafeGDScriptClassInstance::free_method_list(const GDExtensionMethodInfo *p_list,
		uint32_t p_count) const
{
	if (p_list) {
		for (uint32_t i = 0; i < p_count; i++) {
			free_method_info(p_list[i]);
		}
		memdelete_arr(p_list);
	}
}

bool SafeGDScriptClassInstance::get(const StringName &p_name, Variant &r_ret) const {
	if (self.has(p_name)) {
		r_ret = self[p_name];
		return true;
	}
	// Not ours: Godot goes on to the engine property.
	return false;
}

bool SafeGDScriptClassInstance::set(const StringName &p_name, const Variant &p_value) {
	// Declared fields only. A stray set("typo", 1) has to reach the engine and
	// fail there rather than silently grow the Dictionary.
	for (const gdscript::ClassField &field : script->get_fields()) {
		if (utf8_of(field.name) == String(p_name)) {
			self[p_name] = p_value;
			return true;
		}
	}
	return false;
}

Variant::Type SafeGDScriptClassInstance::get_property_type(const StringName &p_name,
		bool *r_is_valid) const
{
	for (const gdscript::ClassField &field : script->get_fields()) {
		if (utf8_of(field.name) == String(p_name)) {
			*r_is_valid = true;
			return variant_type_or_nil(field.type);
		}
	}
	*r_is_valid = false;
	return Variant::NIL;
}

const GDExtensionPropertyInfo *SafeGDScriptClassInstance::get_property_list(uint32_t *r_count) const {
	const std::vector<gdscript::ClassField> &declared = script->get_fields();
	*r_count = uint32_t(declared.size());
	GDExtensionPropertyInfo *list = memnew_arr(GDExtensionPropertyInfo, declared.size() + 1);
	GDExtensionPropertyInfo *at = list;
	for (const gdscript::ClassField &field : declared) {
		const Variant::Type type = variant_type_or_nil(field.type);
		at->name = stringname_alloc(utf8_of(field.name));
		at->class_name = stringname_alloc("Variant");
		at->type = GDExtensionVariantType(type);
		at->hint = PROPERTY_HINT_NONE;
		at->hint_string = string_alloc(String());
		at->usage = uint32_t(PROPERTY_USAGE_SCRIPT_VARIABLE) |
				(type == Variant::NIL ? uint32_t(PROPERTY_USAGE_NIL_IS_VARIANT) : 0u);
		at++;
	}
	return list;
}

void SafeGDScriptClassInstance::free_property_list(const GDExtensionPropertyInfo *p_list,
		uint32_t p_count) const
{
	if (p_list) {
		memdelete_arr(p_list);
	}
}

void SafeGDScriptClassInstance::notification(int p_notification, bool p_reversed) {
	static const StringName s_notification("_notification");
	if (script->find_method_info(s_notification) == nullptr) {
		return;
	}
	Variant what = int64_t(p_notification);
	const Variant *args[] = { &what };
	GDExtensionCallError error;
	this->callp(s_notification, args, 1, error);
}

String SafeGDScriptClassInstance::to_string(bool *r_valid) {
	static const StringName s_to_string("_to_string");
	if (script->find_method_info(s_to_string) != nullptr) {
		GDExtensionCallError error;
		const Variant answer = this->callp(s_to_string, nullptr, 0, error);
		if (error.error == GDEXTENSION_CALL_OK) {
			*r_valid = true;
			return answer.operator String();
		}
	}
	*r_valid = true;
	return "<" + String(script->get_class_name()) + ">";
}

// Unreachable: a RefCounted base never gets a script instance (see the bind
// below), so nothing here is ever asked to break a cycle.
bool SafeGDScriptClassInstance::refcount_decremented() {
	return false;
}

// -= The bind syscall's host half =-

void safegdscript_bind_nested_class(Sandbox &p_sandbox, Object *p_base,
		const Dictionary &p_instance, const String &p_class_name)
{
	SafeGDScript *outer = p_sandbox.get_script_owner_id() == ObjectID()
			? nullptr
			: fast_cast_to<SafeGDScript>(ObjectDB::get_instance(p_sandbox.get_script_owner_id()));
	// A plain Sandbox node running the ELF owns no Script resources; the class
	// stays the Dictionary it has always been.
	if (outer == nullptr) {
		return;
	}
	// A RefCounted base would be a cycle we cannot break: "@base" is a strong
	// reference to the object, the object owns the instance, and the instance
	// holds the Dictionary. Godot only offers refcount_decremented() to cut it,
	// and cutting it there means dropping the last reference from inside the
	// object's own unreference(). Such a class stays what it has always been --
	// a plain Dictionary with no host-side script.
	if (fast_cast_to<RefCounted>(p_base) != nullptr) {
		return;
	}
	if (p_base->get_script().get_type() != Variant::NIL) {
		ERR_PRINT("SafeGDScript: '" + p_class_name + "' already carries a script; "
				"attaching another would replace it.");
		throw std::runtime_error("class_bind: the object already has a script");
	}
	Ref<SafeGDScriptClass> nested = outer->find_nested_class(StringName(p_class_name));
	if (nested.is_null()) {
		ERR_PRINT("SafeGDScript: the program constructs a class '" + p_class_name +
				"' the compiler did not publish.");
		throw std::runtime_error("class_bind: unknown class " + std::string(p_class_name.utf8().get_data()));
	}

	nested->pending_self = p_instance;
	p_base->set_script(Variant(Ref<Script>(nested)));
	nested->pending_self = Dictionary();
}

void safegdscript_bypass_super(Object *p_object, const StringName &p_method) {
	if (p_object == nullptr) {
		return;
	}
	auto it = bound_instances.find(uint64_t(p_object->get_instance_id()));
	if (it != bound_instances.end()) {
		it->second->bypass_once(p_method);
	}
}
