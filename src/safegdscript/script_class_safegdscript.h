#pragma once

#include "../gdscript/compiler/function_signature.h"
#include "../godot/script_instance.h"
#include "script_safegdscript.h"
#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <vector>

using namespace godot;

class Sandbox;
class SafeGDScriptClassInstance;

// A nested class with an engine base, as a Script resource of its own.
//
// The guest keeps its Dictionary; this is the host-side identity of the same
// instance, so Godot has something to call _ready/_process/_input on and
// something to answer has_method()/get()/get_script() as. Deliberately small:
// compilation, reload, breakpoints, profiling and base-source tracking belong to
// the outer SafeGDScript, and a nested class owns none of them.
class SafeGDScriptClass : public ScriptExtension {
	GDCLASS(SafeGDScriptClass, ScriptExtension);

protected:
	static void _bind_methods() {}

public:
	// Object::set_script() only builds a script instance when the Script says it
	// can be instantiated, so this has to be true for the bind to take. Attaching
	// one by hand is refused in _instance_create() instead, where the guest's
	// Dictionary is either parked or missing.
	virtual bool _can_instantiate() const override { return true; }
	virtual Ref<Script> _get_base_script() const override;
	virtual StringName _get_global_name() const override { return StringName(); }
	virtual bool _inherits_script(const Ref<Script> &p_script) const override;
	virtual StringName _get_instance_base_type() const override { return native_base; }
	virtual void *_instance_create(Object *p_for_object) const override;
	virtual bool _instance_has(Object *p_object) const override;
	virtual bool _has_source_code() const override { return false; }
	virtual String _get_source_code() const override { return String(); }
	virtual void _set_source_code(const String &p_code) override {}
	virtual Error _reload(bool p_keep_state) override { return Error::OK; }
	virtual bool _has_method(const StringName &p_method) const override;
	virtual bool _has_static_method(const StringName &p_method) const override;
	virtual Dictionary _get_method_info(const StringName &p_method) const override;
	virtual bool _is_tool() const override { return false; }
	virtual bool _is_valid() const override;
	virtual bool _is_abstract() const override { return false; }
	virtual ScriptLanguage *_get_language() const override;
	virtual bool _has_script_signal(const StringName &p_signal) const override { return false; }
	virtual TypedArray<Dictionary> _get_script_signal_list() const override { return {}; }
	virtual TypedArray<Dictionary> _get_script_method_list() const override;
	virtual TypedArray<Dictionary> _get_script_property_list() const override;
	virtual TypedArray<StringName> _get_members() const override;
	virtual int32_t _get_member_line(const StringName &p_member) const override;
	virtual bool _is_placeholder_fallback_enabled() const override { return false; }

	// Rebuilt in place on every compile: live instances hold a Ref to this object
	// and get_script() has to keep answering the same thing across a reload.
	void configure(SafeGDScript *p_outer, const gdscript::ClassSignature &p_signature,
			const std::vector<gdscript::FunctionSignature> &p_signatures);
	void set_base_class(const Ref<SafeGDScriptClass> &p_base) { base = p_base; }
	// The source no longer declares this class; its instances answer nothing.
	void invalidate();

	SafeGDScript *get_outer_script() const;
	const StringName &get_class_name() const { return class_name; }
	const std::vector<gdscript::ClassField> &get_fields() const { return fields; }
	const std::vector<MethodInfo> &get_methods_info() const { return methods_info; }
	const SafeGDScriptClass *get_base_class() const { return base.ptr(); }
	// Own methods first, then the declared chain's.
	const MethodInfo *find_method_info(const StringName &p_method) const;
	// '@Class.method' for the class that declares it, or empty when nothing does.
	StringName lifted_symbol(const StringName &p_method) const;

	void remove_instance(SafeGDScriptClassInstance *p_instance) { instances.erase(p_instance); }

	// Parked around the set_script() call that creates the instance; the
	// Dictionary is how the guest and the instance share one set of fields.
	Dictionary pending_self;

private:
	ObjectID outer_id;
	StringName class_name;
	StringName native_base;
	Ref<SafeGDScriptClass> base;
	std::vector<MethodInfo> methods_info;
	std::vector<gdscript::ClassField> fields;
	int32_t line = 0;
	// Declaration line per method, for the editor's go-to-definition.
	HashMap<StringName, int32_t> method_lines;
	bool valid = false;
	mutable HashSet<SafeGDScriptClassInstance *> instances;
};

class SafeGDScriptClassInstance : public ScriptInstanceExtension {
	Object *owner = nullptr;
	Ref<SafeGDScriptClass> script;
	// The class holds the outer script by id, not by reference, so this is what
	// keeps the program this instance calls into alive -- and keeps the key its
	// shared Sandbox is filed under from dangling.
	Ref<SafeGDScript> outer;
	// Shared storage with the guest, not a copy: Godot's Dictionary is a handle.
	Dictionary self;
	Sandbox *sandbox = nullptr;
	// Armed by a `super.method()` on the native base for exactly one call.
	StringName bypass;

public:
	bool set(const StringName &p_name, const Variant &p_value) override;
	bool get(const StringName &p_name, Variant &r_ret) const override;
	const GDExtensionPropertyInfo *get_property_list(uint32_t *r_count) const override;
	void free_property_list(const GDExtensionPropertyInfo *p_list, uint32_t p_count) const override;
	Variant::Type get_property_type(const StringName &p_name, bool *r_is_valid) const override;
	bool validate_property(GDExtensionPropertyInfo &p_property) const override { return true; }
	bool property_can_revert(const StringName &p_name) const override { return false; }
	bool property_get_revert(const StringName &p_name, Variant &r_ret) const override { return false; }
	Object *get_owner() override { return owner; }
	Object *get_owner_object() const { return owner; }
	void get_property_state(GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) override {}
	const GDExtensionMethodInfo *get_method_list(uint32_t *r_count) const override;
	void free_method_list(const GDExtensionMethodInfo *p_list, uint32_t p_count) const override;
	bool has_method(const StringName &p_method) const override;
	GDExtensionInt get_method_argument_count(const StringName &p_method, bool &r_valid) const override;
	Variant callp(const StringName &p_method, const Variant **p_args, int p_argcount, GDExtensionCallError &r_error) override;
	void notification(int p_notification, bool p_reversed) override;
	String to_string(bool *r_valid) override;
	void refcount_incremented() override {}
	bool refcount_decremented() override;
	Ref<Script> get_script() const override { return script; }
	bool is_placeholder() const override { return false; }
	void property_set_fallback(const StringName &p_name, const Variant &p_value, bool *r_valid) override { *r_valid = false; }
	Variant property_get_fallback(const StringName &p_name, bool *r_valid) override { *r_valid = false; return Variant(); }
	ScriptLanguage *_get_language() override;

	// Refuses the next call to p_method once, so Object::callp falls through to
	// the engine's own MethodBind instead of recursing into the caller.
	void bypass_once(const StringName &p_method) { bypass = p_method; }

	SafeGDScriptClassInstance(Object *p_owner, const Ref<SafeGDScriptClass> &p_script,
			const Dictionary &p_self);
	~SafeGDScriptClassInstance();
};

// Called by ECALL_CLASS_BIND. p_base is the engine object under the guest
// Dictionary's "@base"; a Sandbox with no SafeGDScript behind it has no Script
// resources to attach and this is a no-op there.
void safegdscript_bind_nested_class(Sandbox &p_sandbox, Object *p_base,
		const Dictionary &p_instance, const String &p_class_name);

// Arms the super bypass on p_object's instance, when it has one of ours.
void safegdscript_bypass_super(Object *p_object, const StringName &p_method);
