#ifndef GODOT_JIT_C_ABI_H
#define GODOT_JIT_C_ABI_H

/* Native, trusted-process ABI. Values are real Godot Variants, never handles.
 * Match the engine's precision when compiling a translation unit externally.
 * All value pointers refer to initialized Variants. Outputs may alias inputs.
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
        int integer[4];
        float color[4];
#ifdef DOUBLE_PRECISION_REAL_T
        GJUInt words[4];
        unsigned char bytes[32];
#else
        GJUInt words[2];
        unsigned char bytes[16];
#endif
    } data;
} GJVariant;

/* Stack-owned debug views, valid until the matching EXIT event. */
typedef struct GJDebugFrame {
    int function;
    int line;
    int instruction;
    const GJVariant *const *locals;
    int local_count;
    const GJVariant *self;
} GJDebugFrame;
enum GJDebugEvent { GJ_DEBUG_ENTER, GJ_DEBUG_EXIT, GJ_DEBUG_LINE, GJ_DEBUG_BREAKPOINT, GJ_DEBUG_ERROR };
struct GJContext;
typedef void (*GJDebugHook)(struct GJContext *, GJDebugFrame *, int event);

typedef struct GJContext {
    GJVariant *globals;
    GJVariant *self;
    void *error; /* std::string*, used only by C++ */
    int failed;
    GJVariant *shared_globals; /* optional; null preserves standalone storage */
    void *runtime; /* native ScriptInstance/Callable bridge */
    GJDebugHook debug; /* optional; absent in non-debug generated code */
    void *resuming; /* private suspended frame, consumed by coroutine entry */
} GJContext;

enum GJOperation {
    GJ_EVALUATE, GJ_CONSTRUCT, GJ_STRING, GJ_CALL, GJ_GET, GJ_SET,
    GJ_GET_NAMED, GJ_SET_NAMED, GJ_ARRAY, GJ_DICTIONARY, GJ_PACKED_ARRAY,
    GJ_UTILITY, GJ_PRINT, GJ_LOAD, GJ_GET_OBJECT, GJ_NEW_OBJECT, GJ_GET_NODE,
    GJ_CALLABLE, GJ_DICTIONARY_HAS, GJ_STRUCT_CHECK, GJ_SUPER_CALL, GJ_CLASS_BIND, GJ_TRAIT_TEST,
    GJ_ARRAY_SIZE, GJ_VECTOR2_NORMALIZED, GJ_COERCE
};

#ifdef __cplusplus
extern "C" {
#endif
/* Keep most math function in the engine for platform determinism. */
#ifdef __cplusplus
typedef bool GJMathBool;
#else
typedef _Bool GJMathBool;
#endif
#define GJ_MATH_ARGS_1 double
#define GJ_MATH_ARGS_2 double, double
#define GJ_MATH_ARGS_3 double, double, double
#define GJ_MATH_ARGS_5 double, double, double, double, double
#define GJ_MATH_ARGS_8 double, double, double, double, double, double, double, double
#define GJ_ENGINE_MATH(X) \
    X(SIN, sin, double, 1) \
    X(COS, cos, double, 1) \
    X(TAN, tan, double, 1) \
    X(ASIN, asin, double, 1) \
    X(ACOS, acos, double, 1) \
    X(ATAN, atan, double, 1) \
    X(SINH, sinh, double, 1) \
    X(COSH, cosh, double, 1) \
    X(TANH, tanh, double, 1) \
    X(ASINH, asinh, double, 1) \
    X(ACOSH, acosh, double, 1) \
    X(ATANH, atanh, double, 1) \
    X(EXP, exp, double, 1) \
    X(LOG, log, double, 1) \
    X(SQRT, sqrt, double, 1) \
    X(FLOORF, floorf, double, 1) \
    X(CEILF, ceilf, double, 1) \
    X(ROUNDF, roundf, double, 1) \
    X(ABSF, absf, double, 1) \
    X(SIGNF, signf, double, 1) \
    X(DEG_TO_RAD, deg_to_rad, double, 1) \
    X(RAD_TO_DEG, rad_to_deg, double, 1) \
    X(LINEAR_TO_DB, linear_to_db, double, 1) \
    X(DB_TO_LINEAR, db_to_linear, double, 1) \
    X(ATAN2, atan2, double, 2) \
    X(POW, pow, double, 2) \
    X(FMOD, fmod, double, 2) \
    X(FPOSMOD, fposmod, double, 2) \
    X(SNAPPEDF, snappedf, double, 2) \
    X(ANGLE_DIFFERENCE, angle_difference, double, 2) \
    X(PINGPONG, pingpong, double, 2) \
    X(EASE, ease, double, 2) \
    X(LERPF, lerpf, double, 3) \
    X(INVERSE_LERP, inverse_lerp, double, 3) \
    X(SMOOTHSTEP, smoothstep, double, 3) \
    X(MOVE_TOWARD, move_toward, double, 3) \
    X(LERP_ANGLE, lerp_angle, double, 3) \
    X(ROTATE_TOWARD, rotate_toward, double, 3) \
    X(WRAPF, wrapf, double, 3) \
    X(REMAP, remap, double, 5) \
    X(CUBIC_INTERPOLATE, cubic_interpolate, double, 5) \
    X(CUBIC_INTERPOLATE_ANGLE, cubic_interpolate_angle, double, 5) \
    X(BEZIER_INTERPOLATE, bezier_interpolate, double, 5) \
    X(BEZIER_DERIVATIVE, bezier_derivative, double, 5) \
    X(CUBIC_INTERPOLATE_IN_TIME, cubic_interpolate_in_time, double, 8) \
    X(CUBIC_INTERPOLATE_ANGLE_IN_TIME, cubic_interpolate_angle_in_time, double, 8) \
    X(IS_NAN, is_nan, GJMathBool, 1) \
    X(IS_INF, is_inf, GJMathBool, 1) \
    X(IS_FINITE, is_finite, GJMathBool, 1) \
    X(IS_ZERO_APPROX, is_zero_approx, GJMathBool, 1) \
    X(IS_EQUAL_APPROX, is_equal_approx, GJMathBool, 2) \
    X(FLOORI, floori, GJInt, 1) \
    X(CEILI, ceili, GJInt, 1) \
    X(ROUNDI, roundi, GJInt, 1) \
    X(STEP_DECIMALS, step_decimals, GJInt, 1)
#define GJ_DECLARE_MATH(id, name, result, count) result gj_math_##name(GJ_MATH_ARGS_##count);
GJ_ENGINE_MATH(GJ_DECLARE_MATH)
#undef GJ_DECLARE_MATH
GJReal gj_sqrt_real(GJReal value);
/* Godot 4.6+ typed built-in entry points, resolved once by the native host. */
#define GJ_VECTOR_NORMALIZE(X) \
    X(2, 5, 2428350749) \
    X(3, 9, 1776574132) \
    X(4, 12, 80860099)
#define GJ_DECLARE_NORMALIZE(n, type, hash) \
    void gj_vector##n##_normalized(void *self, const void **args, void *result, int count);
GJ_VECTOR_NORMALIZE(GJ_DECLARE_NORMALIZE)
#undef GJ_DECLARE_NORMALIZE

void gj_copy(GJVariant *dst, const GJVariant *src);
void gj_destroy(GJVariant *value);
int gj_truth(const GJVariant *value);
/* Returns zero on failure, records a diagnostic; no C++ exception crosses C.
 * args is an ordinary native array of pointers to ordinary native Variants.
 * name_id indexes the module gj_names table; -1 means absent. STRING uses
 * detail as its byte length (embedded NUL supported).
 * detail selects an operator/type/print channel, or -1 for a generic operation.
 */
int gj_op(GJContext *ctx, int operation, GJVariant *dst, GJVariant *self,
          int name_id, int detail, const GJVariant *const *args, int count);
int gj_fail(GJContext *ctx, const char *message);
/* Checked loop step: 1 publishes an item, 0 ends iteration, -1 fails.
 * Rechecks the current size each time, so mutations remain visible. */
int gj_array_next(GJContext *ctx, GJVariant *item, GJVariant *array, GJInt index);
/* Proven Array, typed inline vector destination. Rechecks size each step. */
int gj_array_next_vector(GJContext *, GJReal *item, GJVariant *array, GJInt index, int type);
/* Await returns 1 on suspension, 0 immediately, -1 on failure. Restore
 * returns the instruction to resume, or -1 on failure. Slots are owned locals. */
int gj_await(GJContext *, void *resuming, int function, int instruction,
             GJVariant *result, const GJVariant *operand, GJVariant *const *slots,
             int count, int destination);
int gj_await_restore(GJContext *, void *resuming, GJVariant *const *slots, int count);
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
/* Transfer only an owned value proven dead on every successor edge. */
#define gj_take(d, s) do { \
    if ((d) != (s)) { gj_clear(d); *(d) = *(s); (s)->type = 0; (s)->data.i = 0; } \
} while (0)
#define gj_number(v) ((v)->type == 3 ? (v)->data.f : \
    (v)->type == 1 ? (double)(v)->data.b : (double)(v)->data.i)
#define gj_boolean(v) ((v)->type == 0 ? 0 : (v)->type == 1 ? (v)->data.b : \
    (v)->type == 2 ? (v)->data.i != 0 : (v)->type == 3 ? (v)->data.f != 0.0 : gj_truth(v))
#endif
