
#define INSTANCE_SELF ((ELFScriptInstance *)p_self)

static GDExtensionScriptInstanceInfo2 init_script_instance_info() {
	GDExtensionScriptInstanceInfo2 info;
	ScriptInstance::init_script_instance_info_common(info);

	info.property_can_revert_func = [](void *p_self, GDExtensionConstStringNamePtr p_name) -> GDExtensionBool {
		return INSTANCE_SELF->property_can_revert(*((StringName *)p_name));
	};

	info.property_get_revert_func = [](void *p_self, GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) -> GDExtensionBool {
		return INSTANCE_SELF->property_get_revert(*((StringName *)p_name), (Variant *)r_ret);
	};

	info.call_func = [](void *p_self, GDExtensionConstStringNamePtr p_method, const GDExtensionConstVariantPtr *p_args, GDExtensionInt p_argument_count, GDExtensionVariantPtr r_return, GDExtensionCallError *r_error) {
		return INSTANCE_SELF->call(*((StringName *)p_method), (const Variant **)p_args, p_argument_count, (Variant *)r_return, r_error);
	};

	info.notification_func = [](void *p_self, int32_t p_what, GDExtensionBool p_reversed) {
		INSTANCE_SELF->notification(p_what);
	};

	info.to_string_func = [](void *p_self, GDExtensionBool *r_is_valid, GDExtensionStringPtr r_out) {
		INSTANCE_SELF->to_string(r_is_valid, (String *)r_out);
	};

	info.free_func = [](void *p_self) {
		memdelete(INSTANCE_SELF);
	};

	info.refcount_decremented_func = [](void *) -> GDExtensionBool {
		// If false (default), object cannot die
		return true;
	};

	return info;
}

const GDExtensionScriptInstanceInfo2 ELFScriptInstance::INSTANCE_INFO = init_script_instance_info();

int ELFScriptInstance::call_internal(const StringName &p_method, lua_State *ET, int p_nargs, int p_nret) {
	LUAU_LOCK(ET);

	const ELFScript *s = script.ptr();

	while (s) {
		if (s->methods.has(p_method)) {
			LuaStackOp<String>::push(ET, p_method);
			s->def_table_get(ET);

			if (!lua_isfunction(ET, -1)) {
				lua_pop(ET, 1);
				return -1;
			}

			lua_insert(ET, -p_nargs - 1);

			LuaStackOp<Object *>::push(ET, owner);
			lua_insert(ET, -p_nargs - 1);

			INIT_TIMEOUT(ET)
			int status = lua_resume(ET, nullptr, p_nargs + 1);

			if (status != LUA_OK && status != LUA_YIELD) {
				s->error("ELFScriptInstance::call_internal", LuaStackOp<String>::get(ET, -1));

				lua_pop(ET, 1);
				return status;
			}

			lua_settop(ET, p_nret);
			return status;
		}

		s = s->base.ptr();
	}

	return -1;
}

bool ELFScriptInstance::set(const StringName &p_name, const Variant &p_value, PropertySetGetError *r_err) {
#define SET_METHOD "ELFScriptInstance::set"
#define SET_NAME "_Set"

	const ELFScript *s = script.ptr();

	while (s) {
		HashMap<StringName, uint64_t>::ConstIterator E = s->get_definition().property_indices.find(p_name);

		if (E) {
			const GDClassProperty &prop = s->get_definition().properties[E->value];

			// Check type
			if (!Utils::variant_types_compatible(p_value.get_type(), Variant::Type(prop.property.type))) {
				if (r_err)
					*r_err = PROP_WRONG_TYPE;

				return false;
			}

			// Check read-only (getter, no setter)
			if (prop.setter == StringName() && prop.getter != StringName()) {
				if (r_err)
					*r_err = PROP_READ_ONLY;

				return false;
			}

			// Set
			LUAU_LOCK(T);
			lua_State *ET = lua_newthread(T);
			int status = LUA_OK;

			if (prop.setter != StringName()) {
				LuaStackOp<Variant>::push(ET, p_value);
				status = call_internal(prop.setter, ET, 1, 0);
			} else {
				LuaStackOp<String>::push(ET, p_name);
				LuaStackOp<Variant>::push(ET, p_value);
				table_set(ET);
			}

			lua_pop(T, 1); // thread

			if (status == LUA_OK || status == LUA_YIELD || status == LUA_BREAK) {
				if (r_err)
					*r_err = PROP_OK;

				return true;
			} else if (status == -1) {
				if (r_err)
					*r_err = PROP_NOT_FOUND;

				s->error(SET_METHOD, "Setter for '" + p_name + "' not found", 1);
			} else {
				if (r_err)
					*r_err = PROP_SET_FAILED;
			}

			return false;
		}

		if (s->methods.has(SET_NAME)) {
			lua_State *ET = lua_newthread(T);

			LuaStackOp<String>::push(ET, p_name);
			LuaStackOp<Variant>::push(ET, p_value);
			int status = call_internal(SET_NAME, ET, 2, 1);

			if (status == OK) {
				if (lua_type(ET, -1) == LUA_TBOOLEAN) {
					bool valid = lua_toboolean(ET, -1);

					if (valid) {
						if (r_err) {
							*r_err = PROP_OK;
						}

						lua_pop(T, 1); // thread
						return true;
					}
				} else {
					if (r_err) {
						*r_err = PROP_SET_FAILED;
					}

					s->error(SET_METHOD, "Expected " SET_NAME " to return a boolean", 1);
					lua_pop(T, 1); // thread
					return false;
				}
			}

			lua_pop(T, 1); // thread
		}

		s = s->base.ptr();
	}

	if (r_err)
		*r_err = PROP_NOT_FOUND;

	return false;
}

bool ELFScriptInstance::get(const StringName &p_name, Variant &r_ret, PropertySetGetError *r_err) {
#define GET_METHOD "ELFScriptInstance::get"
#define GET_NAME "_Get"

	const ELFScript *s = script.ptr();

	while (s) {
		HashMap<StringName, uint64_t>::ConstIterator E = s->get_definition().property_indices.find(p_name);

		if (E) {
			const GDClassProperty &prop = s->get_definition().properties[E->value];

			// Check write-only (setter, no getter)
			if (prop.setter != StringName() && prop.getter == StringName()) {
				if (r_err)
					*r_err = PROP_WRITE_ONLY;

				return false;
			}

			// Get
			LUAU_LOCK(T);
			lua_State *ET = lua_newthread(T);
			int status = LUA_OK;

			if (prop.getter != StringName()) {
				status = call_internal(prop.getter, ET, 0, 1);
			} else {
				LuaStackOp<String>::push(ET, p_name);
				table_get(ET);
			}

			if (status == LUA_OK) {
				if (!LuauVariant::lua_is(ET, -1, prop.property.type)) {
					if (r_err)
						*r_err = PROP_WRONG_TYPE;

					s->error(
							GET_METHOD,
							prop.getter == StringName() ? "Table entry for '" + p_name + "' is the wrong type" : "Getter for '" + p_name + "' returned the wrong type",
							1);

					lua_pop(T, 1); // thread
					return false;
				}

				LuauVariant ret;
				ret.lua_check(ET, -1, prop.property.type);
				r_ret = ret.to_variant();

				if (r_err)
					*r_err = PROP_OK;

				lua_pop(T, 1); // thread
				return true;
			} else if (status == LUA_YIELD || status == LUA_BREAK) {
				if (r_err)
					*r_err = PROP_GET_FAILED;

				s->error(GET_METHOD, "Getter for '" + p_name + "' yielded unexpectedly", 1);
			} else if (status == -1) {
				if (r_err)
					*r_err = PROP_NOT_FOUND;

				s->error(GET_METHOD, "Getter for '" + p_name + "' not found", 1);
			} else {
				if (r_err)
					*r_err = PROP_GET_FAILED;
			}

			lua_pop(T, 1); // thread
			return false;
		}

		if (s->methods.has(GET_NAME)) {
			lua_State *ET = lua_newthread(T);

			LuaStackOp<String>::push(ET, p_name);
			int status = call_internal(GET_NAME, ET, 1, 1);

			if (status == OK) {
				if (LuaStackOp<Variant>::is(ET, -1)) {
					Variant ret = LuaStackOp<Variant>::get(ET, -1);

					if (ret != Variant()) {
						if (r_err) {
							*r_err = PROP_OK;
						}

						r_ret = ret;
						lua_pop(T, 1); // thread
						return true;
					}
				} else {
					if (r_err) {
						*r_err = PROP_GET_FAILED;
					}

					s->error("ELFScriptInstance::get", "Expected " GET_NAME " to return a Variant", 1);
					lua_pop(T, 1); // thread
					return false;
				}
			}

			lua_pop(T, 1); // thread
		}

		s = s->base.ptr();
	}

	if (r_err)
		*r_err = PROP_NOT_FOUND;

	return false;
}

GDExtensionPropertyInfo *ELFScriptInstance::get_property_list(uint32_t *r_count) {
#define GET_PROPERTY_LIST_METHOD "ELFScriptInstance::get_property_list"
#define GET_PROPERTY_LIST_NAME "_GetPropertyList"

	LocalVector<GDExtensionPropertyInfo> properties;
	LocalVector<GDExtensionPropertyInfo> custom_properties;
	HashSet<StringName> defined;

	const ELFScript *s = script.ptr();

	// Push properties in reverse then reverse the entire vector.
	// Ensures base properties are first.
	// (see _get_script_property_list)
	while (s) {
		for (int i = s->get_definition().properties.size() - 1; i >= 0; i--) {
			const GDClassProperty &prop = s->get_definition().properties[i];

			if (defined.has(prop.property.name))
				continue;

			defined.insert(prop.property.name);

			GDExtensionPropertyInfo dst;
			copy_prop(prop.property, dst);

			properties.push_back(dst);
		}

		if (s->methods.has(GET_PROPERTY_LIST_NAME)) {
			lua_State *ET = lua_newthread(T);
			int status = call_internal(GET_PROPERTY_LIST_NAME, ET, 0, 1);

			if (status != LUA_OK) {
				goto next;
			}

			if (!lua_istable(ET, -1)) {
				s->error(GET_PROPERTY_LIST_METHOD, "Expected " GET_PROPERTY_LIST_NAME " to return a table", 1);
				goto next;
			}

			{
				// Process method return value
				// Must be protected to handle errors, which is why this is jank
				lua_pushcfunction(
						ET, [](lua_State *FL) {
							int sz = lua_objlen(FL, 1);
							for (int i = 1; i <= sz; i++) {
								lua_rawgeti(FL, 1, i);

								GDProperty *ret = reinterpret_cast<GDProperty *>(lua_newuserdatadtor(FL, sizeof(GDProperty), [](void *p_ptr) {
									((GDProperty *)p_ptr)->~GDProperty();
								}));

								new (ret) GDProperty();
								*ret = luascript_read_property(FL, -2);

								lua_remove(FL, -2); // value
							}

							return sz;
						},
						"get_property_list");

				lua_insert(ET, 1);

				INIT_TIMEOUT(ET)
				int get_status = lua_pcall(ET, 1, LUA_MULTRET, 0);
				if (get_status != LUA_OK) {
					s->error(GET_PROPERTY_LIST_METHOD, LuaStackOp<String>::get(ET, -1));
					goto next;
				}

				// The entire stack of ET is now the list of GDProperty values.
				for (int i = lua_gettop(ET); i >= 1; i--) {
					GDProperty *property = reinterpret_cast<GDProperty *>(lua_touserdata(ET, i));

					GDExtensionPropertyInfo dst;
					copy_prop(*property, dst);

					custom_properties.push_back(dst);
				}
			}

		next:
			lua_pop(T, 1); // thread
		}

		s = s->base.ptr();
	}

	properties.invert();

	// Custom properties are last.
	for (int i = custom_properties.size() - 1; i >= 0; i--) {
		properties.push_back(custom_properties[i]);
	}

	int size = properties.size();
	*r_count = size;

	GDExtensionPropertyInfo *list = alloc_with_len<GDExtensionPropertyInfo>(size);
	memcpy(list, properties.ptr(), sizeof(GDExtensionPropertyInfo) * size);

	return list;
}

Variant::Type ELFScriptInstance::get_property_type(const StringName &p_name, bool *r_is_valid) const {
	const ELFScript *s = script.ptr();

	while (s) {
		HashMap<StringName, uint64_t>::ConstIterator E = s->get_definition().property_indices.find(p_name);

		if (E) {
			if (r_is_valid)
				*r_is_valid = true;

			return (Variant::Type)s->get_definition().properties[E->value].property.type;
		}

		s = s->base.ptr();
	}

	if (r_is_valid)
		*r_is_valid = false;

	return Variant::NIL;
}

bool ELFScriptInstance::property_can_revert(const StringName &p_name) {
#define PROPERTY_CAN_REVERT_NAME "_PropertyCanRevert"

	const ELFScript *s = script.ptr();

	while (s) {
		if (s->methods.has(PROPERTY_CAN_REVERT_NAME)) {
			lua_State *ET = lua_newthread(T);

			LuaStackOp<String>::push(ET, p_name);
			int status = call_internal(PROPERTY_CAN_REVERT_NAME, ET, 1, 1);

			if (status != OK) {
				lua_pop(T, 1); // thread
				return false;
			}

			if (lua_type(ET, -1) != LUA_TBOOLEAN) {
				s->error("ELFScriptInstance::property_can_revert", "Expected " PROPERTY_CAN_REVERT_NAME " to return a boolean", 1);
				lua_pop(T, 1); // thread
				return false;
			}

			bool ret = lua_toboolean(ET, -1);
			lua_pop(T, 1); // thread
			return ret;
		}

		s = s->base.ptr();
	}

	return false;
}

bool ELFScriptInstance::property_get_revert(const StringName &p_name, Variant *r_ret) {
#define PROPERTY_GET_REVERT_NAME "_PropertyGetRevert"

	const ELFScript *s = script.ptr();

	while (s) {
		if (s->methods.has(PROPERTY_GET_REVERT_NAME)) {
			lua_State *ET = lua_newthread(T);

			LuaStackOp<String>::push(ET, p_name);
			int status = call_internal(PROPERTY_GET_REVERT_NAME, ET, 1, 1);

			if (status != OK) {
				lua_pop(T, 1); // thread
				return false;
			}

			if (!LuaStackOp<Variant>::is(ET, -1)) {
				s->error("ELFScriptInstance::property_get_revert", "Expected " PROPERTY_GET_REVERT_NAME " to return a Variant", 1);
				lua_pop(T, 1); // thread
				return false;
			}

			*r_ret = LuaStackOp<Variant>::get(ET, -1);
			lua_pop(T, 1); // thread
			return true;
		}

		s = s->base.ptr();
	}

	return false;
}

bool ELFScriptInstance::has_method(const StringName &p_name) const {
	const ELFScript *s = script.ptr();

	while (s) {
		if (s->has_method(p_name))
			return true;

		s = s->base.ptr();
	}

	return false;
}

void ELFScriptInstance::call(
		const StringName &p_method,
		const Variant *const *p_args, const GDExtensionInt p_argument_count,
		Variant *r_return, GDExtensionCallError *r_error) {
	const ELFScript *s = script.ptr();

	while (s) {
		StringName actual_name = p_method;

		// check name given and name converted to pascal
		// (e.g. if Node::_ready is called -> _Ready)
		if (s->has_method(p_method, &actual_name)) {
			const GDMethod &method = s->get_definition().methods[actual_name];

			// Check argument count
			int args_allowed = method.arguments.size();
			int args_default = method.default_arguments.size();
			int args_required = args_allowed - args_default;

			if (p_argument_count < args_required) {
				r_error->error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
				r_error->argument = args_required;

				return;
			}

			if (p_argument_count > args_allowed) {
				r_error->error = GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS;
				r_error->argument = args_allowed;

				return;
			}

			// Prepare for call
			lua_State *ET = lua_newthread(T); // execution thread

			for (int i = 0; i < p_argument_count; i++) {
				const Variant &arg = *p_args[i];

				if (!(method.arguments[i].usage & PROPERTY_USAGE_NIL_IS_VARIANT) &&
						!Utils::variant_types_compatible(arg.get_type(), Variant::Type(method.arguments[i].type))) {
					r_error->error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
					r_error->argument = i;
					r_error->expected = method.arguments[i].type;

					lua_pop(T, 1); // thread
					return;
				}

				LuaStackOp<Variant>::push(ET, arg);
			}

			for (int i = p_argument_count - args_required; i < args_default; i++)
				LuaStackOp<Variant>::push(ET, method.default_arguments[i]);

			// Call
			r_error->error = GDEXTENSION_CALL_OK;

			int status = call_internal(actual_name, ET, args_allowed, 1);

			if (status == LUA_OK) {
				*r_return = LuaStackOp<Variant>::get(ET, -1);
			} else if (status == LUA_YIELD) {
				if (method.return_val.type != GDEXTENSION_VARIANT_TYPE_NIL) {
					lua_pop(T, 1); // thread
					ERR_FAIL_MSG("Non-void method yielded unexpectedly");
				}

				*r_return = Variant();
			}

			lua_pop(T, 1); // thread
			return;
		}

		s = s->base.ptr();
	}

	r_error->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
}

void ELFScriptInstance::notification(int32_t p_what) {
#define NOTIF_NAME "_Notification"

	// These notifications will fire at program exit; see ~ELFScriptInstance
	// 3: NOTIFICATION_PREDELETE_CLEANUP (not bound)
	if (p_what == Object::NOTIFICATION_PREDELETE || p_what == 3) {
		lua_State *L = LuauRuntime::get_singleton()->get_vm(vm_type);

		if (!L || !luaGD_getthreaddata(L))
			return;
	}

	const ELFScript *s = script.ptr();

	while (s) {
		if (s->methods.has(NOTIF_NAME)) {
			lua_State *ET = lua_newthread(T);

			LuaStackOp<int32_t>::push(ET, p_what);
			call_internal(NOTIF_NAME, ET, 1, 0);

			lua_pop(T, 1); // thread
		}

		s = s->base.ptr();
	}
}

void ELFScriptInstance::to_string(GDExtensionBool *r_is_valid, String *r_out) {
#define TO_STRING_NAME "_ToString"

	const ELFScript *s = script.ptr();

	while (s) {
		if (s->methods.has(TO_STRING_NAME)) {
			lua_State *ET = lua_newthread(T);

			int status = call_internal(TO_STRING_NAME, ET, 0, 1);

			if (status == LUA_OK)
				*r_out = LuaStackOp<String>::get(ET, -1);

			if (r_is_valid)
				*r_is_valid = status == LUA_OK;

			lua_pop(T, 1); // thread
			return;
		}

		s = s->base.ptr();
	}
}

bool ELFScriptInstance::table_set(lua_State *T) const {
	if (lua_mainthread(T) != lua_mainthread(this->T))
		return false;

	LUAU_LOCK(T);
	lua_getref(T, table_ref);
	lua_insert(T, -3);
	lua_settable(T, -3);
	lua_remove(T, -1);

	return true;
}

bool ELFScriptInstance::table_get(lua_State *T) const {
	if (lua_mainthread(T) != lua_mainthread(this->T))
		return false;

	LUAU_LOCK(T);
	lua_getref(T, table_ref);
	lua_insert(T, -2);
	lua_gettable(T, -2);
	lua_remove(T, -2);

	return true;
}

#define DEF_GETTER(m_type, m_method_name, m_def_key)                                                   \
	const m_type *ELFScriptInstance::get_##m_method_name(const StringName &p_name) const {            \
		const ELFScript *s = script.ptr();                                                            \
                                                                                                       \
		while (s) {                                                                                    \
			HashMap<StringName, m_type>::ConstIterator E = s->get_definition().m_def_key.find(p_name); \
                                                                                                       \
			if (E)                                                                                     \
				return &E->value;                                                                      \
                                                                                                       \
			s = s->base.ptr();                                                                         \
		}                                                                                              \
                                                                                                       \
		return nullptr;                                                                                \
	}

DEF_GETTER(GDMethod, method, methods)

const GDClassProperty *ELFScriptInstance::get_property(const StringName &p_name) const {
	const ELFScript *s = script.ptr();

	while (s) {
		if (s->has_property(p_name))
			return &s->get_property(p_name);

		s = s->base.ptr();
	}

	return nullptr;
}

DEF_GETTER(GDMethod, signal, signals)

const Variant *ELFScriptInstance::get_constant(const StringName &p_name) const {
	HashMap<StringName, Variant>::ConstIterator E = script->constants.find(p_name);
	return E ? &E->value : nullptr;
}

ELFScriptInstance *ELFScriptInstance::from_object(GDExtensionObjectPtr p_object) {
	if (!p_object)
		return nullptr;

	Ref<ELFScript> script = nb::Object(p_object).get_script();
	uint64_t id = internal::gdextension_interface_object_get_instance_id(p_object);
	if (script.is_valid() && script->instance_has(id))
		return script->instance_get(id);

	return nullptr;
}

ELFScriptInstance::ELFScriptInstance(const Ref<ELFScript> &p_script, Object *p_owner, LuauRuntime::VMType p_vm_type) :
		script(p_script), owner(p_owner), vm_type(p_vm_type) {
#define INST_CTOR_METHOD "ELFScriptInstance::ELFScriptInstance"

	// this usually occurs in _instance_create, but that is marked const for ScriptExtension
	{
		MutexLock lock(*LuauLanguage::singleton->lock.ptr());
		p_script->instances.insert(p_owner->get_instance_id(), this);
	}

	LocalVector<ELFScript *> base_scripts;
	ELFScript *s = p_script.ptr();

	while (s) {
		base_scripts.push_back(s);
		permissions = permissions | s->get_definition().permissions;

		s = s->base.ptr();
	}

	base_scripts.invert(); // To initialize base-first

	if (permissions != PERMISSION_BASE) {
		CRASH_COND_MSG(SandboxService::get_singleton() && !SandboxService::get_singleton()->is_core_script(p_script->get_path()), "!!! Non-core script declared permissions !!!");
		UtilityFunctions::print_verbose("Creating instance of script ", p_script->get_path(), " with requested permissions ", p_script->get_definition().permissions);
	}

	lua_State *L = LuauRuntime::get_singleton()->get_vm(p_vm_type);
	LUAU_LOCK(L);
	T = luaGD_newthread(L, permissions);
	luaL_sandboxthread(T);

	GDThreadData *udata = luaGD_getthreaddata(T);
	udata->script = p_script;

	thread_ref = lua_ref(L, -1);
	lua_pop(L, 1); // thread

	lua_newtable(T);
	table_ref = lua_ref(T, -1);
	lua_pop(T, 1); // table

	for (ELFScript *&scr : base_scripts) {
		// Initialize default values
		for (const GDClassProperty &prop : scr->get_definition().properties) {
			if (prop.getter == StringName() && prop.setter == StringName()) {
				LuaStackOp<String>::push(T, prop.property.name);
				LuaStackOp<Variant>::push(T, prop.default_value);
				table_set(T);
			}
		}

		// Run _Init for each script
		Error method_err = scr->load_table(p_vm_type);

		if (method_err == OK) {
			LuaStackOp<String>::push(T, "_Init");
			scr->def_table_get(T);

			if (lua_isfunction(T, -1)) {
				// This object can be considered as the full script instance (minus some initialized values) because Object sets its script
				// before instance_create was called, and this instance was registered with the script before now.
				LuaStackOp<Object *>::push(T, p_owner);

				INIT_TIMEOUT(T)
				int status = lua_pcall(T, 1, 0, 0);

				if (status == LUA_YIELD) {
					p_script->error(INST_CTOR_METHOD, "_Init yielded unexpectedly");
				} else if (status != LUA_OK) {
					p_script->error(INST_CTOR_METHOD, "_Init failed: " + LuaStackOp<String>::get(T, -1));
					lua_pop(T, 1);
				}
			} else {
				lua_pop(T, 1);
			}
		} else {
			ERR_PRINT("Couldn't load script methods for " + scr->get_path());
		}
	}
}

ELFScriptInstance::~ELFScriptInstance() {
	if (script.is_valid() && owner) {
		MutexLock lock(*LuauLanguage::singleton->lock.ptr());
		script->instances.erase(owner->get_instance_id());
	}

	lua_State *L = LuauRuntime::get_singleton()->get_vm(vm_type);

	// Check to prevent issues with unref during thread free (luaGD_close in ~LuauRuntime)
	if (L && luaGD_getthreaddata(L)) {
		LUAU_LOCK(L);

		lua_unref(L, table_ref);
		lua_unref(L, thread_ref);
	}

	table_ref = -1;
	thread_ref = -1;
}
