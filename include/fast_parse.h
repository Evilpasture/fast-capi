#pragma once

#include <Python.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#    define FP_LIKELY(x) __builtin_expect(!!(x), 1)
#    define FP_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#    define FP_LIKELY(x) (x)
#    define FP_UNLIKELY(x) (x)
#endif

#if defined(__cplusplus)
#    define FP_RESTRICT __restrict
#else
#    define FP_RESTRICT restrict
#endif

/**
 * ============================================================================
 * FAST PARSE ENGINE
 * ============================================================================
 * A high-performance, zero-allocation argument parsing system for Python C
 * extensions. Optimized for PEP 703 (Free-threading) and PEP 489 (Multi-phase
 * initialization).
 * ============================================================================
 */

static constexpr int FP_EMPTY_SLOT = 0xFFFF;
static constexpr int FP_PAD_       = 7;
/** --- 1. TYPES & STRUCTS --- **/

typedef struct {
    const char *name;
    const char *type_name;
    PyObject *interned;
    PyTypeObject *type_guard;
    bool (*convert)(PyObject *, void *);
    bool required;
    uint8_t _pad[FP_PAD_];
} FastArgSpec;

typedef struct FastParser FastParser;

typedef bool (*FastParseFunc)(PyObject *const *FP_RESTRICT args, Py_ssize_t nargs,
                              PyObject *FP_RESTRICT kwnames,
                              const FastParser *FP_RESTRICT fastparser,
                              void *FP_RESTRICT *FP_RESTRICT targets);

static constexpr auto alignment_size = 64;
struct FastParser {
    alignas(alignment_size) const char *parser_name;
    FastArgSpec *specs;
    uint16_t *lookup_table;
    FastParseFunc hot_path;
    size_t count;
    size_t table_mask;
    uint64_t required_mask;
    uint64_t type_guard_mask;
    bool warned;
};

static_assert(sizeof(struct FastParser) == (size_t)alignment_size * 2);
static_assert(alignof(struct FastParser) == alignment_size);

// Forward declaration of the generic fallback
[[nodiscard]] static inline bool fp_parse_vector(PyObject *const *FP_RESTRICT args,
                                                 Py_ssize_t nargs, PyObject *FP_RESTRICT kwnames,
                                                 const FastParser *FP_RESTRICT fastparser,
                                                 void *FP_RESTRICT *FP_RESTRICT targets);

/** --- 2. SPECULATIVE STUBS (Monomorphic fast-paths) --- **/

enum FastParseIndex : uint8_t {
    FP_IDX_0,
    FP_IDX_1,
    FP_IDX_2,
    FP_IDX_3,
    FP_IDX_4,
    FP_IDX_5,
    FP_IDX_6,
    FP_IDX_7,
    FP_IDX_8
};

// Individual conversion call using the Enum
#define FP_CALL_CONV(n)                                                                            \
    (int)fastparser->specs[FP_IDX_##n].convert(args[FP_IDX_##n], targets[FP_IDX_##n])

// Stub Function Boilerplate
#define FP_GEN_STUB(N, ...)                                                                        \
    [[nodiscard]] static inline bool fp_speculate_p##N##_naked(                                    \
        PyObject *const *FP_RESTRICT args, Py_ssize_t nargs, PyObject *FP_RESTRICT kwnames,        \
        const FastParser *FP_RESTRICT fastparser, void *FP_RESTRICT *FP_RESTRICT targets) {        \
        if (FP_LIKELY(nargs == N && kwnames == nullptr)) {                                         \
            return (__VA_ARGS__) != 0;                                                             \
        }                                                                                          \
        return fp_parse_vector(args, nargs, kwnames, fastparser, targets);                         \
    }

// 0 Args (Special case)
[[nodiscard]] static inline bool fp_speculate_p0(PyObject *const *FP_RESTRICT args,
                                                 Py_ssize_t nargs, PyObject *FP_RESTRICT kwnames,
                                                 const FastParser *FP_RESTRICT fastparser,
                                                 void *FP_RESTRICT *FP_RESTRICT targets) {
    if (FP_LIKELY(nargs == 0 && kwnames == nullptr)) {
        return true;
    }
    return fp_parse_vector(args, nargs, kwnames, fastparser, targets);
}

// Generate stubs 1 through 8
FP_GEN_STUB(1, FP_CALL_CONV(0))

FP_GEN_STUB(2, FP_CALL_CONV(0) && FP_CALL_CONV(1))

FP_GEN_STUB(3, FP_CALL_CONV(0) && FP_CALL_CONV(1) && FP_CALL_CONV(2))

FP_GEN_STUB(4, FP_CALL_CONV(0) && FP_CALL_CONV(1) && FP_CALL_CONV(2) && FP_CALL_CONV(3))

FP_GEN_STUB(5, FP_CALL_CONV(0) && FP_CALL_CONV(1) && FP_CALL_CONV(2) && FP_CALL_CONV(3) &&
                   FP_CALL_CONV(4))

FP_GEN_STUB(6, FP_CALL_CONV(0) && FP_CALL_CONV(1) && FP_CALL_CONV(2) && FP_CALL_CONV(3) &&
                   FP_CALL_CONV(4) && FP_CALL_CONV(5))

FP_GEN_STUB(7, FP_CALL_CONV(0) && FP_CALL_CONV(1) && FP_CALL_CONV(2) && FP_CALL_CONV(3) &&
                   FP_CALL_CONV(4) && FP_CALL_CONV(5) && FP_CALL_CONV(6))

FP_GEN_STUB(8, FP_CALL_CONV(0) && FP_CALL_CONV(1) && FP_CALL_CONV(2) && FP_CALL_CONV(3) &&
                   FP_CALL_CONV(4) && FP_CALL_CONV(5) && FP_CALL_CONV(6) && FP_CALL_CONV(7))

/** --- 3. CONVERTER DISPATCH --- **/

[[nodiscard]] static inline bool fp_conv_float(PyObject *obj, void *target) {
    // HOT PATH: Exact float match (Pointer comparison)
    if (PyFloat_CheckExact(obj)) {
        *(float *)target = (float)((PyFloatObject *)obj)->ob_fval;
        return true;
    }

    // COLD PATH: Handle ints or float subclasses
    double val = PyFloat_AsDouble(obj);
    if (FP_UNLIKELY(val == -1.0 && PyErr_Occurred())) {
        return false;
    }
    *(float *)target = (float)val;
    return true;
}

[[nodiscard]] static inline bool fp_conv_double(PyObject *obj, void *target) {
    // HOT PATH: Exact float match
    // Bypasses the overhead of PyFloat_AsDouble (magic method checks, int conversion logic)
    if (PyFloat_CheckExact(obj)) {
        *(double *)target = ((PyFloatObject *)obj)->ob_fval;
        return true;
    }

    // COLD PATH: Handle ints, None, or float subclasses
    if (FP_UNLIKELY(obj == Py_None)) {
        PyErr_SetString(PyExc_TypeError, "double argument cannot be None");
        return false;
    }

    double val = PyFloat_AsDouble(obj);
    if (FP_UNLIKELY(val == -1.0 && PyErr_Occurred())) {
        return false;
    }
    *(double *)target = val;
    return true;
}

[[nodiscard]] static inline bool fp_conv_int(PyObject *obj, void *target) {
    // 1. Singletons (Pointer identity is the fastest possible check)
    if (obj == Py_True) {
        *(int *)target = 1;
        return true;
    }
    if (obj == Py_False) {
        *(int *)target = 0;
        return true;
    }

    // 2. None Check (Cold path, but keeps PyLong_AsLong clean)
    if (FP_UNLIKELY(obj == Py_None)) {
        PyErr_SetString(PyExc_TypeError, "int argument cannot be None");
        return false;
    }

    // 3. Single Conversion Call
    long val = PyLong_AsLong(obj);

    // 4. Hot Path: 99.9% of integers in physics/logic are not -1
    if (FP_LIKELY(val != -1)) {
        *(int *)target = (int)val;
        return true;
    }

    // 5. Cold Path: Was it actually -1 or a conversion error?
    if (PyErr_Occurred()) {
        return false;
    }

    *(int *)target = -1;
    return true;
}

[[nodiscard]] static inline bool fp_conv_u32(PyObject *obj, void *target) {
    if (obj == Py_True) {
        *(uint32_t *)target = 1;
        return true;
    }
    if (obj == Py_False) {
        *(uint32_t *)target = 0;
        return true;
    }
    if (FP_UNLIKELY(obj == Py_None)) {
        PyErr_SetString(PyExc_TypeError, "uint32 argument cannot be None");
        return false;
    }

    unsigned long val = PyLong_AsUnsignedLong(obj);

    if (FP_LIKELY(val != (unsigned long)-1)) {
        *(uint32_t *)target = (uint32_t)val;
        return true;
    }

    if (PyErr_Occurred()) {
        return false;
    }

    *(uint32_t *)target = (uint32_t)-1;
    return true;
}

[[nodiscard]] static inline bool fp_conv_u64(PyObject *obj, void *target) {
    if (obj == Py_True) {
        *(uint64_t *)target = 1;
        return true;
    }
    if (obj == Py_False) {
        *(uint64_t *)target = 0;
        return true;
    }
    // u64 often used for handles, which might be None
    if (obj == Py_None) {
        *(uint64_t *)target = 0;
        return true;
    }

    unsigned long long val = PyLong_AsUnsignedLongLong(obj);

    if (FP_LIKELY(val != (unsigned long long)-1)) {
        *(uint64_t *)target = (uint64_t)val;
        return true;
    }

    if (PyErr_Occurred()) {
        return false;
    }

    *(uint64_t *)target = (uint64_t)-1;
    return true;
}

[[nodiscard]] static inline bool fp_conv_bool(PyObject *obj, void *target) {
    // Exact pointer check for the singletons
    if (obj == Py_True) {
        *(bool *)target = true;
        return true;
    }
    if (obj == Py_False) {
        *(bool *)target = false;
        return true;
    }

    // Fallback for truthiness of lists, strings, etc.
    int val = PyObject_IsTrue(obj);
    if (FP_UNLIKELY(val == -1)) {
        return false;
    }
    *(bool *)target = (bool)val;
    return true;
}

[[nodiscard]] static inline bool fp_conv_pyobj(PyObject *obj, void *target) {
    *(PyObject **)target = obj;
    return true;
}

[[nodiscard]] static inline bool fp_conv_str(PyObject *obj, void *target) {
    if (FP_UNLIKELY(!PyUnicode_Check(obj))) {
        PyErr_Format(PyExc_TypeError, "expected str, got %s", Py_TYPE(obj)->tp_name);
        return false;
    }
    const char *str = PyUnicode_AsUTF8(obj);
    if (FP_UNLIKELY(str == nullptr)) {
        return false;
    }
    *(const char **)target = str;
    return true;
}

[[nodiscard]] static inline bool fp_conv_ssize(PyObject *obj, void *target) {
    // HOT PATH: Exact integer match
    if (FP_LIKELY(PyLong_CheckExact(obj))) {
        Py_ssize_t val = PyLong_AsSsize_t(obj);
        // Optimization: Values like buffer sizes and counts are rarely exactly -1.
        if (FP_LIKELY(val != -1)) {
            *(Py_ssize_t *)target = val;
            return true;
        }
    }

    // COLD PATH: Handle None, subclasses, or the literal value -1
    if (obj == Py_None) {
        PyErr_SetString(PyExc_TypeError, "size/index argument cannot be None");
        return false;
    }

    Py_ssize_t val = PyLong_AsSsize_t(obj);
    if (val == -1 && PyErr_Occurred()) {
        return false;
    }
    *(Py_ssize_t *)target = val;
    return true;
}

// Allow host projects to inject custom types
#ifndef FP_CUSTOM_CONVERTERS
#    define FP_CUSTOM_CONVERTERS /* empty */
#endif

extern void ERROR_FastParse_Unsupported_Type(void);

#define FP_GET_CONVERTER(T)                                                                        \
    _Generic((T),                                                                                  \
        float: fp_conv_float,                                                                      \
        double: fp_conv_double,                                                                    \
        int: fp_conv_int,                                                                          \
        uint32_t: fp_conv_u32,                                                                     \
        uint64_t: fp_conv_u64,                                                                     \
        Py_ssize_t: fp_conv_ssize,                                                                 \
        bool: fp_conv_bool,                                                                        \
        const char *: fp_conv_str,                                                                 \
        PyObject *: fp_conv_pyobj FP_CUSTOM_CONVERTERS, /* INJECTION POINT */                      \
        default: ERROR_FastParse_Unsupported_Type)

#ifndef FP_CUSTOM_TYPE_NAMES
#    define FP_CUSTOM_TYPE_NAMES /* empty */
#endif

#define FP_GET_TYPE_NAME(T)                                                                        \
    _Generic((T),                                                                                  \
        float: "float",                                                                            \
        double: "float",                                                                           \
        int: "int",                                                                                \
        uint32_t: "int",                                                                           \
        uint64_t: "int",                                                                           \
        Py_ssize_t: "int",                                                                         \
        bool: "bool",                                                                              \
        const char *: "str",                                                                       \
        PyObject *: "object" FP_CUSTOM_TYPE_NAMES, /* INJECTION POINT */                           \
        default: "any")

#define FP_ARG(name_str, var)                                                                      \
    {.name      = (name_str),                                                                      \
     .type_name = FP_GET_TYPE_NAME((typeof_unqual(var)){}),                                        \
     .convert   = FP_GET_CONVERTER((typeof_unqual(var)){}),                                        \
     .required  = false}

#define FP_REQ_ARG(name_str, var)                                                                  \
    {.name      = (name_str),                                                                      \
     .type_name = FP_GET_TYPE_NAME((typeof_unqual(var)){}),                                        \
     .convert   = FP_GET_CONVERTER((typeof_unqual(var)){}),                                        \
     .required  = true}

#define FP_ARG_CUSTOM(name_str, conv_func)                                                         \
    {.name = (name_str), .convert = (conv_func), .required = false}

/** --- 4. EXTERN DECLARATIONS --- **/

extern bool fp_report_type_error(const FastParser *fastparser, size_t index, PyObject *val);
extern bool fp_report_missing(const FastParser *fastparser, uint64_t provided_mask);
extern bool fp_report_multiple(const FastParser *fastparser, size_t index);
extern bool fp_report_too_many(const FastParser *fastparser, Py_ssize_t nargs);
extern bool fp_report_unknown_keyword(const FastParser *fastparser, PyObject *key);
extern void fp_init_impl(FastParser *fastparser, FastArgSpec *specs, size_t count);
extern void fp_deinit(FastParser *fastparser);
extern bool fp_parse_legacy(PyObject *args, PyObject *kwargs, [[maybe_unused]] PyObject *unused,
                            const FastParser *FP_RESTRICT fastparser,
                            void *FP_RESTRICT *FP_RESTRICT targets);

/** --- 5. THE HOT PATH --- **/
[[gnu::always_inline, gnu::const]]
static inline size_t fp_hash_ptr(PyObject *ptr, size_t mask) {
    auto val = (uintptr_t)ptr;
    // Golden ratio multiplier spreads pointer bits effectively
    constexpr auto gratio     = 11400714819323198485ULL;
    constexpr auto shift_bits = 32ULL;
    return ((val * gratio) >> shift_bits) & mask;
}
[[gnu::always_inline]]
static inline bool fp_check_type_guard(const FastArgSpec *FP_RESTRICT spec,
                                       PyObject *FP_RESTRICT val, uint64_t tg_mask, size_t idx) {
    if (!(tg_mask & (1ULL << idx))) {
        return true;
    }

    // Fast path: Exact pointer match
    if (Py_IS_TYPE(val, spec->type_guard)) {
        return true;
    }

    // Slow path: Full inheritance check
    return PyObject_TypeCheck(val, spec->type_guard);
}
[[gnu::always_inline]]
static inline size_t fp_find_keyword_index(PyObject *FP_RESTRICT key,
                                           const FastParser *FP_RESTRICT fastparse) {
    const uint16_t *ltable = fastparse->lookup_table;
    size_t found_idx       = FP_EMPTY_SLOT;

    // 1. Try Hash Table if it exists
    if (FP_LIKELY(ltable != nullptr)) {
        size_t hash = fp_hash_ptr(key, fastparse->table_mask);
#pragma unroll 2
        while (ltable[hash] != FP_EMPTY_SLOT) {
            size_t candidate = ltable[hash];
            if (fastparse->specs[candidate].interned == key) {
                return candidate;
            }
            // Performance hit: matching string content but different pointers
            if (FP_UNLIKELY(PyUnicode_Compare(key, fastparse->specs[candidate].interned) == 0)) {
                found_idx = candidate;
                goto trigger_warning;
            }
            hash = (hash + 1) & fastparse->table_mask;
        }
    }

    // 2. Fallback: Linear search (Crucial for small arg counts < 8)
#pragma unroll 4
    for (size_t j = 0; j < fastparse->count; ++j) {
        if (fastparse->specs[j].interned == key) {
            return j;
        }
        if (FP_UNLIKELY(PyUnicode_Compare(key, fastparse->specs[j].interned) == 0)) {
            found_idx = j;
            goto trigger_warning;
        }
    }

    return FP_EMPTY_SLOT;

trigger_warning: {
    // I don't think I want to make this an atomic operation considering that parsing is almost
    // always single-threaded. Avoids header bloat. If it's a race somehow, no big deal.
    if (FP_UNLIKELY(fastparse->warned == false)) {
        // UB. It's just a bool...
        bool *warn_ptr = (bool *)&fastparse->warned;
        *warn_ptr      = true;
        // If PyErr_WarnFormat returns < 0, a Warning was upgraded to an Error.
        // We return FP_EMPTY_SLOT to allow the caller to abort and propagate the error.
        if (PyErr_WarnFormat(PyExc_RuntimeWarning, 1,
                             "Priority Alert: Specific keyword '%U' in parser implementation '%s' "
                             "is not interned. \n"
                             "This condition mandates a slow string comparison and degrades "
                             "performance significantly. \n"
                             "Sanitize the environment by interning strings manually with "
                             "sys.intern() immediately.",
                             key,
                             fastparse->parser_name ? fastparse->parser_name : "unknown") < 0) {
            return FP_EMPTY_SLOT;
        }
    }
}
    return found_idx;
}
[[gnu::always_inline]]
static inline bool fp_process_pos(const FastParser *FP_RESTRICT fastparse,
                                  PyObject *const *FP_RESTRICT args, Py_ssize_t nargs,
                                  void *FP_RESTRICT *FP_RESTRICT targets) {
#pragma unroll 2
    for (Py_ssize_t i = 0; i < nargs; ++i) {
        if (!fp_check_type_guard(&fastparse->specs[i], args[i], fastparse->type_guard_mask, i)) {
            return fp_report_type_error(fastparse, i, args[i]);
        }
        if (FP_UNLIKELY(!fastparse->specs[i].convert(args[i], targets[i]))) {
            return false;
        }
    }
    return true;
}
[[gnu::always_inline]]
static inline bool fp_process_kw(const FastParser *FP_RESTRICT fastparse,
                                 PyObject *const *FP_RESTRICT args, Py_ssize_t nargs,
                                 PyObject *FP_RESTRICT kwnames, uint64_t *FP_RESTRICT mask,
                                 void *FP_RESTRICT *FP_RESTRICT targets) {
    if (!kwnames) {
        return true;
    }

    const Py_ssize_t nkw     = PyTuple_GET_SIZE(kwnames);
    PyObject *const *kw_vals = args + nargs;

#pragma unroll 2
    for (Py_ssize_t i = 0; i < nkw; ++i) {
        PyObject *key = PyTuple_GET_ITEM(kwnames, i);
        size_t idx    = fp_find_keyword_index(key, fastparse);

        if (FP_UNLIKELY(idx == FP_EMPTY_SLOT)) {
            return fp_report_unknown_keyword(fastparse, key);
        }

        if (FP_UNLIKELY(*mask & (1ULL << idx))) {
            return fp_report_multiple(fastparse, idx);
        }

        if (!fp_check_type_guard(&fastparse->specs[idx], kw_vals[i], fastparse->type_guard_mask,
                                 idx)) {
            return fp_report_type_error(fastparse, idx, kw_vals[i]);
        }

        if (FP_UNLIKELY(!fastparse->specs[idx].convert(kw_vals[i], targets[idx]))) {
            return false;
        }
        *mask |= (1ULL << idx);
    }
    return true;
}

[[gnu::always_inline, gnu::const]]
static inline uint64_t fp_make_mask(size_t n) {
    if (n == 0) {
        return 0;
    }
    static constexpr uint64_t FULL_BITS = 64;
    if (n >= FULL_BITS) {
        return ~(uint64_t)0;
    }
    return (1ULL << n) - 1;
}

// unadulterated speed!!!
[[nodiscard, gnu::always_inline, gnu::flatten, gnu::hot, gnu::no_stack_protector,
  gnu::nonnull(1, 4, 5)]] static inline bool
fp_parse_vector(PyObject *const *FP_RESTRICT args, Py_ssize_t nargs, PyObject *FP_RESTRICT kwnames,
                const FastParser *FP_RESTRICT fastparse, void *FP_RESTRICT *FP_RESTRICT targets) {
    // 1. Pre-flight Check
    if (FP_UNLIKELY(nargs > (Py_ssize_t)fastparse->count)) {
        return fp_report_too_many(fastparse, nargs);
    }

    // 2. Handle Positionals
    if (FP_UNLIKELY(!fp_process_pos(fastparse, args, nargs, targets))) {
        return false;
    }

    // 3. Setup Mask & Handle Keywords

    uint64_t mask = fp_make_mask(nargs);
    if (FP_UNLIKELY(!fp_process_kw(fastparse, args, nargs, kwnames, &mask, targets))) {
        return false;
    }

    // 4. Final Verification
    if (FP_UNLIKELY((mask & fastparse->required_mask) != fastparse->required_mask)) {
        return fp_report_missing(fastparse, mask);
    }

    return true;
}

/** --- 6. PUBLIC MACROS --- **/

// Triggered if argument 1 is of an unsupported type
extern void ERROR_FastParse_First_Arg_Must_Be_PyObject_Ptr_Or_Vectorcall_Ptr(void);
// NOLINTNEXTLINE(readability-identifier-naming)
#define FastParse_Unified(arg1, arg2, arg3, arg4, arg5)                                            \
    _Generic((arg1),                                                                               \
        PyObject *const *: (arg4)->hot_path,                                                       \
        PyObject **: (arg4)->hot_path,                                                             \
        PyObject *: fp_parse_legacy,                                                               \
        default: ERROR_FastParse_First_Arg_Must_Be_PyObject_Ptr_Or_Vectorcall_Ptr)(                \
        (arg1), (arg2), (arg3), (arg4), (arg5))
// NOLINTNEXTLINE(readability-identifier-naming)
#define FastParse_Init(fastparser, specs, count)                                                   \
    do {                                                                                           \
        static_assert((count) <= 64, "FastParse only supports up to 64 arguments");                \
        fp_init_impl(fastparser, specs, count);                                                    \
    } while (false)