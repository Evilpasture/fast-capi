#include "fast_parse.h"
#include <stdlib.h>

static const FastParseFunc MONO_STUBS[] = {fp_speculate_p0, fp_speculate_p1_naked,
                                           fp_speculate_p2_naked, fp_speculate_p3_naked,
                                           fp_speculate_p4_naked};

static_assert(sizeof(MONO_STUBS) / sizeof(FastParseFunc) == 5,
              "MONO_STUBS table must contain exactly 5 stubs (0-4 args)");

bool fp_report_missing(const FastParser *fastparser, uint64_t provided_mask) {
#pragma unroll 2
    for (size_t i = 0; i < fastparser->count; i++) {
        if (fastparser->specs[i].required && !(provided_mask & (1ULL << i))) {
            PyErr_Format(PyExc_TypeError, "required argument '%s' missing",
                         fastparser->specs[i].name);
            return false;
        }
    }
    return false;
}

bool fp_report_type_error(const FastParser *fastparser, size_t index, PyObject *val) {
    const FastArgSpec *spec = &fastparser->specs[index];
    const char *expected_type = "unknown";

    if (spec->type_name) {
        expected_type = spec->type_name;
    } else if (spec->type_guard) {
        expected_type = spec->type_guard->tp_name;
    }

    PyErr_Format(PyExc_TypeError, "argument '%s' must be %s, not %.200s", spec->name, expected_type,
                 Py_TYPE(val)->tp_name);
    return false;
}

bool fp_report_multiple(const FastParser *fastparser, size_t index) {
    PyErr_Format(PyExc_TypeError, "argument '%s' got multiple values",
                 fastparser->specs[index].name);
    return false;
}

bool fp_report_too_many(const FastParser *fastparser, Py_ssize_t nargs) {
    PyErr_Format(PyExc_TypeError, "too many positional arguments (expected %zu, got %zd)",
                 fastparser->count, nargs);
    return false;
}

void fp_init_impl(FastParser *fastparser, FastArgSpec *specs, size_t count) {
    if (count > 64) {
        Py_FatalError("FastParse: Argument count exceeds 64.");
    }

    fastparser->specs = specs;
    fastparser->count = count;
    fastparser->required_mask = 0;
    fastparser->type_guard_mask = 0;
    fastparser->lookup_table = nullptr;
#pragma unroll 2
    for (size_t i = 0; i < count; i++) {
        if (specs[i].name) {
            specs[i].interned = PyUnicode_InternFromString(specs[i].name);
        }
        if (specs[i].required) {
            fastparser->required_mask |= (1ULL << i);
        }
        if (specs[i].type_guard) {
            fastparser->type_guard_mask |= (1ULL << i);
        }
    }

    if (count > 8) { // Threshold for using hash table
        size_t table_size = 1;
#pragma unroll 2
        while (table_size < (count * 2)) {
            table_size <<= 1UL;
        }

        fastparser->table_mask = table_size - 1;
        fastparser->lookup_table = (uint16_t *)malloc(table_size * sizeof(uint16_t));
        if (!fastparser->lookup_table) {
            Py_FatalError("FastParse: Failed to allocate lookup table.");
        }
#pragma unroll 2
        for (size_t i = 0; i < table_size; i++) {
            fastparser->lookup_table[i] = FP_EMPTY_SLOT;
        }

        for (size_t i = 0; i < count; i++) {
            size_t hash = fp_hash_ptr(fastparser->specs[i].interned, fastparser->table_mask);
#pragma unroll 2
            while (fastparser->lookup_table[hash] != FP_EMPTY_SLOT) {
                hash = (hash + 1) & fastparser->table_mask;
            }
            fastparser->lookup_table[hash] = (uint16_t)i;
        }
    }

    fastparser->hot_path = fp_parse_vector;

    uint64_t all_required = (count >= 64) ? ~0ULL : ((1ULL << count) - 1);
    if (fastparser->type_guard_mask == 0 && fastparser->required_mask == all_required) {
        if (count < (sizeof(MONO_STUBS) / sizeof(FastParseFunc))) {
            fastparser->hot_path = MONO_STUBS[count];
        }
    }
}

void fp_deinit(FastParser *fastparser) {
    if (!fastparser) {
        return;
    }

    if (fastparser->specs) {
#pragma unroll 2
        for (size_t i = 0; i < fastparser->count; i++) {
            Py_XDECREF(fastparser->specs[i].interned);
            fastparser->specs[i].interned = nullptr;
        }
    }

    if (fastparser->lookup_table) {
        free(fastparser->lookup_table);
        fastparser->lookup_table = nullptr;
    }
}
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] bool fp_parse_legacy(PyObject *args, PyObject *kwargs,
                                   [[maybe_unused]] PyObject *unused, const FastParser *fastparser,
                                   void **targets) {
    uint64_t provided_mask = 0;
    const size_t count = fastparser->count;
    const FastArgSpec *specs = fastparser->specs;

    if (args) {
        Py_ssize_t nargs = PyTuple_GET_SIZE(args);
        if (FP_UNLIKELY(nargs > (Py_ssize_t)count)) {
            return fp_report_too_many(fastparser, nargs);
        }
#pragma unroll 2
        for (Py_ssize_t i = 0; i < nargs; ++i) {
            provided_mask |= (1ULL << (uint64_t)i);
            if (FP_UNLIKELY(!specs[i].convert(PyTuple_GET_ITEM(args, i), targets[i]))) {
                return false;
            }
        }
    }

    if (kwargs) {
        PyObject *key = nullptr;
        PyObject *val = nullptr;
        Py_ssize_t pos = 0;
        while (PyDict_Next(kwargs, &pos, &key, &val)) {
            size_t idx = FP_EMPTY_SLOT;

            if (FP_LIKELY(fastparser->lookup_table)) {
                size_t hash = fp_hash_ptr(key, fastparser->table_mask);
#pragma unroll 2
                while (fastparser->lookup_table[hash] != FP_EMPTY_SLOT) {
                    size_t candidate = fastparser->lookup_table[hash];
                    if (FP_LIKELY(specs[candidate].interned == key)) {
                        idx = candidate;
                        break;
                    }
                    hash = (hash + 1) & fastparser->table_mask;
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
                return fp_report_multiple(fastparser, idx);
            }

            provided_mask |= (1ULL << idx);
            if (FP_UNLIKELY(!specs[idx].convert(val, targets[idx]))) {
                return false;
            }
        }
    }

    if (FP_UNLIKELY((provided_mask & fastparser->required_mask) != fastparser->required_mask)) {
        return fp_report_missing(fastparser, provided_mask);
    }

    return (PyErr_Occurred() == nullptr);
}