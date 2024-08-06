
class ELFScriptInstance : public ScriptInstance {
	Ref<ELFScript> script;
	Object *owner;
	LuauRuntime::VMType vm_type;
	BitField<ThreadPermissions> permissions = PERMISSION_BASE;

	int table_ref;
	int thread_ref;
	lua_State *T;

	int call_internal(const StringName &p_method, lua_State *ET, int p_nargs, int p_nret);

public:
	static const GDExtensionScriptInstanceInfo2 INSTANCE_INFO;

	bool set(const StringName &p_name, const Variant &p_value, PropertySetGetError *r_err = nullptr) override;
	bool get(const StringName &p_name, Variant &r_ret, PropertySetGetError *r_err = nullptr) override;

	GDExtensionPropertyInfo *get_property_list(uint32_t *r_count) override;

	Variant::Type get_property_type(const StringName &p_name, bool *r_is_valid) const override;

	bool property_can_revert(const StringName &p_name);
	bool property_get_revert(const StringName &p_name, Variant *r_ret);

	bool has_method(const StringName &p_name) const override;

	void call(const StringName &p_method, const Variant *const *p_args, const GDExtensionInt p_argument_count, Variant *r_return, GDExtensionCallError *r_error);

	void notification(int32_t p_what);
	void to_string(GDExtensionBool *r_is_valid, String *r_out);

	Object *get_owner() const override { return owner; }
	Ref<ELFScript> get_script() const override { return script; }

	bool table_set(lua_State *T) const;
	bool table_get(lua_State *T) const;

	LuauRuntime::VMType get_vm_type() const { return vm_type; }

	const GDMethod *get_method(const StringName &p_name) const;
	const GDClassProperty *get_property(const StringName &p_name) const;
	const GDMethod *get_signal(const StringName &p_name) const;
	const Variant *get_constant(const StringName &p_name) const;

	static ELFScriptInstance *from_object(GDExtensionObjectPtr p_object);

	ELFScriptInstance(const Ref<ELFScript> &p_script, Object *p_owner, LuauRuntime::VMType p_vm_type);
	~ELFScriptInstance();
};
