#include "fast_parse.h"
#include <stdlib.h>

static const FastParseFunc MONO_STUBS[] = {
    fp_speculate_p0,       fp_speculate_p1_naked, fp_speculate_p2_naked, fp_speculate_p3_naked,
    fp_speculate_p4_naked, fp_speculate_p5_naked, fp_speculate_p6_naked};

static constexpr size_t STUBS_SIZE = 7;
static_assert(sizeof(MONO_STUBS) / sizeof(FastParseFunc) == STUBS_SIZE,
              "MONO_STUBS table must contain exactly 7 stubs (0-6 args)");

bool fp_report_missing(const FastParser *fastparser, uint64_t provided_mask) {
    const char *pname = fastparser->parser_name ? fastparser->parser_name : "function";

    PyObject *missing_details = PyList_New(0);
    PyObject *signature_parts = PyList_New(0);

    // If we can't allocate lists for the fancy error, fall back to a basic error
    if (FP_UNLIKELY(!missing_details || !signature_parts)) {
        Py_XDECREF(missing_details);
        Py_XDECREF(signature_parts);
        PyErr_Format(PyExc_TypeError, "%s() missing required positional arguments", pname);
        return false;
    }

    for (size_t i = 0; i < fastparser->count; i++) {
        const FastArgSpec *spec   = &fastparser->specs[i];
        const char *fallback_name = spec->type_guard ? spec->type_guard->tp_name : "object";
        const char *type_info     = spec->type_name ? spec->type_name : fallback_name;

        bool is_provided = (provided_mask & (1ULL << i)) != 0;
        if (spec->required && !is_provided) {
            PyObject *m = PyUnicode_FromFormat("    - %s (%s)", spec->name, type_info);
            if (m) {
                PyList_Append(missing_details, m);
                Py_DECREF(m);
            }
        }

        PyObject *sig_part = (int)spec->required
                                 ? PyUnicode_FromFormat("%s: %s", spec->name, type_info)
                                 : PyUnicode_FromFormat("[%s: %s]", spec->name, type_info);

        if (sig_part) {
            PyList_Append(signature_parts, sig_part);
            Py_DECREF(sig_part);
        }
    }

    Py_ssize_t num_missing = PyList_Size(missing_details);
    PyObject *newline      = PyUnicode_FromString("\n");
    PyObject *comma        = PyUnicode_FromString(", ");

    // Check for NULL before joining
    PyObject *joined_missing = (newline) ? PyUnicode_Join(newline, missing_details) : nullptr;
    PyObject *joined_sig     = (comma) ? PyUnicode_Join(comma, signature_parts) : nullptr;

    if (joined_missing && joined_sig) {
        PyErr_Format(
            PyExc_TypeError,
            "%s() missing %zd required positional argument%s:\n%U\n\nExpected signature:\n  %s(%U)",
            pname, num_missing, num_missing == 1 ? "" : "s", joined_missing, pname, joined_sig);
    } else {
        // Ultimate fallback if string joining failed
        PyErr_Format(PyExc_TypeError, "%s() missing %zd required positional arguments", pname,
                     num_missing);
    }

    // Cleanup: XDECREF is safe for nullptrs
    Py_XDECREF(newline);
    Py_XDECREF(comma);
    Py_XDECREF(joined_missing);
    Py_XDECREF(joined_sig);
    Py_XDECREF(missing_details);
    Py_XDECREF(signature_parts);

    return false;
}

bool fp_report_type_error(const FastParser *fastparser, size_t index, PyObject *val) {
    const FastArgSpec *spec = &fastparser->specs[index];
    const char *pname       = fastparser->parser_name ? fastparser->parser_name : "function";

    const char *fallback_name = (spec->type_guard) ? spec->type_guard->tp_name : "object";
    const char *expected      = (spec->type_name) ? spec->type_name : fallback_name;

    PyObject *val_repr        = nullptr;
    PyObject *signature_parts = nullptr;
    PyObject *comma           = nullptr;
    PyObject *joined_sig      = nullptr;

    // 1. Safe Repr Acquisition
    val_repr = PyObject_Repr(val);
    if (FP_UNLIKELY(!val_repr)) {
        PyErr_Clear(); // Clear OOM from Repr to set our TypeError
        val_repr = PyUnicode_FromString("<repr unavailable>");
    } else if (PyUnicode_GetLength(val_repr) > 100) {
        PyObject *truncated = PyUnicode_Substring(val_repr, 0, 97);
        if (truncated) {
            PyObject *fmt = PyUnicode_FromFormat("%U...", truncated);
            Py_DECREF(truncated);
            if (fmt) {
                Py_DECREF(val_repr);
                val_repr = fmt;
            }
        }
    }

    // 2. Diagnostic Signature Construction
    signature_parts = PyList_New(0);
    if (FP_UNLIKELY(!signature_parts)) {
        goto basic_error;
    }

    for (size_t i = 0; i < fastparser->count; i++) {
        const FastArgSpec *s = &fastparser->specs[i];
        const char *fb       = s->type_guard ? s->type_guard->tp_name : "object";
        const char *t        = s->type_name ? s->type_name : fb;

        static const char *FMT_MAP[] = {"[%s: %s]", "%s: %s"};
        const char *fmt = (i == index) ? "!!! %s: %s !!!" : FMT_MAP[(int)s->required != 0];

        PyObject *part = PyUnicode_FromFormat(fmt, s->name, t);

        if (part) {
            PyList_Append(signature_parts, part);
            Py_DECREF(part);
        }
    }

    comma = PyUnicode_FromString(", ");
    if (FP_UNLIKELY(!comma)) {
        goto basic_error;
    }

    joined_sig = PyUnicode_Join(comma, signature_parts);
    if (FP_UNLIKELY(!joined_sig)) {
        goto basic_error;
    }

    // 3. Final Fancy Format
    PyErr_Format(PyExc_TypeError,
                 "%s() argument '%s' must be %s, not %s.\n\n"
                 "Received value: %U\n"
                 "Signature context:\n  %s(%U)",
                 pname, spec->name, expected, Py_TYPE(val)->tp_name, val_repr ? val_repr : Py_None,
                 pname, joined_sig);
    goto cleanup;

basic_error:
    // If we ran out of memory making the fancy message, at least give the standard one
    PyErr_Format(PyExc_TypeError, "%s() argument '%s' must be %s, not %s", pname, spec->name,
                 expected, Py_TYPE(val)->tp_name);

cleanup:
    Py_XDECREF(comma);
    Py_XDECREF(joined_sig);
    Py_XDECREF(signature_parts);
    Py_XDECREF(val_repr);
    return false;
}

bool fp_report_multiple(const FastParser *fastparser, size_t index) {
    const char *pname    = fastparser->parser_name ? fastparser->parser_name : "function";
    const char *arg_name = fastparser->specs[index].name;

    PyObject *parts = nullptr;
    PyObject *comma = nullptr;
    PyObject *sig   = nullptr;

    // 1. Diagnostic Signature Construction
    parts = PyList_New(0);
    if (FP_UNLIKELY(!parts)) {
        goto basic_error;
    }

    for (size_t i = 0; i < fastparser->count; i++) {
        const char *name = fastparser->specs[i].name;
        PyObject *p =
            (i == index) ? PyUnicode_FromFormat("!!! %s !!!", name) : PyUnicode_FromString(name);
        if (p) {
            PyList_Append(parts, p);
            Py_DECREF(p);
        }
    }

    comma = PyUnicode_FromString(", ");
    if (FP_UNLIKELY(!comma)) {
        goto basic_error;
    }

    sig = PyUnicode_Join(comma, parts);
    if (FP_UNLIKELY(!sig)) {
        goto basic_error;
    }

    // 2. Final Fancy Format
    PyErr_Format(PyExc_TypeError,
                 "%s() got multiple values for argument '%s'.\n"
                 "It was provided both as a positional argument and as a keyword argument.\n\n"
                 "Signature context:\n  %s(%U)",
                 pname, arg_name, pname, sig);
    goto cleanup;

basic_error:
    // Fallback if we can't allocate the fancy message
    PyErr_Format(PyExc_TypeError, "%s() got multiple values for argument '%s'", pname, arg_name);

cleanup:
    Py_XDECREF(comma);
    Py_XDECREF(sig);
    Py_XDECREF(parts);
    return false;
}

bool fp_report_too_many(const FastParser *fastparser, Py_ssize_t nargs) {
    const char *pname = fastparser->parser_name ? fastparser->parser_name : "function";

    PyObject *parts = nullptr;
    PyObject *comma = nullptr;
    PyObject *sig   = nullptr;

    // 1. Calculate counts (Stack-based, safe)
    size_t req_count = 0;
    for (size_t i = 0; i < fastparser->count; i++) {
        if (fastparser->specs[i].required) { req_count++;
}
    }
    size_t total_count = fastparser->count;

    char count_str[64];
    if (req_count == total_count) {
        snprintf(count_str, sizeof(count_str), "%zu", total_count);
    } else {
        snprintf(count_str, sizeof(count_str), "from %zu to %zu", req_count, total_count);
    }

    // 2. Diagnostic Signature Construction
    parts = PyList_New(0);
    if (FP_UNLIKELY(!parts)) { goto basic_error;
}

    for (size_t i = 0; i < total_count; i++) {
        PyObject *p = PyUnicode_FromString(fastparser->specs[i].name);
        if (p) {
            PyList_Append(parts, p);
            Py_DECREF(p);
        }
    }

    comma = PyUnicode_FromString(", ");
    if (FP_UNLIKELY(!comma)) { goto basic_error;
}

    sig = PyUnicode_Join(comma, parts);
    if (FP_UNLIKELY(!sig)) { goto basic_error;
}

    // 3. Final Fancy Format
    PyErr_Format(PyExc_TypeError,
                 "%s() takes %s positional arguments but %zd were given.\n\n"
                 "This function does not accept variable positional arguments (*args).\n"
                 "Expected sequence:\n  %s(%U)",
                 pname, count_str, nargs, pname, sig);
    goto cleanup;

basic_error:
    // Fallback if we can't allocate the fancy message
    PyErr_Format(PyExc_TypeError, "%s() takes %s positional arguments but %zd were given", 
                 pname, count_str, nargs);

cleanup:
    Py_XDECREF(comma);
    Py_XDECREF(sig);
    Py_XDECREF(parts);
    return false;
}

[[gnu::always_inline, gnu::const]]
static inline int fp_min3(int a, int b, int c) {
    int m = a < b ? a : b;
    return m < c ? m : c;
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

    const char *best_match           = nullptr;
    constexpr auto initial_best_dist = 100;
    int best_dist                    = initial_best_dist;
    const char *pname = fastparser->parser_name ? fastparser->parser_name : "function";

    PyObject *valid_args_list = nullptr;
    PyObject *newline         = nullptr;
    PyObject *joined          = nullptr;

    // 1. Find the best fuzzy match (Stack-based, no OOM risk)
    for (size_t i = 0; i < fastparser->count; i++) {
        const char *cand = fastparser->specs[i].name;
        if (!cand) {
            continue;
        }
        int dist = fp_levenshtein(actual_name, cand);
        if (dist < best_dist) {
            best_dist  = dist;
            best_match = cand;
        }
    }

    // 2. Diagnostic Message Construction
    valid_args_list = PyList_New(0);
    if (FP_UNLIKELY(!valid_args_list)) {
        goto basic_error;
    }

    for (size_t i = 0; i < fastparser->count; i++) {
        const FastArgSpec *spec = &fastparser->specs[i];
        if (!spec->name) {
            continue;
        }

        const char *fb = spec->type_guard ? spec->type_guard->tp_name : "object";
        const char *t  = spec->type_name ? spec->type_name : fb;

        PyObject *info = PyUnicode_FromFormat("    - %s: %s%s", spec->name, t,
                                              (int)spec->required ? " [required]" : "");
        if (info) {
            PyList_Append(valid_args_list, info);
            Py_DECREF(info);
        }
    }

    newline = PyUnicode_FromString("\n");
    if (FP_UNLIKELY(!newline)) {
        goto basic_error;
    }

    joined = PyUnicode_Join(newline, valid_args_list);
    if (FP_UNLIKELY(!joined)) {
        goto basic_error;
    }

    // 3. Format "Did you mean" suggestion
    static constexpr auto suggestion_buffer = 256;
    char suggestion[suggestion_buffer];
    suggestion[0] = '\0';
    if (best_match && best_dist < 3) {
        snprintf(suggestion, sizeof(suggestion), " Did you mean '%s'?", best_match);
    }

    // 4. Final Fancy Format
    PyErr_Format(PyExc_TypeError,
                 "'%s' is an invalid keyword argument for %s().%s\nValid arguments are:\n%U",
                 actual_name, pname, suggestion, joined);
    goto cleanup;

basic_error:
    // Fallback logic
    if (best_match && best_dist < 3) {
        PyErr_Format(PyExc_TypeError,
                     "'%s' is an invalid keyword argument for %s(); did you mean '%s'?",
                     actual_name, pname, best_match);
    } else {
        PyErr_Format(PyExc_TypeError, "'%s' is an invalid keyword argument for %s()", actual_name,
                     pname);
    }

cleanup:
    Py_XDECREF(newline);
    Py_XDECREF(joined);
    Py_XDECREF(valid_args_list);
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
    fastparser->warned          = false;
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

/** --- LEGACY HELPERS --- **/

[[gnu::always_inline]]
static inline bool fp_process_pos_legacy(const FastParser *FP_RESTRICT fastparse, PyObject *args,
                                         Py_ssize_t nargs, void *FP_RESTRICT *FP_RESTRICT targets) {
#pragma unroll 2
    for (Py_ssize_t i = 0; i < nargs; ++i) {
        PyObject *val = PyTuple_GET_ITEM(args, i);
        if (!fp_check_type_guard(&fastparse->specs[i], val, fastparse->type_guard_mask,
                                 (size_t)i)) {
            return fp_report_type_error(fastparse, (size_t)i, val);
        }
        if (FP_UNLIKELY(!fastparse->specs[i].convert(val, targets[i]))) {
            return false;
        }
    }
    return true;
}

[[gnu::always_inline]]
static inline bool fp_process_kw_legacy(const FastParser *FP_RESTRICT fastparse, PyObject *kwargs,
                                        uint64_t *FP_RESTRICT mask,
                                        void *FP_RESTRICT *FP_RESTRICT targets) {
    PyObject *key;
    PyObject *val;
    Py_ssize_t pos = 0;

    while (PyDict_Next(kwargs, &pos, &key, &val)) {
        size_t idx = fp_find_keyword_index(key, fastparse);

        if (FP_UNLIKELY(idx == FP_EMPTY_SLOT)) {
            return fp_report_unknown_keyword(fastparse, key);
        }

        if (FP_UNLIKELY(*mask & (1ULL << idx))) {
            return fp_report_multiple(fastparse, idx);
        }

        if (!fp_check_type_guard(&fastparse->specs[idx], val, fastparse->type_guard_mask, idx)) {
            return fp_report_type_error(fastparse, idx, val);
        }

        if (FP_UNLIKELY(!fastparse->specs[idx].convert(val, targets[idx]))) {
            return false;
        }
        *mask |= (1ULL << idx);
    }
    return true;
}

/** --- ORCHESTRATOR --- **/

[[nodiscard, gnu::flatten]]
bool fp_parse_legacy(PyObject *args, PyObject *kwargs, [[maybe_unused]] PyObject *unused,
                     const FastParser *FP_RESTRICT fastparser,
                     void *FP_RESTRICT *FP_RESTRICT targets) {

    Py_ssize_t nargs = args ? PyTuple_GET_SIZE(args) : 0;

    // 1. Pre-flight Check
    if (FP_UNLIKELY(nargs > (Py_ssize_t)fastparser->count)) {
        return fp_report_too_many(fastparser, nargs);
    }

    // 2. Handle Positionals
    if (nargs > 0) {
        if (FP_UNLIKELY(!fp_process_pos_legacy(fastparser, args, nargs, targets))) {
            return false;
        }
    }

    // 3. Setup Mask & Handle Keywords
    uint64_t mask = fp_make_mask((size_t)nargs);

    if (kwargs && PyDict_GET_SIZE(kwargs) > 0) {
        if (FP_UNLIKELY(!fp_process_kw_legacy(fastparser, kwargs, &mask, targets))) {
            return false;
        }
    }

    // 4. Final Verification
    if (FP_UNLIKELY((mask & fastparser->required_mask) != fastparser->required_mask)) {
        return fp_report_missing(fastparser, mask);
    }

    return true;
}