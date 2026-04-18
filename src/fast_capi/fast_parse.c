#include "fast_parse.h"
#include <stdlib.h>

static const FastParseFunc MONO_STUBS[] = {fp_speculate_p0, fp_speculate_p1_naked,
                                           fp_speculate_p2_naked, fp_speculate_p3_naked,
                                           fp_speculate_p4_naked};

static_assert(sizeof(MONO_STUBS) / sizeof(FastParseFunc) == 5,
              "MONO_STUBS table must contain exactly 5 stubs (0-4 args)");

bool fp_report_missing(const FastParser *fastparser, uint64_t provided_mask) {
    PyObject *missing_list = PyList_New(0);
    if (!missing_list) {
        return false;
    }

    for (size_t i = 0; i < fastparser->count; i++) {
        if ((int)fastparser->specs[i].required && !(provided_mask & (1ULL << i))) {
            PyList_Append(missing_list, PyUnicode_FromString(fastparser->specs[i].name));
        }
    }

    Py_ssize_t num_missing = PyList_Size(missing_list);
    if (num_missing == 1) {
        PyErr_Format(PyExc_TypeError, "missing 1 required positional argument: '%U'",
                     PyList_GetItem(missing_list, 0));
    } else if (num_missing > 1) {
        PyObject *comma  = PyUnicode_FromString("', '");
        PyObject *joined = PyUnicode_Join(comma, missing_list);
        PyErr_Format(PyExc_TypeError, "missing %zd required positional arguments: '%U'",
                     num_missing, joined);
        Py_XDECREF(joined);
        Py_XDECREF(comma);
    }

    Py_DECREF(missing_list);
    return false;
}

bool fp_report_type_error(const FastParser *fastparser, size_t index, PyObject *val) {
    const FastArgSpec *spec   = &fastparser->specs[index];
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

[[gnu::always_inline]]
static inline int fp_min3(int a, int b, int c) {
    int m = a;
    if (b < m)
        m = b;
    if (c < m)
        m = c;
    return m;
}

static int fp_levenshtein(const char *s1, const char *s2) {
    // Assume s1 is the user input, s2 is the candidate from specs
    // Strip prefixes like 'ob_' or '_' if your interning logic needs it
    int len1 = (int)strlen(s1);
    int len2 = (int)strlen(s2);

    // Swap to ensure len1 >= len2 for smaller stack usage
    if (len1 < len2) {
        const char *tmp = s1;
        s1              = s2;
        s2              = tmp;
        int t           = len1;
        len1            = len2;
        len2            = t;
    }

    if (len2 == 0) {
        return len1;
    }
    if (len1 > 64) {
        len1 = 64; // Cap for safety
    }

    // uint8_t is enough for distance on 64-char strings
    alignas(16) uint8_t column[65];
#pragma unroll 4
    for (int i = 0; i <= len2; i++) {
        column[i] = (uint8_t)i;
    }
#pragma unroll 4
    for (int x = 1; x <= len1; x++) {
        column[0]         = (uint8_t)x;
        uint8_t last_diag = (uint8_t)(x - 1);
        for (int y = 1; y <= len2; y++) {
            uint8_t old_column = column[y];
            uint8_t cost       = (s1[x - 1] != s2[y - 1]);

            // Branchless min if your compiler doesn't do it automatically
            column[y] = fp_min3(column[y] + 1,     // Deletion
                                column[y - 1] + 1, // Insertion
                                last_diag + cost); // Substitution
            last_diag = old_column;
        }
    }
    return (int)column[len2];
}

bool fp_report_unknown_keyword(const FastParser *fastparser, PyObject *key) {
    const char *actual_name = PyUnicode_AsUTF8(key);
    if (!actual_name) {
        return false;
    }

    const char *best_match = nullptr;
    int best_dist          = 100;

    for (size_t i = 0; i < fastparser->count; i++) {
        const char *cand = fastparser->specs[i].name;
        int dist         = fp_levenshtein(actual_name, cand);

        if (dist < best_dist) {
            best_dist  = dist;
            best_match = cand;
        }
    }

    // Only suggest if the match is reasonably close (distance < 3)
    if (best_match && best_dist < 3) {
        PyErr_Format(PyExc_TypeError, "'%s' is an invalid keyword argument; did you mean '%s'?",
                     actual_name, best_match);
    } else {
        PyErr_Format(PyExc_TypeError, "'%s' is an invalid keyword argument", actual_name);
    }
    return false;
}

void fp_init_impl(FastParser *fastparser, FastArgSpec *specs, size_t count) {
    if (count > 64) {
        Py_FatalError("FastParse: Argument count exceeds 64.");
    }

    fastparser->specs           = specs;
    fastparser->count           = count;
    fastparser->required_mask   = 0;
    fastparser->type_guard_mask = 0;
    fastparser->lookup_table    = nullptr;
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

        fastparser->table_mask   = table_size - 1;
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
                                   [[maybe_unused]] PyObject *unused,
                                   const FastParser *FP_RESTRICT fastparser,
                                   void *FP_RESTRICT *FP_RESTRICT targets) {
    uint64_t provided_mask   = 0;
    const size_t count       = fastparser->count;
    const FastArgSpec *specs = fastparser->specs;
    const uint64_t tg_mask   = fastparser->type_guard_mask;

    // 1. Process Positional Arguments (from Tuple)
    if (args) {
        const Py_ssize_t nargs = PyTuple_GET_SIZE(args);
        if (FP_UNLIKELY(nargs > (Py_ssize_t)count)) {
            return fp_report_too_many(fastparser, nargs);
        }

        for (Py_ssize_t i = 0; i < nargs; ++i) {
            PyObject *val = PyTuple_GET_ITEM(args, i);

            // Apply type guard if present
            if (!fp_check_type_guard(&specs[i], val, tg_mask, (size_t)i)) {
                return fp_report_type_error(fastparser, (size_t)i, val);
            }

            // Convert
            if (FP_UNLIKELY(!specs[i].convert(val, targets[i]))) {
                return false;
            }
            provided_mask |= (1ULL << (size_t)i);
        }
    }

    // 2. Process Keyword Arguments (from Dict)
    if (kwargs && PyDict_Size(kwargs) > 0) {
        PyObject *key  = nullptr;
        PyObject *val  = nullptr;
        Py_ssize_t pos = 0;

        while (PyDict_Next(kwargs, &pos, &key, &val)) {
            // Use your unified lookup logic (Hash Table -> Linear Search fallback)
            size_t idx = fp_find_keyword_index(key, fastparser);

            if (FP_UNLIKELY(idx == FP_EMPTY_SLOT)) {
                return fp_report_unknown_keyword(fastparser, key);
            }

            // Check if this argument was already provided positionally
            if (FP_UNLIKELY(provided_mask & (1ULL << idx))) {
                return fp_report_multiple(fastparser, idx);
            }

            // Apply type guard
            if (!fp_check_type_guard(&specs[idx], val, tg_mask, idx)) {
                return fp_report_type_error(fastparser, idx, val);
            }

            // Convert
            if (FP_UNLIKELY(!specs[idx].convert(val, targets[idx]))) {
                return false;
            }
            provided_mask |= (1ULL << idx);
        }
    }

    // 3. Final Verification: Check for missing required arguments
    if (FP_UNLIKELY((provided_mask & fastparser->required_mask) != fastparser->required_mask)) {
        return fp_report_missing(fastparser, provided_mask);
    }

    return true;
}