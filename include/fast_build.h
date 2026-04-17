#pragma once

#include <Python.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- 0. CONFIGURATION & COMPILER COMPATIBILITY --- */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#ifndef _NULLPTR_T
#define _NULLPTR_T
typedef typeof(nullptr) nullptr_t;
#endif
#endif

#ifndef FB_LIKELY
#if defined(__GNUC__) || defined(__clang__)
#define FB_LIKELY(x) __builtin_expect(!!(x), 1)
#define FB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define FB_LIKELY(x) (x)
#define FB_UNLIKELY(x) (x)
#endif
#endif

#ifndef FB_NODISCARD
#define FB_NODISCARD [[nodiscard]]
#endif

#ifndef FB_FORCE_INLINE
#define FB_FORCE_INLINE inline
#endif

/* --- 1. TYPE CONSTRUCTORS (Inlined) --- */

FB_NODISCARD FB_FORCE_INLINE static PyObject *fb_from_float(float v) {
    return PyFloat_FromDouble((double)v);
}
FB_NODISCARD FB_FORCE_INLINE static PyObject *fb_from_double(double v) {
    return PyFloat_FromDouble(v);
}
FB_NODISCARD FB_FORCE_INLINE static PyObject *fb_from_int(int v) {
    return PyLong_FromLong((long)v);
}
FB_NODISCARD FB_FORCE_INLINE static PyObject *fb_from_long(long v) {
    return PyLong_FromLong(v);
}
FB_NODISCARD FB_FORCE_INLINE static PyObject *fb_from_longlong(long long v) {
    return PyLong_FromLongLong(v);
}
FB_NODISCARD FB_FORCE_INLINE static PyObject *fb_from_u32(uint32_t v) {
    return PyLong_FromUnsignedLong((unsigned long)v);
}
FB_NODISCARD FB_FORCE_INLINE static PyObject *fb_from_u64(uint64_t v) {
    return PyLong_FromUnsignedLongLong((unsigned long long)v);
}
FB_NODISCARD FB_FORCE_INLINE static PyObject *fb_from_str(const char *v) {
    return PyUnicode_FromString(v);
}
FB_NODISCARD FB_FORCE_INLINE static PyObject *fb_from_bool(bool v) {
    PyObject *res = (int)v ? Py_True : Py_False;
    Py_INCREF(res);
    return res;
}
FB_NODISCARD FB_FORCE_INLINE static PyObject *fb_incref(PyObject *v) {
    Py_XINCREF(v);
    return v;
}
FB_NODISCARD FB_FORCE_INLINE static PyObject *fb_from_none([[maybe_unused]] nullptr_t v) {
    Py_RETURN_NONE;
}

/* --- 2. THE C23 COMPILE-TIME ROUTER --- */

extern PyObject *FB_UNSUPPORTED_TYPE_PASSED_TO_FASTBUILD(void);

// Allow host projects to inject custom builders
#ifndef FB_CUSTOM_CONVERTERS
#define FB_CUSTOM_CONVERTERS /* empty */
#endif

#define FB_VAL(x)                                                                                  \
    _Generic((x),                                                                                  \
        float: fb_from_float,                                                                      \
        double: fb_from_double,                                                                    \
        int: fb_from_int,                                                                          \
        long: fb_from_long,                                                                        \
        long long: fb_from_longlong,                                                               \
        unsigned int: fb_from_u32,                                                                 \
        unsigned long: PyLong_FromUnsignedLong,                                                    \
        unsigned long long: fb_from_u64,                                                           \
        bool: fb_from_bool,                                                                        \
        char *: fb_from_str,                                                                       \
        const char *: fb_from_str,                                                                 \
        nullptr_t: fb_from_none,                                                                   \
        PyObject *: fb_incref FB_CUSTOM_CONVERTERS,                                                \
        default: FB_UNSUPPORTED_TYPE_PASSED_TO_FASTBUILD)(x)

/* --- 3. THE CONTAINER PACKERS --- */

FB_NODISCARD FB_FORCE_INLINE static PyObject *fb_pack_tuple(size_t n, PyObject **arr) {
    if (FB_UNLIKELY(n == 0)) {
        return PyTuple_New(0);
    }
#pragma unroll 4
    for (size_t i = 0; i < n; i++) {
        if (FB_UNLIKELY(!arr[i])) {
            goto error;
        }
    }

    PyObject *t = PyTuple_New((Py_ssize_t)n);
    if (FB_UNLIKELY(!t)) {
        goto error;
    }
#pragma unroll 4
    for (size_t i = 0; i < n; i++) {
        PyTuple_SET_ITEM(t, i, arr[i]); // Steals reference
    }
    return t;

error:
#pragma unroll 2
    for (size_t i = 0; i < n; i++) {
        Py_XDECREF(arr[i]);
    }
    return nullptr;
}

FB_NODISCARD FB_FORCE_INLINE static PyObject *fb_pack_list(size_t n, PyObject **arr) {
    if (FB_UNLIKELY(n == 0)) {
        return PyList_New(0);
    }
#pragma unroll 4
    for (size_t i = 0; i < n; i++) {
        if (FB_UNLIKELY(!arr[i])) {
            goto error;
        }
    }

    PyObject *l = PyList_New((Py_ssize_t)n);
    if (FB_UNLIKELY(!l)) {
        goto error;
    }
#pragma unroll 4
    for (size_t i = 0; i < n; i++) {
        PyList_SET_ITEM(l, i, arr[i]); // Steals reference
    }
    return l;

error:
#pragma unroll 2
    for (size_t i = 0; i < n; i++) {
        Py_XDECREF(arr[i]);
    }
    return nullptr;
}

FB_NODISCARD FB_FORCE_INLINE static PyObject *fb_pack_dict(size_t n, PyObject **arr) {
    if (FB_UNLIKELY(n == 0)) {
        return PyDict_New();
    }

    // Must be key-value pairs
    if (FB_UNLIKELY(n % 2 != 0)) {
        goto error;
    }
#pragma unroll 4
    for (size_t i = 0; i < n; i++) {
        if (FB_UNLIKELY(!arr[i])) {
            goto error;
        }
    }

    PyObject *d = PyDict_New();
    if (FB_UNLIKELY(!d)) {
        goto error;
    }
#pragma unroll 4
    for (size_t i = 0; i < n; i += 2) {
        if (FB_UNLIKELY(PyDict_SetItem(d, arr[i], arr[i + 1]) < 0)) {
            Py_DECREF(d);
            goto error;
        }
    }
#pragma unroll 4
    for (size_t i = 0; i < n; i++) {
        Py_DECREF(arr[i]);
    }
    return d;

error:
#pragma unroll 2
    for (size_t i = 0; i < n; i++) {
        Py_XDECREF(arr[i]);
    }
    return nullptr;
}

/* --- 4. PREPROCESSOR MAPPING (Up to 16 Args / 8 KV Pairs) --- */

#define FB_EXPAND(x) x

#define FB_NARGS_IMPL(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17,  \
                      _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32,   \
                      N, ...)                                                                      \
    N

#define FB_NARGS(...)                                                                              \
    FB_NARGS_IMPL(__VA_ARGS__, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, \
                  15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)

#define FB_MAP_1(x) FB_VAL(x)
#define FB_MAP_2(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_1(__VA_ARGS__))
#define FB_MAP_3(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_2(__VA_ARGS__))
#define FB_MAP_4(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_3(__VA_ARGS__))
#define FB_MAP_5(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_4(__VA_ARGS__))
#define FB_MAP_6(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_5(__VA_ARGS__))
#define FB_MAP_7(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_6(__VA_ARGS__))
#define FB_MAP_8(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_7(__VA_ARGS__))
#define FB_MAP_9(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_8(__VA_ARGS__))
#define FB_MAP_10(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_9(__VA_ARGS__))
#define FB_MAP_11(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_10(__VA_ARGS__))
#define FB_MAP_12(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_11(__VA_ARGS__))
#define FB_MAP_13(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_12(__VA_ARGS__))
#define FB_MAP_14(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_13(__VA_ARGS__))
#define FB_MAP_15(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_14(__VA_ARGS__))
#define FB_MAP_16(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_15(__VA_ARGS__))
#define FB_MAP_17(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_16(__VA_ARGS__))
#define FB_MAP_18(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_17(__VA_ARGS__))
#define FB_MAP_19(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_18(__VA_ARGS__))
#define FB_MAP_20(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_19(__VA_ARGS__))
#define FB_MAP_21(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_20(__VA_ARGS__))
#define FB_MAP_22(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_21(__VA_ARGS__))
#define FB_MAP_23(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_22(__VA_ARGS__))
#define FB_MAP_24(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_23(__VA_ARGS__))
#define FB_MAP_25(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_24(__VA_ARGS__))
#define FB_MAP_26(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_25(__VA_ARGS__))
#define FB_MAP_27(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_26(__VA_ARGS__))
#define FB_MAP_28(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_27(__VA_ARGS__))
#define FB_MAP_29(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_28(__VA_ARGS__))
#define FB_MAP_30(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_29(__VA_ARGS__))
#define FB_MAP_31(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_30(__VA_ARGS__))
#define FB_MAP_32(x, ...) FB_VAL(x), FB_EXPAND(FB_MAP_31(__VA_ARGS__))

#define FB_CONCAT_IMPL(a, b) a##b
#define FB_CONCAT(a, b) FB_CONCAT_IMPL(a, b)
#define FB_MAP(...) FB_EXPAND(FB_CONCAT(FB_MAP_, FB_NARGS(__VA_ARGS__))(__VA_ARGS__))

/* --- 5. THE PUBLIC API --- */

#define FastKey(parser_ptr, idx) ((parser_ptr)->specs[(idx)].interned)

#define FastBuild_Value(x) FB_VAL(x)

#define FastBuild_Tuple(...)                                                                       \
    fb_pack_tuple(FB_NARGS(__VA_ARGS__), FB_NARGS(__VA_ARGS__)                                     \
                                             ? (PyObject *[]){__VA_OPT__(FB_MAP(__VA_ARGS__))}     \
                                             : nullptr)

#define FastBuild_List(...)                                                                        \
    fb_pack_list(FB_NARGS(__VA_ARGS__), FB_NARGS(__VA_ARGS__)                                      \
                                            ? (PyObject *[]){__VA_OPT__(FB_MAP(__VA_ARGS__))}      \
                                            : nullptr)

#define FastBuild_Dict(...)                                                                        \
    fb_pack_dict(FB_NARGS(__VA_ARGS__), FB_NARGS(__VA_ARGS__)                                      \
                                            ? (PyObject *[]){__VA_OPT__(FB_MAP(__VA_ARGS__))}      \
                                            : nullptr)
