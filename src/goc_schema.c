/* src/goc_schema.c
 *
 * Implementation of the libgoc value schema library.
 *
 * Copyright (c) Divyansh Prakash
 */

#include <assert.h>
#include <complex.h>
#include <math.h>
#include <regex.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "../include/goc_schema.h"
#include "../include/goc.h"

#ifdef HAVE_GC_H
# include <gc/gc.h>
#endif

/* -------------------------------------------------------------------------
 * Internal definitions
 * ---------------------------------------------------------------------- */

typedef enum {
    SCHEMA_NULL, SCHEMA_BOOL, SCHEMA_INT, SCHEMA_REAL, SCHEMA_COMPLEX, SCHEMA_UINT,
    SCHEMA_NUMBER, SCHEMA_BYTE, SCHEMA_UBYTE, SCHEMA_STR, SCHEMA_ANY,
    SCHEMA_BOOL_CONST, SCHEMA_INT_CONST, SCHEMA_REAL_CONST, SCHEMA_STR_CONST,
    SCHEMA_INT_MIN, SCHEMA_INT_MAX, SCHEMA_INT_RANGE, SCHEMA_INT_ENUM,
    SCHEMA_REAL_MIN, SCHEMA_REAL_MAX, SCHEMA_REAL_RANGE,
    SCHEMA_REAL_EX_MIN, SCHEMA_REAL_EX_MAX, SCHEMA_REAL_MULTIPLE,
    SCHEMA_STR_MIN_LEN, SCHEMA_STR_MAX_LEN, SCHEMA_STR_LEN,
    SCHEMA_STR_PATTERN, SCHEMA_STR_ENUM, SCHEMA_STR_FORMAT,
    SCHEMA_ARR, SCHEMA_ARR_LEN, SCHEMA_ARR_UNIQUE, SCHEMA_ARR_CONTAINS,
    SCHEMA_TUPLE, SCHEMA_OBJ,
    SCHEMA_IF, SCHEMA_ANY_OF, SCHEMA_ONE_OF, SCHEMA_ALL_OF, SCHEMA_NOT,
    SCHEMA_REF,
    SCHEMA_PREDICATE,
} schema_kind;

struct goc_schema {
    schema_kind        kind;
    goc_schema_meta_t* meta;
    union {
        bool           bool_const;
        int64_t        int_const;
        double         real_const;
        char*          str_const;

        int64_t        int_min;
        int64_t        int_max;
        struct {
            int64_t min;
            int64_t max;
        } int_range;
        goc_array*     int_enum;

        double         real_min;
        double         real_max;
        struct {
            double min;
            double max;
        } real_range;
        double         real_factor;

        size_t         str_len_min;
        size_t         str_len_max;
        struct {
            size_t min;
            size_t max;
        } str_len;
        char*          str_pattern;
        regex_t*       str_regex;
        goc_array*     str_enum;
        char*          str_format_name;
        regex_t*       str_format_regex;

        struct {
            goc_schema* elem;
        } arr;

        struct {
            goc_schema* elem;
            size_t      min;
            size_t      max;
        } arr_len;

        struct {
            goc_schema* elem;
        } arr_unique;

        struct {
            goc_schema* elem;
            goc_schema* contains;
            size_t      min_contains;
            size_t      max_contains;
        } arr_contains;

        struct {
            goc_array* items;
            goc_schema* additional_items;
        } tuple;

        struct {
            goc_array* fields;
            goc_schema_dict_opts_t opts;
        } obj;

        goc_array* schemas;

        goc_schema_ref* ref;

        struct {
            bool (*fn)(void* val);
            const char* name;
        } predicate;
    } u;
};

struct goc_schema_ref {
    goc_schema  base;
    goc_schema* target;
};

struct goc_schema_registry {
    goc_dict* map;
};

static goc_schema* s_schema_null    = NULL;
static goc_schema* s_schema_bool    = NULL;
static goc_schema* s_schema_int     = NULL;
static goc_schema* s_schema_uint    = NULL;
static goc_schema* s_schema_number  = NULL;
static goc_schema* s_schema_byte    = NULL;
static goc_schema* s_schema_ubyte   = NULL;
static goc_schema* s_schema_real    = NULL;
static goc_schema* s_schema_complex = NULL;
static goc_schema* s_schema_str     = NULL;
static goc_schema* s_schema_any     = NULL;
static goc_schema* s_schema_arr     = NULL;
static goc_schema* s_schema_dict    = NULL;

static goc_dict*    schema_hierarchy = NULL;
static uv_rwlock_t  schema_hierarchy_lock;

static const uintptr_t kProbeError = 1;

static inline bool goc_schema_is_probe_error(goc_schema_error* err) {
    return (uintptr_t)err == kProbeError;
}

static bool goc_schema_is_gc_ptr(void* ptr) {
    if (!ptr) return false;
    return GC_base(ptr) != NULL;
}

static bool goc_schema_is_boxed(void* val);

static bool goc_schema_is_goc_array(void* val) {
    if (!val) return false;
    if (goc_schema_is_boxed(val)) return false;
    goc_array* arr = (goc_array*)val;
    if (!goc_schema_is_gc_ptr(arr->data)) return false;
    uintptr_t data_ptr = (uintptr_t)arr->data;
    if ((data_ptr & (sizeof(void*) - 1)) != 0) return false;
    if (data_ptr < 4096) return false;
    if (arr->cap < 8) return false;
    if (arr->head > arr->cap) return false;
    if (arr->len > arr->cap) return false;
    if (arr->head + arr->len > arr->cap) return false;
    return true;
}

static bool goc_schema_is_goc_dict(void* val) {
    if (!val) return false;
    if (goc_schema_is_boxed(val)) return false;
    goc_dict* d = (goc_dict*)val;
    if (!d->table || !d->table_keys || !d->keys) return false;
    if (!goc_schema_is_gc_ptr(d->table) || !goc_schema_is_gc_ptr(d->table_keys) || !goc_schema_is_gc_ptr(d->keys)) return false;
    uintptr_t table_ptr = (uintptr_t)d->table;
    uintptr_t table_keys_ptr = (uintptr_t)d->table_keys;
    uintptr_t keys_ptr = (uintptr_t)d->keys;
    if ((table_ptr & (sizeof(void*) - 1)) != 0 ||
        (table_keys_ptr & (sizeof(void*) - 1)) != 0 ||
        (keys_ptr & (sizeof(void*) - 1)) != 0) {
        return false;
    }
    if (table_ptr < 4096 || table_keys_ptr < 4096 || keys_ptr < 4096) return false;
    if (d->table_cap < 8) return false;
    if ((d->table_cap & (d->table_cap - 1)) != 0) return false;
    if (d->len > d->occupied) return false;
    if (d->occupied > d->table_cap) return false;
    return true;
}

static goc_boxed_type_t goc_schema_boxed_type(void* val) {
    if (!val) return GOC_BOXED_TYPE_UNKNOWN;
    void* base = GC_base(val);
    if (!base || base == val) return GOC_BOXED_TYPE_UNKNOWN;
    goc_boxed_header_t* hdr = (goc_boxed_header_t*)base;
    return hdr->type;
}

static size_t goc_schema_boxed_payload_size(void* val) {
    if (!val) return 0;
    void* base = GC_base(val);
    if (!base || base == val) return 0;
    goc_boxed_header_t* hdr = (goc_boxed_header_t*)base;
    return hdr->size;
}

static bool goc_schema_is_boxed(void* val) {
    if (!val) return false;
    void* base = GC_base(val);
    if (!base || base == val) return false;
    goc_boxed_header_t* hdr = (goc_boxed_header_t*)base;
    if (hdr->type <= GOC_BOXED_TYPE_UNKNOWN || hdr->type >= GOC_BOXED_TYPE_MAX) return false;
    if (hdr->size == 0) return false;
    return true;
}

static int64_t goc_schema_boxed_signed_value(void* val) {
    const void* payload = val;
    size_t sz = goc_schema_boxed_payload_size(val);
    if (sz == sizeof(char)) {
        return (int64_t)*(signed char*)payload;
    }
    if (sz == sizeof(short)) {
        return (int64_t)*(short*)payload;
    }
    if (sz == sizeof(int)) {
        return (int64_t)*(int*)payload;
    }
    if (sz == sizeof(long)) {
        return (int64_t)*(long*)payload;
    }
    if (sz == sizeof(long long)) {
        return (int64_t)*(long long*)payload;
    }
    return 0;
}

static uint64_t goc_schema_boxed_unsigned_value(void* val) {
    const void* payload = val;
    size_t sz = goc_schema_boxed_payload_size(val);
    if (sz == sizeof(unsigned char)) {
        return (uint64_t)*(unsigned char*)payload;
    }
    if (sz == sizeof(unsigned short)) {
        return (uint64_t)*(unsigned short*)payload;
    }
    if (sz == sizeof(unsigned int)) {
        return (uint64_t)*(unsigned int*)payload;
    }
    if (sz == sizeof(unsigned long)) {
        return (uint64_t)*(unsigned long*)payload;
    }
    if (sz == sizeof(unsigned long long)) {
        return (uint64_t)*(unsigned long long*)payload;
    }
    return 0;
}

static bool goc_schema_is_boxed_bool(void* val) {
    if (!val) return false;
    if (!goc_schema_is_boxed(val)) return false;
    return goc_schema_boxed_type(val) == GOC_BOXED_TYPE_BOOL &&
           goc_schema_boxed_payload_size(val) == sizeof(bool);
}

static bool goc_schema_is_boxed_byte(void* val) {
    if (!val) return false;
    if (!goc_schema_is_boxed(val)) return false;
    return goc_schema_boxed_type(val) == GOC_BOXED_TYPE_BYTE &&
           goc_schema_boxed_payload_size(val) == sizeof(char);
}

static bool goc_schema_is_boxed_ubyte(void* val) {
    if (!val) return false;
    if (!goc_schema_is_boxed(val)) return false;
    return goc_schema_boxed_type(val) == GOC_BOXED_TYPE_UBYTE &&
           goc_schema_boxed_payload_size(val) == sizeof(unsigned char);
}

static bool goc_schema_is_boxed_int(void* val) {
    if (!val) return false;
    if (!goc_schema_is_boxed(val)) return false;
    goc_boxed_type_t type = goc_schema_boxed_type(val);
    size_t sz = goc_schema_boxed_payload_size(val);
    if (type == GOC_BOXED_TYPE_BYTE) {
        return sz == sizeof(char);
    }
    if (type != GOC_BOXED_TYPE_INT) return false;
    return sz == sizeof(short) || sz == sizeof(int) || sz == sizeof(long) || sz == sizeof(long long);
}

static bool goc_schema_is_boxed_uint(void* val) {
    if (!val) return false;
    if (!goc_schema_is_boxed(val)) return false;
    if (goc_schema_boxed_type(val) != GOC_BOXED_TYPE_UINT) return false;
    size_t sz = goc_schema_boxed_payload_size(val);
    return sz == sizeof(unsigned short) || sz == sizeof(unsigned int) ||
           sz == sizeof(unsigned long) || sz == sizeof(unsigned long long);
}

static bool goc_schema_is_boxed_int_unsigned(void* val) {
    if (!val) return false;
    if (!goc_schema_is_boxed(val)) return false;
    goc_boxed_type_t type = goc_schema_boxed_type(val);
    if (type == GOC_BOXED_TYPE_UBYTE) {
        return goc_schema_boxed_payload_size(val) == sizeof(unsigned char);
    }
    if (type != GOC_BOXED_TYPE_UINT) return false;
    size_t sz = goc_schema_boxed_payload_size(val);
    return sz == sizeof(unsigned short) || sz == sizeof(unsigned int) ||
           sz == sizeof(unsigned long) || sz == sizeof(unsigned long long);
}

static bool goc_schema_is_boxed_real(void* val) {
    if (!val) return false;
    if (!goc_schema_is_boxed(val)) return false;
    if (goc_schema_boxed_type(val) != GOC_BOXED_TYPE_REAL) return false;
    size_t sz = goc_schema_boxed_payload_size(val);
    return sz == sizeof(float) || sz == sizeof(double) || sz == sizeof(long double);
}

static bool goc_schema_is_boxed_complex(void* val) {
    if (!val) return false;
    if (!goc_schema_is_boxed(val)) return false;
    if (goc_schema_boxed_type(val) != GOC_BOXED_TYPE_COMPLEX) return false;
    size_t sz = goc_schema_boxed_payload_size(val);
    return sz == sizeof(float complex) ||
           sz == sizeof(double complex) ||
           sz == sizeof(long double complex);
}

static long double complex goc_schema_boxed_complex_value(void* val) {
    size_t sz = goc_schema_boxed_payload_size(val);
    if (sz == sizeof(float complex)) {
        return (long double complex)goc_unbox(float complex, val);
    }
    if (sz == sizeof(double complex)) {
        return (long double complex)goc_unbox(double complex, val);
    }
    if (sz == sizeof(long double complex)) {
        return goc_unbox(long double complex, val);
    }
    return 0.0L + 0.0L * I;
}

static const char* schema_ptr_key(goc_schema* s) {
    return goc_sprintf("0x%" PRIxPTR, (uintptr_t)s);
}

static void goc_schema_hierarchy_init(void) {
    if (schema_hierarchy) return;
    schema_hierarchy = goc_dict_make(0);
    uv_rwlock_init(&schema_hierarchy_lock);
}

static void goc_schema_register_builtin_edges(void* ud);

static bool goc_schema_is_string(void* val) {
    if (!val) return false;
    if (goc_schema_is_boxed(val)) return false;
    if (goc_schema_is_goc_array(val) || goc_schema_is_goc_dict(val)) return false;
    return true;
}

static const char* goc_schema_type_name(void* val) {
    if (!val) return "null";
    if (goc_schema_is_goc_array(val)) return "array";
    if (goc_schema_is_goc_dict(val)) return "object";
    if (goc_schema_is_boxed_bool(val)) return "bool";
    if (goc_schema_is_boxed_byte(val)) return "byte";
    if (goc_schema_is_boxed_ubyte(val)) return "ubyte";
    if (goc_schema_is_boxed_uint(val)) return "uint";
    if (goc_schema_is_boxed_int(val)) return "int";
    if (goc_schema_is_boxed_complex(val)) return "complex";
    if (goc_schema_is_boxed_real(val)) return "real";
    if (goc_schema_is_string(val)) return "str";
    return "unknown";
}

static bool goc_schema_vals_equal(void* a, void* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (goc_schema_is_boxed_int(a) && goc_schema_is_boxed_int(b)) {
        return goc_schema_boxed_signed_value(a) == goc_schema_boxed_signed_value(b);
    }
    if (goc_schema_is_boxed_int_unsigned(a) && goc_schema_is_boxed_int_unsigned(b)) {
        return goc_schema_boxed_unsigned_value(a) == goc_schema_boxed_unsigned_value(b);
    }
    if (goc_schema_is_boxed_real(a) && goc_schema_is_boxed_real(b)) {
        double da = goc_unbox(double, a);
        double db = goc_unbox(double, b);
        if (isnan(da) || isnan(db)) return false;
        return memcmp(&da, &db, sizeof(double)) == 0;
    }
    if (goc_schema_is_boxed_complex(a) && goc_schema_is_boxed_complex(b)) {
        long double complex ca = goc_schema_boxed_complex_value(a);
        long double complex cb = goc_schema_boxed_complex_value(b);
        return memcmp(&ca, &cb, sizeof(long double complex)) == 0;
    }
    if (goc_schema_is_boxed_bool(a) && goc_schema_is_boxed_bool(b)) {
        return goc_unbox(bool, a) == goc_unbox(bool, b);
    }
    if (goc_schema_is_string(a) && goc_schema_is_string(b)) {
        return strcmp((char*)a, (char*)b) == 0;
    }
    if (goc_schema_is_goc_array(a) && goc_schema_is_goc_array(b)) {
        goc_array* aa = (goc_array*)a;
        goc_array* ab = (goc_array*)b;
        size_t len = goc_array_len(aa);
        if (len != goc_array_len(ab)) return false;
        for (size_t i = 0; i < len; i++) {
            if (!goc_schema_vals_equal(goc_array_get(aa, i), goc_array_get(ab, i))) {
                return false;
            }
        }
        return true;
    }
    if (goc_schema_is_goc_dict(a) && goc_schema_is_goc_dict(b)) {
        goc_dict* da = (goc_dict*)a;
        goc_dict* db = (goc_dict*)b;
        if (goc_dict_len(da) != goc_dict_len(db)) return false;
        goc_array* keys = goc_dict_keys(da);
        for (size_t i = 0; i < goc_array_len(keys); i++) {
            char* k = (char*)goc_array_get(keys, i);
            void* va = goc_dict_get(da, k, NULL);
            void* vb = goc_dict_get(db, k, NULL);
            if (!goc_schema_vals_equal(va, vb)) return false;
        }
        return true;
    }
    return false;
}

static _Atomic int           g_schema_initialized = 0;
static goc_schema_registry*  s_global_registry    = NULL;

static void goc_schema_init_singletons(void) {
    goc_schema_hierarchy_init();

    s_schema_null = (goc_schema*)goc_malloc(sizeof(goc_schema));
    s_schema_null->kind = SCHEMA_NULL;
    s_schema_null->meta = NULL;

    s_schema_bool = (goc_schema*)goc_malloc(sizeof(goc_schema));
    s_schema_bool->kind = SCHEMA_BOOL;
    s_schema_bool->meta = NULL;

    s_schema_int = (goc_schema*)goc_malloc(sizeof(goc_schema));
    s_schema_int->kind = SCHEMA_INT;
    s_schema_int->meta = NULL;

    s_schema_uint = (goc_schema*)goc_malloc(sizeof(goc_schema));
    s_schema_uint->kind = SCHEMA_UINT;
    s_schema_uint->meta = NULL;

    s_schema_byte = (goc_schema*)goc_malloc(sizeof(goc_schema));
    s_schema_byte->kind = SCHEMA_BYTE;
    s_schema_byte->meta = NULL;

    s_schema_ubyte = (goc_schema*)goc_malloc(sizeof(goc_schema));
    s_schema_ubyte->kind = SCHEMA_UBYTE;
    s_schema_ubyte->meta = NULL;

    s_schema_number = (goc_schema*)goc_malloc(sizeof(goc_schema));
    s_schema_number->kind = SCHEMA_NUMBER;
    s_schema_number->meta = NULL;

    s_schema_real = (goc_schema*)goc_malloc(sizeof(goc_schema));
    s_schema_real->kind = SCHEMA_REAL;
    s_schema_real->meta = NULL;

    s_schema_complex = (goc_schema*)goc_malloc(sizeof(goc_schema));
    s_schema_complex->kind = SCHEMA_COMPLEX;
    s_schema_complex->meta = NULL;

    s_schema_str = (goc_schema*)goc_malloc(sizeof(goc_schema));
    s_schema_str->kind = SCHEMA_STR;
    s_schema_str->meta = NULL;

    s_schema_any = (goc_schema*)goc_malloc(sizeof(goc_schema));
    s_schema_any->kind = SCHEMA_ANY;
    s_schema_any->meta = NULL;

    s_schema_arr = (goc_schema*)goc_malloc(sizeof(goc_schema));
    s_schema_arr->kind = SCHEMA_ARR;
    s_schema_arr->meta = NULL;
    memset(&s_schema_arr->u, 0, sizeof(s_schema_arr->u));
    s_schema_arr->u.arr.elem = s_schema_any;

    s_schema_dict = (goc_schema*)goc_malloc(sizeof(goc_schema));
    s_schema_dict->kind = SCHEMA_OBJ;
    s_schema_dict->meta = NULL;
    memset(&s_schema_dict->u, 0, sizeof(s_schema_dict->u));
    s_schema_dict->u.obj.fields = goc_array_make(0);

    goc_schema_register_builtin_edges(NULL);

    s_global_registry = goc_schema_registry_make();
    goc_schema_registry_add(s_global_registry, "goc/any",     s_schema_any);
    goc_schema_registry_add(s_global_registry, "goc/null",    s_schema_null);
    goc_schema_registry_add(s_global_registry, "goc/bool",    s_schema_bool);
    goc_schema_registry_add(s_global_registry, "goc/int",     s_schema_int);
    goc_schema_registry_add(s_global_registry, "goc/uint",    s_schema_uint);
    goc_schema_registry_add(s_global_registry, "goc/byte",    s_schema_byte);
    goc_schema_registry_add(s_global_registry, "goc/ubyte",   s_schema_ubyte);
    goc_schema_registry_add(s_global_registry, "goc/number",  s_schema_number);
    goc_schema_registry_add(s_global_registry, "goc/real",    s_schema_real);
    goc_schema_registry_add(s_global_registry, "goc/complex", s_schema_complex);
    goc_schema_registry_add(s_global_registry, "goc/str",     s_schema_str);
    goc_schema_registry_add(s_global_registry, "goc/arr",     s_schema_arr);
    goc_schema_registry_add(s_global_registry, "goc/dict",    s_schema_dict);
}

static void goc_schema_ensure_init(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_schema_initialized,
                                                &expected, 1,
                                                memory_order_acq_rel,
                                                memory_order_relaxed)) {
        goc_schema_init_singletons();
    }
}

static goc_schema* goc_schema_make(schema_kind kind) {
    goc_schema* schema = (goc_schema*)goc_malloc(sizeof(goc_schema));
    schema->kind = kind;
    schema->meta = NULL;
    memset(&schema->u, 0, sizeof(schema->u));
    return schema;
}

struct goc_schema_error {
    const char* path;
    const char* message;
};

const char* goc_schema_error_path(const goc_schema_error* err) {
    if (!err || goc_schema_is_probe_error((goc_schema_error*)err)) return "";
    return err->path ? err->path : "";
}

const char* goc_schema_error_message(const goc_schema_error* err) {
    if (!err || goc_schema_is_probe_error((goc_schema_error*)err)) return "";
    return err->message ? err->message : "";
}

static goc_schema_error* goc_schema_error_new(const char* path,
                                                const char* message,
                                                bool probe) {
    if (probe) {
        return (goc_schema_error*)kProbeError;
    }
    goc_schema_error* err = (goc_schema_error*)goc_malloc(sizeof(goc_schema_error));
    err->path = path;
    err->message = message;
    return err;
}

static goc_schema_error* goc_schema_validate_impl(const goc_schema* schema,
                                                    void* val,
                                                    const char* path,
                                                    bool probe);
static bool goc_schema_probe(const goc_schema* schema, void* val);

static void goc_schema_register_builtin_edges(void* ud) {
    (void)ud;
    goc_schema_derive(s_schema_null,    s_schema_any);
    goc_schema_derive(s_schema_str,     s_schema_any);

    // numeric tower
    goc_schema_derive(s_schema_number,  s_schema_any);
    goc_schema_derive(s_schema_complex, s_schema_number);
    goc_schema_derive(s_schema_real,    s_schema_complex);
    goc_schema_derive(s_schema_int,     s_schema_real);
    goc_schema_derive(s_schema_uint,    s_schema_int);
    goc_schema_derive(s_schema_byte,    s_schema_int);
    goc_schema_derive(s_schema_ubyte,   s_schema_byte);
    goc_schema_derive(s_schema_bool,    s_schema_byte);
}

goc_schema* goc_schema_any(void)     { goc_schema_ensure_init(); return s_schema_any;     }
goc_schema* goc_schema_null(void)    { goc_schema_ensure_init(); return s_schema_null;    }
goc_schema* goc_schema_str(void)     { goc_schema_ensure_init(); return s_schema_str;     }

goc_schema* goc_schema_number(void)  { goc_schema_ensure_init(); return s_schema_number;  }
goc_schema* goc_schema_complex(void) { goc_schema_ensure_init(); return s_schema_complex; }
goc_schema* goc_schema_real(void)    { goc_schema_ensure_init(); return s_schema_real;    }
goc_schema* goc_schema_int(void)     { goc_schema_ensure_init(); return s_schema_int;     }
goc_schema* goc_schema_uint(void)    { goc_schema_ensure_init(); return s_schema_uint;    }
goc_schema* goc_schema_byte(void)    { goc_schema_ensure_init(); return s_schema_byte;    }
goc_schema* goc_schema_ubyte(void)   { goc_schema_ensure_init(); return s_schema_ubyte;   }
goc_schema* goc_schema_bool(void)    { goc_schema_ensure_init(); return s_schema_bool;    }
goc_schema* goc_schema_arr_any(void) { goc_schema_ensure_init(); return s_schema_arr;     }
goc_schema* goc_schema_dict_any(void){ goc_schema_ensure_init(); return s_schema_dict;    }


void goc_schema_derive(goc_schema* child, goc_schema* parent) {
    if (!child || !parent) return;
    uv_rwlock_wrlock(&schema_hierarchy_lock);
    const char* key = schema_ptr_key(child);
    goc_array* parents = (goc_array*)goc_dict_get(schema_hierarchy, key, NULL);
    if (!parents) {
        parents = goc_array_make(0);
        goc_dict_set(schema_hierarchy, key, parents);
    }
    goc_array_push(parents, parent);
    uv_rwlock_wrunlock(&schema_hierarchy_lock);
}

static bool goc_schema_array_contains_schema(goc_array* arr, goc_schema* schema) {
    if (!arr || !schema) return false;
    for (size_t i = 0; i < goc_array_len(arr); i++) {
        if (goc_array_get(arr, i) == schema) return true;
    }
    return false;
}

bool goc_schema_is_a(goc_schema* child, goc_schema* parent) {
    if (!child || !parent) return false;
    if (child == parent) return true;
    uv_rwlock_rdlock(&schema_hierarchy_lock);
    goc_array* queue = goc_array_make(0);
    goc_array* visited = goc_array_make(0);
    goc_array_push(queue, child);
    bool found = false;

    for (size_t qi = 0; qi < goc_array_len(queue) && !found; qi++) {
        goc_schema* cur = (goc_schema*)goc_array_get(queue, qi);
        bool seen = false;
        for (size_t i = 0; i < goc_array_len(visited); i++) {
            if (goc_array_get(visited, i) == cur) { seen = true; break; }
        }
        if (seen) continue;
        goc_array_push(visited, cur);

        goc_array* parents = (goc_array*)goc_dict_get(schema_hierarchy, schema_ptr_key(cur), NULL);
        if (!parents) continue;
        for (size_t i = 0; i < goc_array_len(parents); i++) {
            goc_schema* p = (goc_schema*)goc_array_get(parents, i);
            if (p == parent) { found = true; break; }
            goc_array_push(queue, p);
        }
    }

    uv_rwlock_rdunlock(&schema_hierarchy_lock);
    return found;
}

static goc_array* goc_schema_copy_parents(goc_schema* schema) {
    goc_array* result = goc_array_make(0);
    if (!schema) return result;
    goc_array* parents = (goc_array*)goc_dict_get(schema_hierarchy, schema_ptr_key(schema), NULL);
    if (!parents) return result;
    for (size_t i = 0; i < goc_array_len(parents); i++) {
        goc_array_push(result, goc_array_get(parents, i));
    }
    return result;
}

static goc_array* goc_schema_collect_ancestors(goc_schema* schema) {
    goc_array* result = goc_array_make(0);
    if (!schema) return result;

    goc_array* queue = goc_array_make(0);
    goc_array* visited = goc_array_make(0);
    goc_array_push(queue, schema);

    for (size_t qi = 0; qi < goc_array_len(queue); qi++) {
        goc_schema* cur = (goc_schema*)goc_array_get(queue, qi);
        if (goc_schema_array_contains_schema(visited, cur)) continue;
        goc_array_push(visited, cur);

        goc_array* parents = (goc_array*)goc_dict_get(schema_hierarchy, schema_ptr_key(cur), NULL);
        if (!parents) continue;
        for (size_t i = 0; i < goc_array_len(parents); i++) {
            goc_schema* parent = (goc_schema*)goc_array_get(parents, i);
            if (!goc_schema_array_contains_schema(visited, parent)) {
                goc_array_push(queue, parent);
            }
            if (parent != schema && !goc_schema_array_contains_schema(result, parent)) {
                goc_array_push(result, parent);
            }
        }
    }

    return result;
}

static goc_array* goc_schema_collect_descendants(goc_schema* schema) {
    goc_array* result = goc_array_make(0);
    if (!schema) return result;

    goc_array* queue = goc_array_make(0);
    goc_array* visited = goc_array_make(0);
    goc_array* keys = goc_dict_keys(schema_hierarchy);

    for (size_t ki = 0; ki < goc_array_len(keys); ki++) {
        const char* key = (const char*)goc_array_get(keys, ki);
        goc_array* parents = (goc_array*)goc_dict_get(schema_hierarchy, key, NULL);
        if (!parents) continue;
        if (goc_schema_array_contains_schema(parents, schema)) {
            uintptr_t child_ptr = (uintptr_t)strtoull(key, NULL, 0);
            goc_schema* child = (goc_schema*)child_ptr;
            goc_array_push(queue, child);
        }
    }

    while (goc_array_len(queue) > 0) {
        goc_schema* cur = (goc_schema*)goc_array_get(queue, 0);
        goc_array* new_queue = goc_array_make(0);
        for (size_t qi = 0; qi < goc_array_len(queue); qi++) {
            goc_schema* node = (goc_schema*)goc_array_get(queue, qi);
            if (goc_schema_array_contains_schema(visited, node)) continue;
            goc_array_push(visited, node);
            if (!goc_schema_array_contains_schema(result, node)) {
                goc_array_push(result, node);
            }

            for (size_t ki = 0; ki < goc_array_len(keys); ki++) {
                const char* key = (const char*)goc_array_get(keys, ki);
                goc_array* parents = (goc_array*)goc_dict_get(schema_hierarchy, key, NULL);
                if (!parents) continue;
                if (goc_schema_array_contains_schema(parents, node)) {
                    uintptr_t child_ptr = (uintptr_t)strtoull(key, NULL, 0);
                    goc_schema* child = (goc_schema*)child_ptr;
                    if (!goc_schema_array_contains_schema(visited, child)) {
                        goc_array_push(new_queue, child);
                    }
                }
            }
        }
        queue = new_queue;
    }

    return result;
}

goc_array* goc_schema_parents(goc_schema* schema) {
    if (!schema) return goc_array_make(0);
    uv_rwlock_rdlock(&schema_hierarchy_lock);
    goc_array* result = goc_schema_copy_parents(schema);
    uv_rwlock_rdunlock(&schema_hierarchy_lock);
    return result;
}

goc_array* goc_schema_ancestors(goc_schema* schema) {
    if (!schema) return goc_array_make(0);
    uv_rwlock_rdlock(&schema_hierarchy_lock);
    goc_array* result = goc_schema_collect_ancestors(schema);
    uv_rwlock_rdunlock(&schema_hierarchy_lock);
    return result;
}

goc_array* goc_schema_descendants(goc_schema* schema) {
    if (!schema) return goc_array_make(0);
    uv_rwlock_rdlock(&schema_hierarchy_lock);
    goc_array* result = goc_schema_collect_descendants(schema);
    uv_rwlock_rdunlock(&schema_hierarchy_lock);
    return result;
}

static void goc_schema_compile_pattern_props(goc_array* pattern_props) {
    if (!pattern_props) return;
    size_t n = goc_array_len(pattern_props);
    for (size_t i = 0; i < n; i++) {
        goc_schema_pattern_prop_t* prop = (goc_schema_pattern_prop_t*)goc_array_get(pattern_props, i);
        if (prop && prop->pattern && prop->regex == NULL) {
            prop->regex = (regex_t*)goc_malloc(sizeof(regex_t));
            if (regcomp(prop->regex, prop->pattern, REG_EXTENDED | REG_NOSUB) != 0) {
                ABORT("goc_schema_dict: invalid pattern_props regex '%s'\n", prop->pattern);
            }
        }
    }
}

static bool goc_schema_probe(const goc_schema* schema, void* val) {
    if (!schema) return false;
    return goc_schema_validate_impl(schema, val, "", true) == NULL;
}

static bool goc_schema_val_is_array(void* val) {
    return goc_schema_is_goc_array(val);
}

static bool goc_schema_val_is_object(void* val) {
    return goc_schema_is_goc_dict(val);
}

static bool goc_schema_val_is_bool(void* val) {
    return goc_schema_is_boxed_bool(val);
}

static bool goc_schema_val_is_int(void* val) {
    return goc_schema_is_boxed_int(val);
}

static bool goc_schema_val_is_uint(void* val) {
    return goc_schema_is_boxed_int_unsigned(val);
}

static bool goc_schema_val_is_byte(void* val) {
    return goc_schema_is_boxed_byte(val);
}

static bool goc_schema_val_is_ubyte(void* val) {
    return goc_schema_is_boxed_ubyte(val);
}

static bool goc_schema_val_is_real(void* val) {
    return goc_schema_is_boxed_real(val);
}

static bool goc_schema_val_is_str(void* val) {
    return goc_schema_is_string(val);
}

static goc_schema_error* goc_schema_validate_array(void* val,
                                      const char* path,
                                      bool probe) {
    if (!goc_schema_val_is_array(val)) {
        return goc_schema_error_new(path,
                                    goc_sprintf("expected array, got %s",
                                                 goc_schema_type_name(val)),
                                    probe);
    }
    return NULL;
}

static goc_schema_error* goc_schema_validate_object(void* val,
                                       const char* path,
                                       bool probe) {
    if (!goc_schema_val_is_object(val)) {
        return goc_schema_error_new(path,
                                    goc_sprintf("expected object, got %s",
                                                 goc_schema_type_name(val)),
                                    probe);
    }
    return NULL;
}

static goc_schema_error* goc_schema_validate_impl(const goc_schema* schema,
                                                    void* val,
                                                    const char* path,
                                                    bool probe) {
    assert(schema != NULL);
    if (!path) {
        path = "";
    }

    switch (schema->kind) {
    case SCHEMA_NULL:
        if (val == NULL) return NULL;
        return goc_schema_error_new(path,
                                    goc_sprintf("expected null, got %s",
                                                 goc_schema_type_name(val)),
                                    probe);
    case SCHEMA_BOOL:
        if (goc_schema_val_is_bool(val)) return NULL;
        return goc_schema_error_new(path,
                                    goc_sprintf("expected bool, got %s",
                                                 goc_schema_type_name(val)),
                                    probe);
    case SCHEMA_INT:
        if (goc_schema_val_is_int(val)) return NULL;
        return goc_schema_error_new(path,
                                    goc_sprintf("expected int, got %s",
                                                 goc_schema_type_name(val)),
                                    probe);
    case SCHEMA_REAL:
        if (goc_schema_val_is_real(val)) return NULL;
        return goc_schema_error_new(path,
                                    goc_sprintf("expected real, got %s",
                                                 goc_schema_type_name(val)),
                                    probe);
    case SCHEMA_COMPLEX:
        if (goc_schema_is_boxed_complex(val)) return NULL;
        return goc_schema_error_new(path,
                                    goc_sprintf("expected complex, got %s",
                                                 goc_schema_type_name(val)),
                                    probe);
    case SCHEMA_UINT:
        if (goc_schema_val_is_uint(val)) return NULL;
        return goc_schema_error_new(path,
                                    goc_sprintf("expected uint, got %s",
                                                 goc_schema_type_name(val)),
                                    probe);
    case SCHEMA_NUMBER:
        if (goc_schema_is_boxed_int(val) ||
            goc_schema_is_boxed_int_unsigned(val) ||
            goc_schema_is_boxed_real(val) ||
            goc_schema_is_boxed_complex(val))
            return NULL;
        return goc_schema_error_new(path,
            goc_sprintf("expected number, got %s", goc_schema_type_name(val)),
            probe);
    case SCHEMA_BYTE:
        if (goc_schema_val_is_byte(val)) return NULL;
        return goc_schema_error_new(path,
                                    goc_sprintf("expected byte, got %s",
                                                 goc_schema_type_name(val)),
                                    probe);
    case SCHEMA_UBYTE:
        if (goc_schema_val_is_ubyte(val)) return NULL;
        return goc_schema_error_new(path,
                                    goc_sprintf("expected ubyte, got %s",
                                                 goc_schema_type_name(val)),
                                    probe);
    case SCHEMA_STR:
        if (goc_schema_val_is_str(val)) return NULL;
        return goc_schema_error_new(path,
                                    goc_sprintf("expected str, got %s",
                                                 goc_schema_type_name(val)),
                                    probe);
    case SCHEMA_ANY:
        return NULL;
    case SCHEMA_BOOL_CONST: {
        if (!goc_schema_val_is_bool(val)) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected bool, got %s",
                                                     goc_schema_type_name(val)),
                                        probe);
        }
        bool got = goc_unbox(bool, val);
        if (got == schema->u.bool_const) return NULL;
        return goc_schema_error_new(path,
                                   goc_sprintf("expected const value %s, got %s",
                                                schema->u.bool_const ? "true" : "false",
                                                got ? "true" : "false"),
                                   probe);
    }
    case SCHEMA_INT_CONST: {
        if (!goc_schema_val_is_int(val)) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected int, got %s",
                                                     goc_schema_type_name(val)),
                                        probe);
        }
        int64_t got = goc_schema_boxed_signed_value(val);
        if (got == schema->u.int_const) return NULL;
        return goc_schema_error_new(path,
                                   goc_sprintf("expected const value %lld, got %lld",
                                                (long long)schema->u.int_const,
                                                (long long)got),
                                   probe);
    }
    case SCHEMA_REAL_CONST: {
        if (!goc_schema_val_is_real(val)) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected real, got %s",
                                                     goc_schema_type_name(val)),
                                        probe);
        }
        double got = goc_unbox(double, val);
        if (got == schema->u.real_const) return NULL;
        return goc_schema_error_new(path,
                                   goc_sprintf("expected const value %g, got %g",
                                                schema->u.real_const,
                                                got),
                                   probe);
    }
    case SCHEMA_STR_CONST: {
        if (!goc_schema_val_is_str(val)) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected str, got %s",
                                                     goc_schema_type_name(val)),
                                        probe);
        }
        if (strcmp((char*)val, schema->u.str_const) == 0) return NULL;
        return goc_schema_error_new(path,
                                   goc_sprintf("expected const value %s, got %s",
                                                schema->u.str_const,
                                                (char*)val),
                                   probe);
    }
    case SCHEMA_INT_MIN:
        if (!goc_schema_val_is_int(val)) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected int, got %s",
                                                     goc_schema_type_name(val)),
                                        probe);
        }
        {
            int64_t got = goc_schema_boxed_signed_value(val);
            if (got >= schema->u.int_min) return NULL;
            return goc_schema_error_new(path,
                                        goc_sprintf("expected minimum %lld, got %lld",
                                                     (long long)schema->u.int_min,
                                                     (long long)got),
                                        probe);
        }
    case SCHEMA_INT_MAX:
        if (!goc_schema_val_is_int(val)) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected int, got %s",
                                                     goc_schema_type_name(val)),
                                        probe);
        }
        {
            int64_t got = goc_schema_boxed_signed_value(val);
            if (got <= schema->u.int_max) return NULL;
            return goc_schema_error_new(path,
                                        goc_sprintf("expected maximum %lld, got %lld",
                                                     (long long)schema->u.int_max,
                                                     (long long)got),
                                        probe);
        }
    case SCHEMA_INT_RANGE:
        if (!goc_schema_val_is_int(val)) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected int, got %s",
                                                     goc_schema_type_name(val)),
                                        probe);
        }
        {
            int64_t got = goc_schema_boxed_signed_value(val);
            if (got >= schema->u.int_range.min && got <= schema->u.int_range.max) return NULL;
            if (got < schema->u.int_range.min) {
                return goc_schema_error_new(path,
                                            goc_sprintf("expected minimum %lld, got %lld",
                                                         (long long)schema->u.int_range.min,
                                                         (long long)got),
                                            probe);
            }
            return goc_schema_error_new(path,
                                        goc_sprintf("expected maximum %lld, got %lld",
                                                     (long long)schema->u.int_range.max,
                                                     (long long)got),
                                        probe);
        }
    case SCHEMA_INT_ENUM:
        if (!goc_schema_val_is_int(val)) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected int, got %s",
                                                     goc_schema_type_name(val)),
                                        probe);
        }
        {
            int64_t got = goc_schema_boxed_signed_value(val);
            size_t n = goc_array_len(schema->u.int_enum);
            for (size_t i = 0; i < n; i++) {
                int64_t candidate = goc_schema_boxed_signed_value(goc_array_get(schema->u.int_enum, i));
                if (candidate == got) return NULL;
            }
            return goc_schema_error_new(path,
                                       goc_sprintf("expected const value %lld, got %lld",
                                                    (long long)goc_schema_boxed_signed_value(goc_array_get(schema->u.int_enum, 0)),
                                                    (long long)got),
                                       probe);
        }
    case SCHEMA_REAL_MIN:
        if (!goc_schema_val_is_real(val)) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected real, got %s",
                                                     goc_schema_type_name(val)),
                                        probe);
        }
        if (goc_unbox(double, val) >= schema->u.real_min) return NULL;
        return goc_schema_error_new(path,
                                    goc_sprintf("expected minimum %g, got %g",
                                                 schema->u.real_min,
                                                 goc_unbox(double, val)),
                                    probe);
    case SCHEMA_REAL_MAX:
        if (!goc_schema_val_is_real(val)) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected real, got %s",
                                                     goc_schema_type_name(val)),
                                        probe);
        }
        if (goc_unbox(double, val) <= schema->u.real_max) return NULL;
        return goc_schema_error_new(path,
                                    goc_sprintf("expected maximum %g, got %g",
                                                 schema->u.real_max,
                                                 goc_unbox(double, val)),
                                    probe);
    case SCHEMA_REAL_RANGE:
        if (!goc_schema_val_is_real(val)) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected real, got %s",
                                                     goc_schema_type_name(val)),
                                        probe);
        }
        {
            double got = goc_unbox(double, val);
            if (got >= schema->u.real_range.min && got <= schema->u.real_range.max) return NULL;
            if (got < schema->u.real_range.min) {
                return goc_schema_error_new(path,
                                            goc_sprintf("expected minimum %g, got %g",
                                                         schema->u.real_range.min,
                                                         got),
                                            probe);
            }
            return goc_schema_error_new(path,
                                        goc_sprintf("expected maximum %g, got %g",
                                                     schema->u.real_range.max,
                                                     got),
                                        probe);
        }
    case SCHEMA_REAL_EX_MIN:
        if (!goc_schema_val_is_real(val)) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected real, got %s",
                                                     goc_schema_type_name(val)),
                                        probe);
        }
        if (goc_unbox(double, val) > schema->u.real_min) return NULL;
        return goc_schema_error_new(path,
                                    goc_sprintf("expected minimum %g, got %g",
                                                 schema->u.real_min,
                                                 goc_unbox(double, val)),
                                    probe);
    case SCHEMA_REAL_EX_MAX:
        if (!goc_schema_val_is_real(val)) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected real, got %s",
                                                     goc_schema_type_name(val)),
                                        probe);
        }
        if (goc_unbox(double, val) < schema->u.real_max) return NULL;
        return goc_schema_error_new(path,
                                    goc_sprintf("expected maximum %g, got %g",
                                                 schema->u.real_max,
                                                 goc_unbox(double, val)),
                                    probe);
    case SCHEMA_REAL_MULTIPLE:
        if (!goc_schema_val_is_real(val)) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected real, got %s",
                                                     goc_schema_type_name(val)),
                                        probe);
        }
        {
            double got = goc_unbox(double, val);
            double factor = schema->u.real_factor;
            double remainder = fmod(got, factor);
            if (remainder < 0) remainder += factor;
            double eps = 1e-9 * fabs(factor);
            if (remainder <= eps || fabs(remainder - factor) <= eps) return NULL;
            return goc_schema_error_new(path,
                                        goc_sprintf("expected multiple of %g, got %g",
                                                     factor,
                                                     got),
                                        probe);
        }
    case SCHEMA_STR_MIN_LEN:
    case SCHEMA_STR_MAX_LEN:
    case SCHEMA_STR_LEN:
    case SCHEMA_STR_PATTERN:
    case SCHEMA_STR_ENUM:
    case SCHEMA_STR_FORMAT: {
        if (!goc_schema_val_is_str(val)) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected str, got %s",
                                                     goc_schema_type_name(val)),
                                        probe);
        }
        char* s = (char*)val;
        size_t len = strlen(s);
        if (schema->kind == SCHEMA_STR_MIN_LEN) {
            if (len >= schema->u.str_len_min) return NULL;
            return goc_schema_error_new(path,
                                        goc_sprintf("expected minimum %zu, got %zu",
                                                     schema->u.str_len_min,
                                                     len),
                                        probe);
        }
        if (schema->kind == SCHEMA_STR_MAX_LEN) {
            if (len <= schema->u.str_len_max) return NULL;
            return goc_schema_error_new(path,
                                        goc_sprintf("expected maximum %zu, got %zu",
                                                     schema->u.str_len_max,
                                                     len),
                                        probe);
        }
            if (schema->kind == SCHEMA_STR_LEN) {
            if (len >= schema->u.str_len.min && len <= schema->u.str_len.max) return NULL;
            return goc_schema_error_new(path,
                                        goc_sprintf("expected minimum %zu, got %zu",
                                                     schema->u.str_len.min,
                                                     len),
                                        probe);
        }
        if (schema->kind == SCHEMA_STR_PATTERN) {
            if (schema->u.str_regex == NULL) return NULL;
            if (regexec(schema->u.str_regex, s, 0, NULL, 0) == 0) return NULL;
            return goc_schema_error_new(path,
                                        goc_sprintf("expected pattern %s, got %s",
                                                     schema->u.str_pattern,
                                                     s),
                                        probe);
        }
        if (schema->kind == SCHEMA_STR_ENUM) {
            size_t n = goc_array_len(schema->u.str_enum);
            for (size_t i = 0; i < n; i++) {
                char* candidate = (char*)goc_array_get(schema->u.str_enum, i);
                if (strcmp(candidate, s) == 0) return NULL;
            }
            return goc_schema_error_new(path,
                                        goc_sprintf("expected const value %s, got %s",
                                                     (char*)goc_array_get(schema->u.str_enum, 0),
                                                     s),
                                        probe);
        }
        if (schema->kind == SCHEMA_STR_FORMAT) {
            if (schema->u.str_format_regex == NULL) return NULL;
            if (regexec(schema->u.str_format_regex, s, 0, NULL, 0) == 0) return NULL;
            return goc_schema_error_new(path,
                                        goc_sprintf("expected format %s, got %s",
                                                     schema->u.str_format_name,
                                                     s),
                                        probe);
        }
        return NULL;
    }
    case SCHEMA_ARR: {
        goc_schema_error* maybe_err = goc_schema_validate_array(val, path, probe);
        if (maybe_err) return maybe_err;
        goc_array* arr = (goc_array*)val;
        size_t len = goc_array_len(arr);
        for (size_t i = 0; i < len; i++) {
            char* sub_path = goc_sprintf("%s.[%zu]", path, i);
            goc_schema_error* err = goc_schema_validate_impl(schema->u.arr.elem,
                                                              goc_array_get(arr, i),
                                                              sub_path,
                                                              probe);
            if (err) return err;
        }
        return NULL;
    }
    case SCHEMA_ARR_LEN: {
        goc_schema_error* maybe_err = goc_schema_validate_array(val, path, probe);
        if (maybe_err) return maybe_err;
        goc_array* arr = (goc_array*)val;
        size_t len = goc_array_len(arr);
        if (len < schema->u.arr_len.min || len > schema->u.arr_len.max) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected %zu-%zu elements, got %zu",
                                                     schema->u.arr_len.min,
                                                     schema->u.arr_len.max,
                                                     len),
                                        probe);
        }
        for (size_t i = 0; i < len; i++) {
            char* sub_path = goc_sprintf("%s.[%zu]", path, i);
            goc_schema_error* err = goc_schema_validate_impl(schema->u.arr_len.elem,
                                                              goc_array_get(arr, i),
                                                              sub_path,
                                                              probe);
            if (err) return err;
        }
        return NULL;
    }
    case SCHEMA_ARR_UNIQUE: {
        goc_schema_error* maybe_err = goc_schema_validate_array(val, path, probe);
        if (maybe_err) return maybe_err;
        goc_array* arr = (goc_array*)val;
        size_t len = goc_array_len(arr);
        for (size_t i = 0; i < len; i++) {
            char* sub_path = goc_sprintf("%s.[%zu]", path, i);
            goc_schema_error* err = goc_schema_validate_impl(schema->u.arr_unique.elem,
                                                              goc_array_get(arr, i),
                                                              sub_path,
                                                              probe);
            if (err) return err;
        }
        for (size_t i = 0; i < len; i++) {
            void* a = goc_array_get(arr, i);
            for (size_t j = i + 1; j < len; j++) {
                void* b = goc_array_get(arr, j);
                if (goc_schema_vals_equal(a, b)) {
                    return goc_schema_error_new(path,
                                                goc_sprintf("duplicate elements at index %zu and %zu",
                                                             i, j),
                                                probe);
                }
            }
        }
        return NULL;
    }
    case SCHEMA_ARR_CONTAINS: {
        goc_schema_error* maybe_err = goc_schema_validate_array(val, path, probe);
        if (maybe_err) return maybe_err;
        goc_array* arr = (goc_array*)val;
        size_t len = goc_array_len(arr);
        for (size_t i = 0; i < len; i++) {
            char* sub_path = goc_sprintf("%s.[%zu]", path, i);
            goc_schema_error* err = goc_schema_validate_impl(schema->u.arr_contains.elem,
                                                              goc_array_get(arr, i),
                                                              sub_path,
                                                              probe);
            if (err) return err;
        }
        size_t match_count = 0;
        for (size_t i = 0; i < len; i++) {
            if (goc_schema_probe(schema->u.arr_contains.contains, goc_array_get(arr, i))) {
                match_count++;
            }
        }
        if (match_count < schema->u.arr_contains.min_contains) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected at least %zu elements matching contains schema",
                                                     schema->u.arr_contains.min_contains),
                                        probe);
        }
        if (schema->u.arr_contains.max_contains != 0 &&
            match_count > schema->u.arr_contains.max_contains) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected at most %zu elements matching contains schema",
                                                     schema->u.arr_contains.max_contains),
                                        probe);
        }
        return NULL;
    }
    case SCHEMA_TUPLE: {
        goc_schema_error* maybe_err = goc_schema_validate_array(val, path, probe);
        if (maybe_err) return maybe_err;
        goc_array* arr = (goc_array*)val;
        size_t len = goc_array_len(arr);
        size_t item_count = goc_array_len(schema->u.tuple.items);
        for (size_t i = 0; i < item_count; i++) {
            char* sub_path = goc_sprintf("%s.[%zu]", path, i);
            goc_schema_item_t* item = (goc_schema_item_t*)goc_array_get(schema->u.tuple.items, i);
            goc_schema_error* err = goc_schema_validate_impl(item->schema,
                                                              goc_array_get(arr, i),
                                                              sub_path,
                                                              probe);
            if (err) return err;
        }
        if (len > item_count) {
            if (!schema->u.tuple.additional_items) {
                return goc_schema_error_new(path,
                                            goc_sprintf("unexpected element at index %zu",
                                                         item_count),
                                            probe);
            }
            for (size_t i = item_count; i < len; i++) {
                char* sub_path = goc_sprintf("%s.[%zu]", path, i);
                goc_schema_error* err = goc_schema_validate_impl(schema->u.tuple.additional_items,
                                                                  goc_array_get(arr, i),
                                                                  sub_path,
                                                                  probe);
                if (err) return err;
            }
        }
        return NULL;
    }
    case SCHEMA_OBJ: {
        goc_schema_error* maybe_err = goc_schema_validate_object(val, path, probe);
        if (maybe_err) return maybe_err;
        goc_dict* dict = (goc_dict*)val;
        size_t property_count = goc_dict_len(dict);
        if (schema->u.obj.opts.min_properties != 0 && property_count < schema->u.obj.opts.min_properties) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected %zu-%zu properties, got %zu",
                                                     schema->u.obj.opts.min_properties,
                                                     schema->u.obj.opts.max_properties,
                                                     property_count),
                                        probe);
        }
        if (schema->u.obj.opts.max_properties != 0 && property_count > schema->u.obj.opts.max_properties) {
            return goc_schema_error_new(path,
                                        goc_sprintf("expected %zu-%zu properties, got %zu",
                                                     schema->u.obj.opts.min_properties,
                                                     schema->u.obj.opts.max_properties,
                                                     property_count),
                                        probe);
        }
        goc_array* keys = goc_dict_keys(dict);
        size_t n_keys = goc_array_len(keys);
        for (size_t i = 0; i < n_keys; i++) {
            char* key = (char*)goc_array_get(keys, i);
            if (schema->u.obj.opts.property_names) {
                goc_schema_error* err = goc_schema_validate_impl(schema->u.obj.opts.property_names,
                                                                  key,
                                                                  path,
                                                                  probe);
                if (err) {
                    char* message = goc_sprintf("key '%s': %s", key, err->message);
                    return goc_schema_error_new(path, message, probe);
                }
            }
            if (schema->u.obj.opts.pattern_props) {
                size_t n = goc_array_len(schema->u.obj.opts.pattern_props);
                for (size_t j = 0; j < n; j++) {
                    goc_schema_pattern_prop_t* prop = (goc_schema_pattern_prop_t*)goc_array_get(schema->u.obj.opts.pattern_props, j);
                    if (!prop || !prop->regex) {
                        ABORT("goc_schema_pattern_props: missing compiled regex for '%s'\n", prop ? prop->pattern : "<null>");
                    }
                    int match = regexec(prop->regex, key, 0, NULL, 0);
                    if (match == 0) {
                        char* sub_path = goc_sprintf("%s.%s", path, key);
                        goc_schema_error* err = goc_schema_validate_impl(prop->schema,
                                                                          goc_dict_get(dict, key, NULL),
                                                                          sub_path,
                                                                          probe);
                        if (err) return err;
                    }
                }
            }
        }
        goc_array* fields = schema->u.obj.fields;
        size_t field_count = goc_array_len(fields);
        for (size_t i = 0; i < field_count; i++) {
            goc_schema_field_t* field = (goc_schema_field_t*)goc_array_get(fields, i);
            void* value = goc_dict_get(dict, field->key, NULL);
            if (value == NULL) {
                if (!field->optional) {
                    char* sub_path = goc_sprintf("%s.%s", path, field->key);
                    return goc_schema_error_new(sub_path, "missing required field", probe);
                }
                continue;
            }
            char* sub_path = goc_sprintf("%s.%s", path, field->key);
            goc_schema_error* err = goc_schema_validate_impl(field->schema,
                                                              value,
                                                              sub_path,
                                                              probe);
            if (err) return err;
        }
        if (schema->u.obj.opts.strict) {
            for (size_t i = 0; i < n_keys; i++) {
                char* key = (char*)goc_array_get(keys, i);
                bool found = false;
                for (size_t j = 0; j < field_count; j++) {
                    goc_schema_field_t* field = (goc_schema_field_t*)goc_array_get(fields, j);
                    if (strcmp(field->key, key) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    char* sub_path = goc_sprintf("%s.%s", path, key);
                    return goc_schema_error_new(sub_path,
                                                goc_sprintf("unexpected key '%s'", key),
                                                probe);
                }
            }
        }
        if (schema->u.obj.opts.dep_required) {
            size_t n = goc_array_len(schema->u.obj.opts.dep_required);
            for (size_t i = 0; i < n; i++) {
                goc_schema_dep_req_t* dep = (goc_schema_dep_req_t*)goc_array_get(schema->u.obj.opts.dep_required, i);
                if (goc_dict_contains(dict, dep->key)) {
                    size_t m = goc_array_len(dep->required);
                    for (size_t j = 0; j < m; j++) {
                        char* required_key = (char*)goc_array_get(dep->required, j);
                        if (!goc_dict_contains(dict, required_key)) {
                            return goc_schema_error_new(path,
                                                        goc_sprintf("field '%s' required when '%s' is present",
                                                                     required_key,
                                                                     dep->key),
                                                        probe);
                        }
                    }
                }
            }
        }
        if (schema->u.obj.opts.dep_schemas) {
            size_t n = goc_array_len(schema->u.obj.opts.dep_schemas);
            for (size_t i = 0; i < n; i++) {
                goc_schema_dep_schema_t* dep = (goc_schema_dep_schema_t*)goc_array_get(schema->u.obj.opts.dep_schemas, i);
                if (goc_dict_contains(dict, dep->key)) {
                    goc_schema_error* err = goc_schema_validate_impl(dep->schema,
                                                                      val,
                                                                      path,
                                                                      probe);
                    if (err) return err;
                }
            }
        }
        return NULL;
    }
    case SCHEMA_IF: {
        if (goc_schema_probe(schema->u.schemas ? goc_array_get(schema->u.schemas,0) : NULL, val)) {
            goc_schema* then_schema = schema->u.schemas ? (goc_schema*)goc_array_get(schema->u.schemas,1) : NULL;
            if (!then_schema) return NULL;
            return goc_schema_validate_impl(then_schema, val, path, probe);
        }
        goc_schema* else_schema = schema->u.schemas ? (goc_schema*)goc_array_get(schema->u.schemas,2) : NULL;
        if (!else_schema) return NULL;
        return goc_schema_validate_impl(else_schema, val, path, probe);
    }
    case SCHEMA_ANY_OF: {
        size_t n = goc_array_len(schema->u.schemas);
        size_t matches = 0;
        for (size_t i = 0; i < n; i++) {
            if (goc_schema_probe((goc_schema*)goc_array_get(schema->u.schemas, i), val)) {
                matches++;
            }
        }
        if (matches > 0) return NULL;
        return goc_schema_error_new(path,
                                    goc_sprintf("value matched none of %zu schemas", n),
                                    probe);
    }
    case SCHEMA_ONE_OF: {
        size_t n = goc_array_len(schema->u.schemas);
        size_t matches = 0;
        for (size_t i = 0; i < n; i++) {
            if (goc_schema_probe((goc_schema*)goc_array_get(schema->u.schemas, i), val)) {
                matches++;
            }
        }
        if (matches == 1) return NULL;
        return goc_schema_error_new(path,
                                    goc_sprintf("value matched %zu of %zu schemas, expected exactly 1",
                                                 matches, n),
                                    probe);
    }
    case SCHEMA_ALL_OF: {
        size_t n = goc_array_len(schema->u.schemas);
        for (size_t i = 0; i < n; i++) {
            goc_schema_error* err = goc_schema_validate_impl((goc_schema*)goc_array_get(schema->u.schemas, i),
                                                              val,
                                                              path,
                                                              probe);
            if (err) return err;
        }
        return NULL;
    }
    case SCHEMA_NOT: {
        if (goc_schema_probe(schema->u.schemas ? (goc_schema*)goc_array_get(schema->u.schemas,0) : NULL, val)) {
            return goc_schema_error_new(path,
                                        "value must not be valid against schema",
                                        probe);
        }
        return NULL;
    }
    case SCHEMA_PREDICATE: {
        if (!schema->u.predicate.fn(val)) {
            const char* name = schema->u.predicate.name ? schema->u.predicate.name : "predicate";
            return goc_schema_error_new(path,
                                        goc_sprintf("value failed %s", name),
                                        probe);
        }
        return NULL;
    }
    case SCHEMA_REF: {
        goc_schema_ref* ref = (goc_schema_ref*)(void*)schema;
        if (ref->target == NULL) {
            return goc_schema_error_new(path, "unresolved schema ref", probe);
        }
        return goc_schema_validate_impl(ref->target, val, path, probe);
    }
    default:
        ABORT("goc_schema_validate_impl: unsupported schema kind %d\n", schema->kind);
    }
}

/* --------------------- public constructors ------------------------------ */

goc_schema* goc_schema_bool_const(bool val) {
    goc_schema* s = goc_schema_make(SCHEMA_BOOL_CONST);
    s->u.bool_const = val;
    return s;
}

goc_schema* goc_schema_int_const(int64_t val) {
    goc_schema* s = goc_schema_make(SCHEMA_INT_CONST);
    s->u.int_const = val;
    return s;
}

goc_schema* goc_schema_real_const(double val) {
    goc_schema* s = goc_schema_make(SCHEMA_REAL_CONST);
    s->u.real_const = val;
    return s;
}

goc_schema* goc_schema_str_const(const char* val) {
    goc_schema* s = goc_schema_make(SCHEMA_STR_CONST);
    s->u.str_const = (char*)val;
    return s;
}

goc_schema* goc_schema_int_min(int64_t min) {
    goc_schema* s = goc_schema_make(SCHEMA_INT_MIN);
    s->u.int_min = min;
    return s;
}

goc_schema* goc_schema_int_max(int64_t max) {
    goc_schema* s = goc_schema_make(SCHEMA_INT_MAX);
    s->u.int_max = max;
    return s;
}

goc_schema* goc_schema_int_range(int64_t min, int64_t max) {
    if (min > max) {
        ABORT("goc_schema_int_range: min > max (%lld > %lld)\n",
              (long long)min, (long long)max);
    }
    goc_schema* s = goc_schema_make(SCHEMA_INT_RANGE);
    s->u.int_range.min = min;
    s->u.int_range.max = max;
    return s;
}

goc_schema* goc_schema_int_enum(goc_array* vals) {
    goc_schema* s = goc_schema_make(SCHEMA_INT_ENUM);
    s->u.int_enum = vals;
    return s;
}

goc_schema* goc_schema_real_min(double min) {
    goc_schema* s = goc_schema_make(SCHEMA_REAL_MIN);
    s->u.real_min = min;
    return s;
}

goc_schema* goc_schema_real_max(double max) {
    goc_schema* s = goc_schema_make(SCHEMA_REAL_MAX);
    s->u.real_max = max;
    return s;
}

goc_schema* goc_schema_real_range(double min, double max) {
    if (min > max) {
        ABORT("goc_schema_real_range: min > max (%g > %g)\n", min, max);
    }
    goc_schema* s = goc_schema_make(SCHEMA_REAL_RANGE);
    s->u.real_range.min = min;
    s->u.real_range.max = max;
    return s;
}

goc_schema* goc_schema_real_ex_min(double min) {
    goc_schema* s = goc_schema_make(SCHEMA_REAL_EX_MIN);
    s->u.real_min = min;
    return s;
}

goc_schema* goc_schema_real_ex_max(double max) {
    goc_schema* s = goc_schema_make(SCHEMA_REAL_EX_MAX);
    s->u.real_max = max;
    return s;
}

goc_schema* goc_schema_real_multiple(double factor) {
    if (!(factor > 0)) {
        ABORT("goc_schema_real_multiple: factor must be positive\n");
    }
    goc_schema* s = goc_schema_make(SCHEMA_REAL_MULTIPLE);
    s->u.real_factor = factor;
    return s;
}

goc_schema* goc_schema_str_min_len(size_t min) {
    goc_schema* s = goc_schema_make(SCHEMA_STR_MIN_LEN);
    s->u.str_len_min = min;
    return s;
}

goc_schema* goc_schema_str_max_len(size_t max) {
    goc_schema* s = goc_schema_make(SCHEMA_STR_MAX_LEN);
    s->u.str_len_max = max;
    return s;
}

goc_schema* goc_schema_str_len(size_t min, size_t max) {
    if (min > max) {
        ABORT("goc_schema_str_len: min > max (%zu > %zu)\n", min, max);
    }
    goc_schema* s = goc_schema_make(SCHEMA_STR_LEN);
    s->u.str_len.min = min;
    s->u.str_len.max = max;
    return s;
}

goc_schema* goc_schema_str_pattern(const char* pattern) {
    goc_schema* s = goc_schema_make(SCHEMA_STR_PATTERN);
    s->u.str_pattern = (char*)pattern;
    s->u.str_regex = (regex_t*)goc_malloc(sizeof(regex_t));
    if (regcomp(s->u.str_regex, pattern, REG_EXTENDED | REG_NOSUB) != 0) {
        ABORT("goc_schema_str_pattern: invalid regex '%s'\n", pattern);
    }
    return s;
}

goc_schema* goc_schema_str_enum(goc_array* vals) {
    goc_schema* s = goc_schema_make(SCHEMA_STR_ENUM);
    s->u.str_enum = vals;
    return s;
}

static const char* goc_schema_format_pattern(const char* format) {
    if (strcmp(format, "date-time") == 0) {
        return "^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\\.[0-9]+)?(Z|[+-][0-9]{2}:[0-9]{2})$";
    }
    if (strcmp(format, "date") == 0) {
        return "^[0-9]{4}-[0-9]{2}-[0-9]{2}$";
    }
    if (strcmp(format, "time") == 0) {
        return "^[0-9]{2}:[0-9]{2}:[0-9]{2}(\\.[0-9]+)?(Z|[+-][0-9]{2}:[0-9]{2})?$";
    }
    if (strcmp(format, "duration") == 0) {
        return "^P([0-9]+Y)?([0-9]+M)?([0-9]+D)?(T([0-9]+H)?([0-9]+M)?([0-9]+(\\.[0-9]+)?S)?)?$";
    }
    if (strcmp(format, "email") == 0) {
        return "^[^@[:space:]]+@[^@[:space:]]+\\.[^@[:space:]]+$";
    }
    if (strcmp(format, "uri") == 0) {
        return "^[a-zA-Z][a-zA-Z0-9+.-]*:[^[:space:]]+$";
    }
    if (strcmp(format, "uuid") == 0) {
        return "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$";
    }
    if (strcmp(format, "ipv4") == 0) {
        return "^([0-9]{1,3}\\.){3}[0-9]{1,3}$";
    }
    if (strcmp(format, "ipv6") == 0) {
        return "^([0-9a-fA-F]{1,4}:){7}[0-9a-fA-F]{1,4}$";
    }
    return NULL;
}

goc_schema* goc_schema_str_format(const char* format) {
    goc_schema* s = goc_schema_make(SCHEMA_STR_FORMAT);
    s->u.str_format_name = (char*)format;
    const char* pattern = goc_schema_format_pattern(format);
    if (pattern) {
        s->u.str_format_regex = (regex_t*)goc_malloc(sizeof(regex_t));
        if (regcomp(s->u.str_format_regex, pattern, REG_EXTENDED | REG_NOSUB) != 0) {
            ABORT("goc_schema_str_format: invalid regex for format '%s'\n", format);
        }
    } else {
        s->u.str_format_regex = NULL;
    }
    return s;
}

goc_schema* goc_schema_arr(goc_schema* elem) {
    goc_schema* s = goc_schema_make(SCHEMA_ARR);
    s->u.arr.elem = elem;
    return s;
}

goc_schema* goc_schema_arr_len(goc_schema* elem, size_t min, size_t max) {
    if (min > max) {
        ABORT("goc_schema_arr_len: min > max (%zu > %zu)\n", min, max);
    }
    goc_schema* s = goc_schema_make(SCHEMA_ARR_LEN);
    s->u.arr_len.elem = elem;
    s->u.arr_len.min = min;
    s->u.arr_len.max = max;
    return s;
}

goc_schema* goc_schema_arr_unique(goc_schema* elem) {
    goc_schema* s = goc_schema_make(SCHEMA_ARR_UNIQUE);
    s->u.arr_unique.elem = elem;
    return s;
}

goc_schema* goc_schema_arr_contains(goc_schema* elem,
                                    goc_schema* contains,
                                    size_t min_contains,
                                    size_t max_contains) {
    goc_schema* s = goc_schema_make(SCHEMA_ARR_CONTAINS);
    s->u.arr_contains.elem = elem;
    s->u.arr_contains.contains = contains;
    s->u.arr_contains.min_contains = min_contains;
    s->u.arr_contains.max_contains = max_contains;
    return s;
}

goc_schema* goc_schema_tuple(goc_array* items, goc_schema* additional_items) {
    goc_schema* s = goc_schema_make(SCHEMA_TUPLE);
    s->u.tuple.items = items;
    s->u.tuple.additional_items = additional_items;
    return s;
}

goc_schema* goc_schema_dict(goc_array* fields, goc_schema_dict_opts_t opts) {
    goc_schema* s = goc_schema_make(SCHEMA_OBJ);
    s->u.obj.fields = fields;
    s->u.obj.opts = opts;
    goc_schema_compile_pattern_props(opts.pattern_props);
    return s;
}

goc_schema* goc_schema_if(goc_schema* cond, goc_schema* then_, goc_schema* else_) {
    goc_schema* s = goc_schema_make(SCHEMA_IF);
    s->u.schemas = goc_array_make(3);
    goc_array_push(s->u.schemas, cond);
    goc_array_push(s->u.schemas, then_);
    goc_array_push(s->u.schemas, else_);
    return s;
}

goc_schema* goc_schema_not(goc_schema* schema) {
    goc_schema* s = goc_schema_make(SCHEMA_NOT);
    s->u.schemas = goc_array_make(1);
    goc_array_push(s->u.schemas, schema);
    return s;
}

goc_schema* goc_schema_predicate(bool (*fn)(void* val), const char* name) {
    goc_schema* s = goc_schema_make(SCHEMA_PREDICATE);
    s->u.predicate.fn   = fn;
    s->u.predicate.name = name;
    return s;
}

goc_schema* _goc_schema_any_of_impl(goc_array* schemas) {
    goc_schema* s = goc_schema_make(SCHEMA_ANY_OF);
    s->u.schemas = schemas;
    return s;
}

goc_schema* _goc_schema_one_of_impl(goc_array* schemas) {
    goc_schema* s = goc_schema_make(SCHEMA_ONE_OF);
    s->u.schemas = schemas;
    return s;
}

goc_schema* _goc_schema_all_of_impl(goc_array* schemas) {
    goc_schema* s = goc_schema_make(SCHEMA_ALL_OF);
    s->u.schemas = schemas;
    return s;
}

goc_schema_ref* goc_schema_ref_make(void) {
    goc_schema_ref* ref = (goc_schema_ref*)goc_malloc(sizeof(goc_schema_ref));
    ref->base.kind = SCHEMA_REF;
    ref->base.meta = NULL;
    ref->target = NULL;
    return ref;
}

void goc_schema_ref_set(goc_schema_ref* ref, goc_schema* schema) {
    if (!ref) return;
    if (ref->target != NULL) {
        GOC_DBG("goc_schema_ref_set: ref %p already resolved\n", (void*)ref);
        return;
    }
    ref->target = schema;
}

goc_schema* goc_schema_ref_get(goc_schema_ref* ref) {
    if (!ref) return NULL;
    return ref->target;
}

goc_schema_registry* goc_schema_registry_make(void) {
    goc_schema_registry* reg = (goc_schema_registry*)goc_malloc(sizeof(goc_schema_registry));
    reg->map = goc_dict_make(8);
    return reg;
}

void goc_schema_registry_add(goc_schema_registry* reg, const char* name, goc_schema* schema) {
    if (!reg || !name) return;
    char* key = goc_sprintf("%s", name);
    goc_dict_set(reg->map, key, schema);
}

goc_schema* goc_schema_registry_get(goc_schema_registry* reg, const char* name) {
    if (!reg || !name) return NULL;
    return (goc_schema*)goc_dict_get(reg->map, name, NULL);
}

goc_schema_registry* goc_schema_global_registry(void) {
    goc_schema_ensure_init();
    return s_global_registry;
}

goc_schema* goc_schema_with_meta(goc_schema* schema, goc_schema_meta_t meta) {
    if (!schema) return NULL;
    goc_schema_meta_t* stored = (goc_schema_meta_t*)goc_malloc(sizeof(goc_schema_meta_t));
    *stored = meta;
    schema->meta = stored;
    return schema;
}

goc_schema_meta_t* goc_schema_meta(goc_schema* schema) {
    if (!schema) return NULL;
    return schema->meta;
}

goc_schema_error* goc_schema_validate(goc_schema* schema, void* val) {
    if (!schema) ABORT("goc_schema_validate: NULL schema\n");
    return goc_schema_validate_impl(schema, val, "", false);
}

bool goc_schema_is_valid(goc_schema* schema, void* val) {
    return goc_schema_validate(schema, val) == NULL;
}

void goc_schema_check(goc_schema* schema, void* val) {
    goc_schema_error* err = goc_schema_validate(schema, val);
    if (!err) return;
    const char* path    = goc_schema_error_path(err);
    const char* message = goc_schema_error_message(err);
    if (!path[0]) path = "<root>";
    if (!message[0]) message = "validation failed";
    ABORT("goc_schema_check: validation failed at %s: %s\n", path, message);
}
