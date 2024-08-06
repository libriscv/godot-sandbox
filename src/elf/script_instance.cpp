#define COMMON_SELF ((ScriptInstance *)p_self)

void ScriptInstance::init_script_instance_info_common(GDExtensionScriptInstanceInfo2 &p_info) {
	// Must initialize potentially unused struct fields to nullptr
	// (if not, causes segfault on MSVC).
	p_info.property_can_revert_func = nullptr;
	p_info.property_get_revert_func = nullptr;

	p_info.call_func = nullptr;
	p_info.notification_func = nullptr;

	p_info.to_string_func = nullptr;

	p_info.refcount_incremented_func = nullptr;
	p_info.refcount_decremented_func = nullptr;

	p_info.is_placeholder_func = nullptr;

	p_info.set_fallback_func = nullptr;
	p_info.get_fallback_func = nullptr;

	p_info.set_func = [](void *p_self, GDExtensionConstStringNamePtr p_name, GDExtensionConstVariantPtr p_value) -> GDExtensionBool {
		return COMMON_SELF->set(*(const StringName *)p_name, *(const Variant *)p_value);
	};

	p_info.get_func = [](void *p_self, GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) -> GDExtensionBool {
		return COMMON_SELF->get(*(const StringName *)p_name, *(Variant *)r_ret);
	};

	p_info.get_property_list_func = [](void *p_self, uint32_t *r_count) -> const GDExtensionPropertyInfo * {
		return COMMON_SELF->get_property_list(r_count);
	};

	p_info.free_property_list_func = [](void *p_self, const GDExtensionPropertyInfo *p_list) {
		COMMON_SELF->free_property_list(p_list);
	};

	p_info.validate_property_func = [](void *p_self, GDExtensionPropertyInfo *p_property) -> GDExtensionBool {
		return COMMON_SELF->validate_property(p_property);
	};

	p_info.get_owner_func = [](void *p_self) {
		return COMMON_SELF->get_owner()->_owner;
	};

	p_info.get_property_state_func = [](void *p_self, GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) {
		COMMON_SELF->get_property_state(p_add_func, p_userdata);
	};

	p_info.get_method_list_func = [](void *p_self, uint32_t *r_count) -> const GDExtensionMethodInfo * {
		return COMMON_SELF->get_method_list(r_count);
	};

	p_info.free_method_list_func = [](void *p_self, const GDExtensionMethodInfo *p_list) {
		COMMON_SELF->free_method_list(p_list);
	};

	p_info.get_property_type_func = [](void *p_self, GDExtensionConstStringNamePtr p_name, GDExtensionBool *r_is_valid) -> GDExtensionVariantType {
		return (GDExtensionVariantType)COMMON_SELF->get_property_type(*(const StringName *)p_name, (bool *)r_is_valid);
	};

	p_info.has_method_func = [](void *p_self, GDExtensionConstStringNamePtr p_name) -> GDExtensionBool {
		return COMMON_SELF->has_method(*(const StringName *)p_name);
	};

	p_info.get_script_func = [](void *p_self) {
		return COMMON_SELF->get_script().ptr()->_owner;
	};

	p_info.get_language_func = [](void *p_self) {
		return COMMON_SELF->get_language()->_owner;
	};
}

static String *string_alloc(const String &p_str) {
	String *ptr = memnew(String);
	*ptr = p_str;

	return ptr;
}

static StringName *stringname_alloc(const String &p_str) {
	StringName *ptr = memnew(StringName);
	*ptr = p_str;

	return ptr;
}

int ScriptInstance::get_len_from_ptr(const void *p_ptr) {
	return *((int *)p_ptr - 1);
}

void ScriptInstance::free_with_len(void *p_ptr) {
	memfree((int *)p_ptr - 1);
}

void ScriptInstance::copy_prop(const GDProperty &p_src, GDExtensionPropertyInfo &p_dst) {
	p_dst.type = p_src.type;
	p_dst.name = stringname_alloc(p_src.name);
	p_dst.class_name = stringname_alloc(p_src.class_name);
	p_dst.hint = p_src.hint;
	p_dst.hint_string = string_alloc(p_src.hint_string);
	p_dst.usage = p_src.usage;
}

void ScriptInstance::free_prop(const GDExtensionPropertyInfo &p_prop) {
	// smelly
	memdelete((StringName *)p_prop.name);
	memdelete((StringName *)p_prop.class_name);
	memdelete((String *)p_prop.hint_string);
}

void ScriptInstance::get_property_state(GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) {
	// ! refer to script_language.cpp get_property_state
	// the default implementation is not carried over to GDExtension

	uint32_t count = 0;
	GDExtensionPropertyInfo *props = get_property_list(&count);

	for (int i = 0; i < count; i++) {
		StringName *name = (StringName *)props[i].name;

		if (props[i].usage & PROPERTY_USAGE_STORAGE) {
			Variant value;
			bool is_valid = get(*name, value);

			if (is_valid)
				p_add_func(name, &value, p_userdata);
		}
	}

	free_property_list(props);
}

static void add_to_state(GDExtensionConstStringNamePtr p_name, GDExtensionConstVariantPtr p_value, void *p_userdata) {
	List<Pair<StringName, Variant>> *list = reinterpret_cast<List<Pair<StringName, Variant>> *>(p_userdata);
	list->push_back({ *(const StringName *)p_name, *(const Variant *)p_value });
}

void ScriptInstance::get_property_state(List<Pair<StringName, Variant>> &p_list) {
	get_property_state(add_to_state, &p_list);
}

void ScriptInstance::free_property_list(const GDExtensionPropertyInfo *p_list) const {
	if (!p_list)
		return;

	// don't ask.
	int size = get_len_from_ptr(p_list);

	for (int i = 0; i < size; i++)
		free_prop(p_list[i]);

	free_with_len((GDExtensionPropertyInfo *)p_list);
}

GDExtensionMethodInfo *ScriptInstance::get_method_list(uint32_t *r_count) const {
	LocalVector<GDExtensionMethodInfo> methods;
	HashSet<StringName> defined;

	const ELFScript *s = get_script().ptr();

	while (s) {
		for (const KeyValue<StringName, GDMethod> &pair : s->get_definition().methods) {
			if (defined.has(pair.key))
				continue;

			defined.insert(pair.key);

			const GDMethod &src = pair.value;

			GDExtensionMethodInfo dst;

			dst.name = stringname_alloc(src.name);
			copy_prop(src.return_val, dst.return_value);
			dst.flags = src.flags;
			dst.argument_count = src.arguments.size();

			if (dst.argument_count > 0) {
				GDExtensionPropertyInfo *arg_list = memnew_arr(GDExtensionPropertyInfo, dst.argument_count);

				for (int j = 0; j < dst.argument_count; j++)
					copy_prop(src.arguments[j], arg_list[j]);

				dst.arguments = arg_list;
			}

			dst.default_argument_count = src.default_arguments.size();

			if (dst.default_argument_count > 0) {
				Variant *defargs = memnew_arr(Variant, dst.default_argument_count);

				for (int j = 0; j < dst.default_argument_count; j++)
					defargs[j] = src.default_arguments[j];

				dst.default_arguments = (GDExtensionVariantPtr *)defargs;
			}

			methods.push_back(dst);
		}

		s = s->get_base().ptr();
	}

	int size = methods.size();
	*r_count = size;

	GDExtensionMethodInfo *list = alloc_with_len<GDExtensionMethodInfo>(size);
	memcpy(list, methods.ptr(), sizeof(GDExtensionMethodInfo) * size);

	return list;
}

void ScriptInstance::free_method_list(const GDExtensionMethodInfo *p_list) const {
	if (!p_list)
		return;

	// don't ask.
	int size = get_len_from_ptr(p_list);

	for (int i = 0; i < size; i++) {
		const GDExtensionMethodInfo &method = p_list[i];

		memdelete((StringName *)method.name);

		free_prop(method.return_value);

		if (method.argument_count > 0) {
			for (int i = 0; i < method.argument_count; i++)
				free_prop(method.arguments[i]);

			memdelete(method.arguments);
		}

		if (method.default_argument_count > 0)
			memdelete((Variant *)method.default_arguments);
	}

	free_with_len((GDExtensionMethodInfo *)p_list);
}

ScriptLanguage *ScriptInstance::get_language() const {
	return LuauLanguage::get_singleton();
}

/////////////////////
// SCRIPT INSTANCE //
/////////////////////
