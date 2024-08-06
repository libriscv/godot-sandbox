
#include <gdextension_interface.h>

class ScriptInstance {
protected:
	// allocates list with an int at the front saying how long it is
	template <typename T>
	static T *alloc_with_len(int p_size) {
		uint64_t list_size = sizeof(T) * p_size;
		void *ptr = memalloc(list_size + sizeof(int));

		*((int *)ptr) = p_size;

		return (T *)((int *)ptr + 1);
	}

	static int get_len_from_ptr(const void *p_ptr);
	static void free_with_len(void *p_ptr);

	static void copy_prop(const GDProperty &p_src, GDExtensionPropertyInfo &p_dst);
	static void free_prop(const GDExtensionPropertyInfo &p_prop);

public:
	static void init_script_instance_info_common(GDExtensionScriptInstanceInfo2 &p_info);

	enum PropertySetGetError {
		PROP_OK,
		PROP_NOT_FOUND,
		PROP_WRONG_TYPE,
		PROP_READ_ONLY,
		PROP_WRITE_ONLY,
		PROP_GET_FAILED,
		PROP_SET_FAILED
	};

	virtual bool set(const StringName &p_name, const Variant &p_value, PropertySetGetError *r_err = nullptr) = 0;
	virtual bool get(const StringName &p_name, Variant &r_ret, PropertySetGetError *r_err = nullptr) = 0;

	void get_property_state(GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata);
	void get_property_state(List<Pair<StringName, Variant>> &p_list);

	virtual GDExtensionPropertyInfo *get_property_list(uint32_t *r_count) = 0;
	void free_property_list(const GDExtensionPropertyInfo *p_list) const;
	virtual bool validate_property(GDExtensionPropertyInfo *p_property) const { return false; }

	virtual Variant::Type get_property_type(const StringName &p_name, bool *r_is_valid) const = 0;

	virtual GDExtensionMethodInfo *get_method_list(uint32_t *r_count) const;
	void free_method_list(const GDExtensionMethodInfo *p_list) const;

	virtual bool has_method(const StringName &p_name) const = 0;

	virtual Object *get_owner() const = 0;
	virtual Ref<ELFScript> get_script() const = 0;
	ScriptLanguage *get_language() const;
};
