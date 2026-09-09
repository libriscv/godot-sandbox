#ifndef GODOT_JIT_C_ABI_H
#define GODOT_JIT_C_ABI_H

/* Native, trusted-process ABI. Values are real Godot Variants, never handles.
 * Match the engine's precision when compiling a translation unit externally.
 * All value pointers refer to initialized Variants; outputs may alias inputs.
 * The caller owns arguments/results/globals. Functions own and destroy locals.
 */
typedef signed long long GJInt;
typedef unsigned long long GJUInt;
#ifdef DOUBLE_PRECISION_REAL_T
typedef double GJReal;
#else
typedef float GJReal;
#endif
typedef struct GJVariant {
    unsigned int type;
    unsigned int reserved;
    union {
        GJInt i;
        double f;
        unsigned char b;
        GJReal real[4];
#ifdef DOUBLE_PRECISION_REAL_T
        unsigned char bytes[32];
#else
        unsigned char bytes[16];
#endif
    } data;
} GJVariant;

typedef struct GJContext {
    GJVariant *globals;
    GJVariant *self;
    void *error; /* std::string*, used only by C++ */
    int failed;
    GJVariant *shared_globals; /* optional; null preserves standalone storage */
    void *runtime; /* native ScriptInstance/Callable bridge */
} GJContext;

enum GJOperation {
    GJ_EVALUATE, GJ_CONSTRUCT, GJ_STRING, GJ_CALL, GJ_GET, GJ_SET,
    GJ_GET_NAMED, GJ_SET_NAMED, GJ_ARRAY, GJ_DICTIONARY, GJ_PACKED_ARRAY,
    GJ_UTILITY, GJ_PRINT, GJ_LOAD, GJ_GET_OBJECT, GJ_NEW_OBJECT, GJ_GET_NODE,
    GJ_CALLABLE, GJ_DICTIONARY_HAS, GJ_STRUCT_CHECK, GJ_SUPER_CALL, GJ_CLASS_BIND, GJ_TRAIT_TEST,
    GJ_ARRAY_SIZE, GJ_VECTOR2_NORMALIZED
};

#ifdef __cplusplus
extern "C" {
#endif
void gj_copy(GJVariant *dst, const GJVariant *src);
void gj_destroy(GJVariant *value);
int gj_truth(const GJVariant *value);
/* Returns zero on failure, records a diagnostic; no C++ exception crosses C.
 * args is an ordinary native array of pointers to ordinary native Variants.
 * name is UTF-8; STRING uses detail as its byte length (embedded NUL supported).
 * detail selects an operator/type/print channel, or -1 for a generic operation.
 */
int gj_op(GJContext *ctx, int operation, GJVariant *dst, GJVariant *self,
          const char *name, int detail, const GJVariant *const *args, int count);
int gj_fail(GJContext *ctx, const char *message);
/* Checked loop step: 1 publishes an item, 0 ends iteration, -1 fails.
 * Rechecks the current size each time, so mutations remain visible. */
int gj_array_next(GJContext *ctx, GJVariant *item, GJVariant *array, GJInt index);
typedef int (*GJEntry)(GJContext *, int function, GJVariant *result,
                       const GJVariant *const *args, int count);
#ifdef __cplusplus
}
#endif

/* These are macros deliberately: TinyCC does not inline functions. Generated
 * code passes stable addresses of C locals. Evaluate scalar RHS before clearing
 * the destination, since a destination may also be an operand. */
/* Inline value types match Variant::clear's non-owning type set. */
#define gj_trivial(t) ((t) <= 3 || ((t) < 24 && ((0x90f7efU >> (t)) & 1U)))
#define gj_clear(v) do { \
    if (!gj_trivial((v)->type)) gj_destroy(v); \
    (v)->type = 0; (v)->data.i = 0; \
} while (0)
#define gj_int(v, x) do { \
    GJInt gj_local_i = (x); gj_clear(v); (v)->data.i = gj_local_i; (v)->type = 2; \
} while (0)
#define gj_float(v, x) do { \
    double gj_local_f = (x); gj_clear(v); (v)->data.f = gj_local_f; (v)->type = 3; \
} while (0)
#define gj_bool(v, x) do { \
    int gj_local_b = !!(x); gj_clear(v); (v)->data.b = gj_local_b; (v)->type = 1; \
} while (0)
#define gj_move(d, s) do { \
    if (gj_trivial((d)->type) && gj_trivial((s)->type)) *(d) = *(s); else gj_copy(d, s); \
} while (0)
#define gj_number(v) ((v)->type == 3 ? (v)->data.f : \
    (v)->type == 1 ? (double)(v)->data.b : (double)(v)->data.i)
#define gj_boolean(v) ((v)->type == 0 ? 0 : (v)->type == 1 ? (v)->data.b : \
    (v)->type == 2 ? (v)->data.i != 0 : (v)->type == 3 ? (v)->data.f != 0.0 : gj_truth(v))
#endif
