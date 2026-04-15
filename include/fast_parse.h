#pragma once

#include <Python.h>
#include <stdbool.h>
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
 * FAST PARSE ENGINE (C23 EDITION)
 * ============================================================================
 * A high-performance, zero-allocation argument parsing system for Python C
 * extensions. Optimized for PEP 703 (Free-threading) and PEP 489 (Multi-phase
 * initialization).
 * ============================================================================
 */

constexpr int FP_EMPTY_SLOT = 0xFFFF;

/** --- 1. TYPES & STRUCTS --- **/

typedef struct {
    const char *name;
    const char *type_name;
    PyObject *interned;
    bool (*convert)(PyObject *, void *);
    PyTypeObject *type_guard;
    bool required;
} FastArgSpec;

typedef struct FastParser FastParser;

typedef bool (*FastParseFunc)(PyObject *const *FP_RESTRICT args, Py_ssize_t nargs,
                              PyObject *FP_RESTRICT kwnames, const FastParser *FP_RESTRICT fp,
                              void *FP_RESTRICT *FP_RESTRICT targets);

struct FastParser {
    const char *parser_name;
    FastArgSpec *specs;
    uint16_t *lookup_table;
    FastParseFunc hot_path;
    size_t count;
    size_t table_mask;
    uint64_t required_mask;
    uint64_t type_guard_mask;
};

// Forward declaration of the generic fallback
[[nodiscard]] static inline bool fp_parse_vector(PyObject *const *FP_RESTRICT args,
                                                 Py_ssize_t nargs, PyObject *FP_RESTRICT kwnames,
                                                 const FastParser *FP_RESTRICT fp,
                                                 void *FP_RESTRICT *FP_RESTRICT targets);

/** --- 2. SPECULATIVE STUBS (Monomorphic fast-paths) --- **/

[[nodiscard]] static inline bool fp_speculate_p0(PyObject *const *FP_RESTRICT args,
                                                 Py_ssize_t nargs, PyObject *FP_RESTRICT kwnames,
                                                 const FastParser *FP_RESTRICT fp,
                                                 void *FP_RESTRICT *FP_RESTRICT targets) {
    if (FP_LIKELY(nargs == 0 && kwnames == nullptr)) {
        return true;
    }
    return fp_parse_vector(args, nargs, kwnames, fp, targets);
}

[[nodiscard]] static inline bool fp_speculate_p1_naked(PyObject *const *FP_RESTRICT args,
                                                       Py_ssize_t nargs,
                                                       PyObject *FP_RESTRICT kwnames,
                                                       const FastParser *FP_RESTRICT fp,
                                                       void *FP_RESTRICT *FP_RESTRICT targets) {
    if (FP_LIKELY(nargs == 1 && kwnames == nullptr)) {
        return fp->specs[0].convert(args[0], targets[0]);
    }
    return fp_parse_vector(args, nargs, kwnames, fp, targets);
}

[[nodiscard]] static inline bool fp_speculate_p2_naked(PyObject *const *FP_RESTRICT args,
                                                       Py_ssize_t nargs,
                                                       PyObject *FP_RESTRICT kwnames,
                                                       const FastParser *FP_RESTRICT fp,
                                                       void *FP_RESTRICT *FP_RESTRICT targets) {
    if (FP_LIKELY(nargs == 2 && kwnames == nullptr)) {
        return (fp->specs[0].convert(args[0], targets[0]) &&
                fp->specs[1].convert(args[1], targets[1])) != 0;
    }
    return fp_parse_vector(args, nargs, kwnames, fp, targets);
}

[[nodiscard]] static inline bool fp_speculate_p3_naked(PyObject *const *FP_RESTRICT args,
                                                       Py_ssize_t nargs,
                                                       PyObject *FP_RESTRICT kwnames,
                                                       const FastParser *FP_RESTRICT fp,
                                                       void *FP_RESTRICT *FP_RESTRICT targets) {
    if (FP_LIKELY(nargs == 3 && kwnames == nullptr)) {
        return (fp->specs[0].convert(args[0], targets[0]) &&
                fp->specs[1].convert(args[1], targets[1]) &&
                fp->specs[2].convert(args[2], targets[2])) != 0;
    }
    return fp_parse_vector(args, nargs, kwnames, fp, targets);
}

[[nodiscard]] static inline bool fp_speculate_p4_naked(PyObject *const *FP_RESTRICT args,
                                                       Py_ssize_t nargs,
                                                       PyObject *FP_RESTRICT kwnames,
                                                       const FastParser *FP_RESTRICT fp,
                                                       void *FP_RESTRICT *FP_RESTRICT targets) {
    if (FP_LIKELY(nargs == 4 && kwnames == nullptr)) {
        return (fp->specs[0].convert(args[0], targets[0]) &&
                fp->specs[1].convert(args[1], targets[1]) &&
                fp->specs[2].convert(args[2], targets[2]) &&
                fp->specs[3].convert(args[3], targets[3])) != 0;
    }
    return fp_parse_vector(args, nargs, kwnames, fp, targets);
}

/** --- 3. CONVERTER DISPATCH --- **/

[[nodiscard]] static inline bool fp_conv_float(PyObject *o, void *t) {
    if (FP_UNLIKELY(o == Py_None)) {
        PyErr_SetString(PyExc_TypeError, "float argument cannot be None");
        return false;
    }
    double v = PyFloat_AsDouble(o);
    if (FP_UNLIKELY(v == -1.0 && PyErr_Occurred())) {
        return false;
    }
    *(float *)t = (float)v;
    return true;
}

[[nodiscard]] static inline bool fp_conv_double(PyObject *o, void *t) {
    if (FP_UNLIKELY(o == Py_None)) {
        PyErr_SetString(PyExc_TypeError, "double argument cannot be None");
        return false;
    }
    double v = PyFloat_AsDouble(o);
    if (FP_UNLIKELY(v == -1.0 && PyErr_Occurred())) {
        return false;
    }
    *(double *)t = v;
    return true;
}

[[nodiscard]] static inline bool fp_conv_int(PyObject *o, void *t) {
    long v = PyLong_AsLong(o);
    if (FP_UNLIKELY(v == -1 && PyErr_Occurred())) {
        return false;
    }
    *(int *)t = (int)v;
    return true;
}

[[nodiscard]] static inline bool fp_conv_u32(PyObject *o, void *t) {
    unsigned long v = PyLong_AsUnsignedLongMask(o);
    if (FP_UNLIKELY(PyErr_Occurred())) {
        return false;
    }
    *(uint32_t *)t = (uint32_t)v;
    return true;
}

[[nodiscard]] static inline bool fp_conv_u64(PyObject *o, void *t) {
    unsigned long long v = PyLong_AsUnsignedLongLong(o);
    if (FP_UNLIKELY(PyErr_Occurred())) {
        return false;
    }
    *(uint64_t *)t = (uint64_t)v;
    return true;
}

[[nodiscard]] static inline bool fp_conv_bool(PyObject *o, void *t) {
    int v = PyObject_IsTrue(o);
    if (FP_UNLIKELY(v == -1)) {
        return false;
    }
    *(bool *)t = (bool)v;
    return true;
}

[[nodiscard]] static inline bool fp_conv_pyobj(PyObject *o, void *t) {
    *(PyObject **)t = o;
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
        bool: fp_conv_bool,                                                                        \
        PyObject *: fp_conv_pyobj FP_CUSTOM_CONVERTERS, /* INJECTION POINT */                       \
        default: ERROR_FastParse_Unsupported_Type                                                  \
    )

#define FP_ARG(name_str, var)                                                                      \
    {.name = (name_str), .convert = FP_GET_CONVERTER((typeof_unqual(var)){0}), .required = false}

#define FP_REQ_ARG(name_str, var)                                                                  \
    {.name = (name_str), .convert = FP_GET_CONVERTER((typeof_unqual(var)){0}), .required = true}

#define FP_ARG_CUSTOM(name_str, conv_func)                                                         \
    {.name = (name_str), .convert = (conv_func), .required = false}

/** --- 4. EXTERN DECLARATIONS --- **/

extern bool fp_report_type_error(const FastParser *fp, size_t index, PyObject *val);
extern bool fp_report_missing(const FastParser *fp, uint64_t provided_mask);
extern bool fp_report_multiple(const FastParser *fp, size_t index);
extern bool fp_report_too_many(const FastParser *fp, Py_ssize_t nargs);
extern void fp_init_impl(FastParser *fp, FastArgSpec *specs, size_t count);
extern void fp_deinit(FastParser *fp);
extern bool fp_parse_legacy(PyObject *args, PyObject *kwargs, PyObject *unused,
                            const FastParser *fp, void **targets);

/** --- 5. THE HOT PATH --- **/

static inline size_t fp_hash_ptr(PyObject *ptr, size_t mask) {
    auto v = (uintptr_t)ptr;
    // Golden ratio multiplier spreads pointer bits effectively
    return ((v * 11400714819323198485ULL) >> 32) & mask;
}

[[nodiscard]] static inline bool fp_parse_vector(PyObject *const *FP_RESTRICT args,
                                                 Py_ssize_t nargs, PyObject *FP_RESTRICT kwnames,
                                                 const FastParser *FP_RESTRICT fp,
                                                 void *FP_RESTRICT *FP_RESTRICT targets) {
    uint64_t provided_mask   = 0;
    const uint64_t tg_mask   = fp->type_guard_mask;
    const size_t count       = fp->count;
    const FastArgSpec *specs = fp->specs;

    // 1. Validate Positional Count
    if (FP_UNLIKELY(nargs > (Py_ssize_t)count)) {
        return fp_report_too_many(fp, nargs);
    }

    // 2. Hot Path: Positional Arguments
    for (Py_ssize_t i = 0; i < nargs; ++i) {
        PyObject *val = args[i];

        if (tg_mask & (1ULL << i)) {
            // Check exact type match first (O(1) pointer comparison)
            if (FP_UNLIKELY(!Py_IS_TYPE(val, specs[i].type_guard))) {
                // Decomposed: Only call expensive Subclass logic if exact match fails
                if (FP_UNLIKELY(!PyObject_TypeCheck(val, specs[i].type_guard))) {
                    return fp_report_type_error(fp, i, val);
                }
            }
        }

        if (FP_UNLIKELY(!specs[i].convert(val, targets[i]))) {
            return false;
        }
    }

    // Branchless mask generation for positionals
    provided_mask = (nargs >= 64) ? ~(uint64_t)0 : ((1ULL << (nargs & 63)) - 1);

    // 3. Keyword Arguments
    if (kwnames) {
        const Py_ssize_t nkw     = PyTuple_GET_SIZE(kwnames);
        PyObject *const *kw_vals = args + nargs;
        const uint16_t *ltable   = fp->lookup_table;
        const size_t t_mask      = fp->table_mask;

        for (Py_ssize_t i = 0; i < nkw; ++i) {
            PyObject *key = PyTuple_GET_ITEM(kwnames, i);
            size_t idx    = FP_EMPTY_SLOT;

            if (FP_LIKELY(ltable)) {
                size_t h         = fp_hash_ptr(key, t_mask);
                size_t candidate = ltable[h];

                // Check for Perfect Hash Hit
                if (FP_LIKELY(candidate != FP_EMPTY_SLOT && specs[candidate].interned == key)) {
                    idx = candidate;
                } else {
                    // Collision resolution (Cold path)
                    while (ltable[h] != FP_EMPTY_SLOT) {
                        if (specs[ltable[h]].interned == key) {
                            idx = ltable[h];
                            break;
                        }
                        h = (h + 1) & t_mask;
                    }
                }
            }

            // Fallback: Missing from table or un-interned keys
            if (FP_UNLIKELY(idx == FP_EMPTY_SLOT)) {
                for (size_t j = 0; j < count; ++j) {
                    if (specs[j].interned == key ||
                        PyUnicode_Compare(key, specs[j].interned) == 0) {
                        idx = j;
                        break;
                    }
                }
                if (FP_UNLIKELY(idx == FP_EMPTY_SLOT)) {
                    PyErr_Format(PyExc_TypeError, "unexpected keyword argument '%U'", key);
                    return false;
                }
            }

            if (FP_UNLIKELY(provided_mask & (1ULL << idx))) {
                return fp_report_multiple(fp, idx);
            }

            PyObject *val = kw_vals[i];

            if (tg_mask & (1ULL << idx)) {
                if (FP_UNLIKELY(!Py_IS_TYPE(val, specs[idx].type_guard))) {
                    if (FP_UNLIKELY(!PyObject_TypeCheck(val, specs[idx].type_guard))) {
                        return fp_report_type_error(fp, idx, val);
                    }
                }
            }

            if (FP_UNLIKELY(!specs[idx].convert(val, targets[idx]))) {
                return false;
            }
            provided_mask |= (1ULL << idx);
        }
    }

    // 4. Final Verification
    if (FP_UNLIKELY((provided_mask & fp->required_mask) != fp->required_mask)) {
        return fp_report_missing(fp, provided_mask);
    }

    return true;
}

/** --- 6. PUBLIC MACROS --- **/

// Triggered if argument 1 is of an unsupported type
extern void ERROR_FastParse_First_Arg_Must_Be_PyObject_Ptr_Or_Vectorcall_Ptr(void);

#define FastParse_Unified(arg1, arg2, arg3, arg4, arg5)                                            \
    _Generic((arg1),                                                                               \
        PyObject *const *: (arg4)->hot_path,                                                       \
        PyObject **: (arg4)->hot_path,                                                             \
        PyObject *: fp_parse_legacy,                                                               \
        default: ERROR_FastParse_First_Arg_Must_Be_PyObject_Ptr_Or_Vectorcall_Ptr)(                \
        (arg1), (arg2), (arg3), (arg4), (arg5))

#define FastParse_Init(fp, specs, count)                                                           \
    do {                                                                                           \
        static_assert((count) <= 64, "FastParse only supports up to 64 arguments");                \
        fp_init_impl(fp, specs, count);                                                            \
    } while (0)