#include "fast_parse.h"
#include <stdlib.h>

static const FastParseFunc MONO_STUBS[] = {fp_speculate_p0, fp_speculate_p1_naked,
                                           fp_speculate_p2_naked, fp_speculate_p3_naked,
                                           fp_speculate_p4_naked};

static_assert(sizeof(MONO_STUBS) / sizeof(FastParseFunc) == 5,
              "MONO_STUBS table must contain exactly 5 stubs (0-4 args)");

bool fp_report_missing(const FastParser *fp, uint64_t provided_mask) {
#pragma unroll 2
    for (size_t i = 0; i < fp->count; i++) {
        if (fp->specs[i].required && !(provided_mask & (1ULL << i))) {
            PyErr_Format(PyExc_TypeError, "required argument '%s' missing", fp->specs[i].name);
            return false;
        }
    }
    return false;
}

bool fp_report_type_error(const FastParser *fp, size_t index, PyObject *val) {
    const FastArgSpec *spec = &fp->specs[index];
    const char *expected_type = spec->type_name
                                    ? spec->type_name
                                    : (spec->type_guard ? spec->type_guard->tp_name : "unknown");

    PyErr_Format(PyExc_TypeError, "argument '%s' must be %s, not %.200s", spec->name, expected_type,
                 Py_TYPE(val)->tp_name);
    return false;
}

bool fp_report_multiple(const FastParser *fp, size_t index) {
    PyErr_Format(PyExc_TypeError, "argument '%s' got multiple values", fp->specs[index].name);
    return false;
}

bool fp_report_too_many(const FastParser *fp, Py_ssize_t nargs) {
    PyErr_Format(PyExc_TypeError, "too many positional arguments (expected %zu, got %zd)",
                 fp->count, nargs);
    return false;
}

void fp_init_impl(FastParser *fp, FastArgSpec *specs, size_t count) {
    if (count > 64) {
        Py_FatalError("FastParse: Argument count exceeds 64.");
    }

    fp->specs = specs;
    fp->count = count;
    fp->required_mask = 0;
    fp->type_guard_mask = 0;
    fp->lookup_table = nullptr;
#pragma unroll 2
    for (size_t i = 0; i < count; i++) {
        if (specs[i].name) {
            specs[i].interned = PyUnicode_InternFromString(specs[i].name);
        }
        if (specs[i].required) {
            fp->required_mask |= (1ULL << i);
        }
        if (specs[i].type_guard) {
            fp->type_guard_mask |= (1ULL << i);
        }
    }

    if (count > 8) { // Threshold for using hash table
        size_t table_size = 1;
#pragma unroll 2
        while (table_size < (count * 2)) {
            table_size <<= 1;
        }

        fp->table_mask = table_size - 1;
        fp->lookup_table = (uint16_t *)malloc(table_size * sizeof(uint16_t));
        if (!fp->lookup_table) {
            Py_FatalError("FastParse: Failed to allocate lookup table.");
        }
#pragma unroll 2
        for (size_t i = 0; i < table_size; i++) {
            fp->lookup_table[i] = FP_EMPTY_SLOT;
        }

        for (size_t i = 0; i < count; i++) {
            size_t h = fp_hash_ptr(fp->specs[i].interned, fp->table_mask);
#pragma unroll 2
            while (fp->lookup_table[h] != FP_EMPTY_SLOT) {
                h = (h + 1) & fp->table_mask;
            }
            fp->lookup_table[h] = (uint16_t)i;
        }
    }

    fp->hot_path = fp_parse_vector;

    uint64_t all_required = (count >= 64) ? ~0ULL : ((1ULL << count) - 1);
    if (fp->type_guard_mask == 0 && fp->required_mask == all_required) {
        if (count < (sizeof(MONO_STUBS) / sizeof(FastParseFunc))) {
            fp->hot_path = MONO_STUBS[count];
        }
    }
}

void fp_deinit(FastParser *fp) {
    if (!fp) {
        return;
    }

    if (fp->specs) {
#pragma unroll 2
        for (size_t i = 0; i < fp->count; i++) {
            Py_XDECREF(fp->specs[i].interned);
            fp->specs[i].interned = nullptr;
        }
    }

    if (fp->lookup_table) {
        free(fp->lookup_table);
        fp->lookup_table = nullptr;
    }
}

[[nodiscard]] bool fp_parse_legacy(PyObject *args, PyObject *kwargs, PyObject *unused,
                                   const FastParser *fp, void **targets) {
    (void)unused;
    uint64_t provided_mask = 0;
    const size_t count = fp->count;
    const FastArgSpec *specs = fp->specs;

    if (args) {
        Py_ssize_t nargs = PyTuple_GET_SIZE(args);
        if (FP_UNLIKELY(nargs > (Py_ssize_t)count)) {
            return fp_report_too_many(fp, nargs);
        }
#pragma unroll 2
        for (Py_ssize_t i = 0; i < nargs; ++i) {
            provided_mask |= (1ULL << i);
            if (FP_UNLIKELY(!specs[i].convert(PyTuple_GET_ITEM(args, i), targets[i]))) {
                return false;
            }
        }
    }

    if (kwargs) {
        PyObject *key;
        PyObject *val;
        Py_ssize_t pos = 0;
        while (PyDict_Next(kwargs, &pos, &key, &val)) {
            size_t idx = FP_EMPTY_SLOT;

            if (FP_LIKELY(fp->lookup_table)) {
                size_t h = fp_hash_ptr(key, fp->table_mask);
#pragma unroll 2
                while (fp->lookup_table[h] != FP_EMPTY_SLOT) {
                    size_t candidate = fp->lookup_table[h];
                    if (FP_LIKELY(specs[candidate].interned == key)) {
                        idx = candidate;
                        break;
                    }
                    h = (h + 1) & fp->table_mask;
                }
            }

            if (FP_UNLIKELY(idx == FP_EMPTY_SLOT)) {
#pragma unroll 2
                for (size_t i = 0; i < count; ++i) {
                    if (specs[i].interned == key ||
                        PyUnicode_Compare(key, specs[i].interned) == 0) {
                        idx = i;
                        break;
                    }
                }
            }

            if (FP_UNLIKELY(idx == FP_EMPTY_SLOT)) {
                PyErr_Format(PyExc_TypeError, "unexpected keyword argument '%U'", key);
                return false;
            }

            if (FP_UNLIKELY(provided_mask & (1ULL << idx))) {
                return fp_report_multiple(fp, idx);
            }

            provided_mask |= (1ULL << idx);
            if (FP_UNLIKELY(!specs[idx].convert(val, targets[idx]))) {
                return false;
            }
        }
    }

    if (FP_UNLIKELY((provided_mask & fp->required_mask) != fp->required_mask)) {
        return fp_report_missing(fp, provided_mask);
    }

    return (PyErr_Occurred() == nullptr);
}