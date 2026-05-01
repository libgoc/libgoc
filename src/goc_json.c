/* src/goc_json.c
 *
 * JSON parser and serializer for libgoc values.
 *
 * Copyright (c) Divyansh Prakash
 */

#include <assert.h>
#include <complex.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "goc_schema_internal.h"
#include "../include/goc_json.h"
#include "yyjson.h"

typedef yyjson_mut_val* (*goc_json_to_json_internal_fn)(yyjson_mut_doc*, void*);

struct goc_json_error {
    const char* path;
    const char* message;
};

static goc_json_error* goc_json_error_new(const char* path, const char* message) {
    goc_json_error* err = (goc_json_error*)goc_malloc(sizeof(goc_json_error));
    err->path = path ? path : "";
    err->message = goc_strdup(message ? message : "");
    return err;
}

const char* goc_json_error_message(const goc_json_error* err) {
    if (!err) return "";
    return err->message ? err->message : "";
}

const char* goc_json_error_path(const goc_json_error* err) {
    if (!err) return "";
    return err->path ? err->path : "";
}

static goc_json_result goc_json_result_ok(void* res) {
    return (goc_json_result){ .res = res, .err = NULL };
}

static goc_json_result goc_json_result_err(goc_json_error* err) {
    return (goc_json_result){ .res = NULL, .err = err };
}

static _Thread_local goc_schema* goc_json_current_schema = NULL;
static _Thread_local goc_schema* goc_json_current_schema_owner = NULL;
static _Thread_local const char* goc_json_current_path = NULL;
static _Thread_local goc_json_error** goc_json_err_out = NULL;

static void goc_json_error_set(const char* message) {
    if (goc_json_err_out) {
        *goc_json_err_out = goc_json_error_new(goc_json_current_path, message);
    }
}

static _Atomic int g_goc_json_initialized = 0;

static yyjson_mut_val* goc_json_serialize_value(yyjson_mut_doc* doc,
                                                goc_schema* schema,
                                                void* val,
                                                const char* path,
                                                goc_json_error** err_out);

static void goc_json_init_builtin_methods(void);

static void goc_json_ensure_init(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_goc_json_initialized,
                                                &expected, 1,
                                                memory_order_acq_rel,
                                                memory_order_relaxed)) {
        goc_json_init_builtin_methods();
    }
}

void goc_json_set_methods(goc_schema* schema,
                          goc_json_to_json_fn to_json,
                          goc_json_read_json_fn read_json) {
    if (!schema) return;
    if (to_json) {
        goc_json_to_json_set(schema, to_json);
    }
    if (read_json) {
        goc_json_read_json_set(schema, read_json);
    }
}

static void* goc_json_method_at(const goc_schema* schema,
                                const char* method_name,
                                const goc_schema** owner) {
    const goc_schema* cur = schema;
    while (cur) {
        if (cur->methods) {
            void* fn = goc_dict_get(cur->methods, method_name, NULL);
            if (fn) {
                if (owner) *owner = cur;
                return fn;
            }
        }
        cur = cur->parent;
    }
    if (owner) *owner = NULL;
    return NULL;
}

static yyjson_mut_val* goc_json_to_json_null(yyjson_mut_doc* doc, void* val) {
    return yyjson_mut_null(doc);
}

static yyjson_mut_val* goc_json_to_json_bool(yyjson_mut_doc* doc, void* val) {
    return yyjson_mut_bool(doc, goc_unbox(bool, val));
}

static yyjson_mut_val* goc_json_to_json_byte(yyjson_mut_doc* doc, void* val) {
    return yyjson_mut_sint(doc, goc_unbox(char, val));
}

static yyjson_mut_val* goc_json_to_json_ubyte(yyjson_mut_doc* doc, void* val) {
    return yyjson_mut_uint(doc, goc_unbox(unsigned char, val));
}

static yyjson_mut_val* goc_json_to_json_int(yyjson_mut_doc* doc, void* val) {
    return yyjson_mut_sint(doc, goc_unbox(int64_t, val));
}

static yyjson_mut_val* goc_json_to_json_uint(yyjson_mut_doc* doc, void* val) {
    return yyjson_mut_uint(doc, goc_unbox(uint64_t, val));
}

static yyjson_mut_val* goc_json_to_json_real(yyjson_mut_doc* doc, void* val) {
    return yyjson_mut_real(doc, goc_unbox(double, val));
}

static yyjson_mut_val* goc_json_to_json_str(yyjson_mut_doc* doc, void* val) {
    return yyjson_mut_strcpy(doc, (char*)val);
}

static yyjson_mut_val* goc_json_to_json_any(yyjson_mut_doc* doc, void* val) {
    char* repr = goc_sprintf("%p", val);
    return yyjson_mut_strcpy(doc, repr);
}

static char* goc_json_double_to_json(double v) {
    char* s = goc_sprintf("%.17g", v);
    if (!strchr(s, '.') && !strchr(s, 'e') && !strchr(s, 'E')) {
        return goc_sprintf("%s.0", s);
    }
    return s;
}

static char* goc_json_to_json_complex(void* val) {
    double complex z = goc_unbox(double complex, val);
    const char* schema_name = "";
    if (goc_json_current_schema && goc_json_current_schema->name) {
        schema_name = goc_json_current_schema->name;
    } else if (goc_json_current_schema_owner && goc_json_current_schema_owner->name) {
        schema_name = goc_json_current_schema_owner->name;
    }
    char* real_str = goc_json_double_to_json(creal(z));
    char* imag_str = goc_json_double_to_json(cimag(z));
    return goc_sprintf(
        "{\"goc_schema\":\"%s\",\"goc_value\":{\"real\":%s,\"imag\":%s}}",
        schema_name, real_str, imag_str);
}

static goc_schema* goc_json_arr_elem_schema(goc_schema* schema) {
    switch (schema->kind) {
        case SCHEMA_ARR: return schema->u.arr.elem;
        case SCHEMA_ARR_LEN: return schema->u.arr_len.elem;
        case SCHEMA_ARR_UNIQUE: return schema->u.arr_unique.elem;
        case SCHEMA_ARR_CONTAINS: return schema->u.arr_contains.elem;
        default: return NULL;
    }
}

static yyjson_mut_val* goc_json_to_json_array(yyjson_mut_doc* doc, void* val) {
    if (!goc_schema_is_goc_array(val)) {
        goc_json_error_set("expected goc_array for array schema");
        return NULL;
    }
    goc_array* arr = (goc_array*)val;
    goc_schema* elem_schema = goc_json_arr_elem_schema(goc_json_current_schema);
    if (!elem_schema || elem_schema == goc_schema_any()) {
        goc_json_error_set("array element type unknown");
        return NULL;
    }
    yyjson_mut_val* json_arr = yyjson_mut_arr(doc);
    size_t len = goc_array_len(arr);
    for (size_t i = 0; i < len; i++) {
        void* item = goc_array_get(arr, i);
        const char* current_path = goc_json_current_path ? goc_json_current_path : "";
        char* sub_path = goc_sprintf("%s.[%zu]", current_path, i);
        yyjson_mut_val* json_item = goc_json_serialize_value(doc, elem_schema, item, sub_path, goc_json_err_out);
        if (!json_item) return NULL;
        if (!yyjson_mut_arr_add_val(json_arr, json_item)) return NULL;
    }
    return json_arr;
}

static yyjson_mut_val* goc_json_to_json_tuple(yyjson_mut_doc* doc, void* val) {
    if (!goc_schema_is_goc_array(val)) {
        goc_json_error_set("expected goc_array for tuple schema");
        return NULL;
    }
    goc_array* arr = (goc_array*)val;
    goc_array* items = goc_json_current_schema->u.tuple.items;
    goc_schema* additional = goc_json_current_schema->u.tuple.additional_items;
    yyjson_mut_val* json_arr = yyjson_mut_arr(doc);
    size_t len = goc_array_len(arr);
    size_t nitems = items ? goc_array_len(items) : 0;
    for (size_t i = 0; i < len; i++) {
        goc_schema* item_schema = NULL;
        if (i < nitems) {
            item_schema = ((goc_schema_item_t*)goc_array_get(items, i))->schema;
        } else {
            item_schema = additional;
        }
        if (!item_schema) {
            goc_json_error_set("tuple element type unknown");
            return NULL;
        }
        void* item = goc_array_get(arr, i);
        const char* current_path = goc_json_current_path ? goc_json_current_path : "";
        char* sub_path = goc_sprintf("%s.[%zu]", current_path, i);
        yyjson_mut_val* json_item = goc_json_serialize_value(doc, item_schema, item, sub_path, goc_json_err_out);
        if (!json_item) return NULL;
        if (!yyjson_mut_arr_add_val(json_arr, json_item)) return NULL;
    }
    return json_arr;
}

static yyjson_mut_val* goc_json_to_json_dict(yyjson_mut_doc* doc, void* val) {
    if (!goc_schema_is_goc_dict(val)) {
        goc_json_error_set("expected goc_dict for dict schema");
        return NULL;
    }
    goc_dict* dict = (goc_dict*)val;
    if (!goc_json_current_schema->u.obj.fields || goc_array_len(goc_json_current_schema->u.obj.fields) == 0) {
        goc_json_error_set("dictionary schema has no field type information");
        return NULL;
    }
    yyjson_mut_val* json_obj = yyjson_mut_obj(doc);
    goc_array* entries = goc_dict_entries(dict);
    size_t len = goc_array_len(entries);
    for (size_t i = 0; i < len; i++) {
        goc_dict_entry_t* entry = goc_array_get(entries, i);
        goc_schema* field_schema = NULL;
        goc_array* fields = goc_json_current_schema->u.obj.fields;
        size_t nfields = goc_array_len(fields);
        for (size_t j = 0; j < nfields; j++) {
            goc_schema_field_t* field = goc_array_get(fields, j);
            if (strcmp(field->key, entry->key) == 0) {
                field_schema = field->schema;
                break;
            }
        }
        if (!field_schema) {
            field_schema = goc_schema_any();
        }
        const char* current_path = goc_json_current_path ? goc_json_current_path : "";
        char* sub_path = goc_sprintf("%s.%s", current_path, entry->key);
        yyjson_mut_val* json_val = goc_json_serialize_value(doc, field_schema, entry->val, sub_path, goc_json_err_out);
        if (!json_val) return NULL;
        if (!yyjson_mut_obj_add_val(doc, json_obj, entry->key, json_val)) return NULL;
    }
    return json_obj;
}

static yyjson_mut_val* goc_json_serialize_value(yyjson_mut_doc* doc,
                                                goc_schema* schema,
                                                void* val,
                                                const char* path,
                                                goc_json_error** err_out);

static void goc_json_init_builtin_methods(void);

static void* goc_json_complex_read_json(void* val) {
    if (!goc_schema_is_goc_dict(val)) return NULL;
    goc_dict* value_obj = (goc_dict*)val;
    double real = goc_dict_get_unboxed(double, value_obj, "real", 0.0);
    double imag = goc_dict_get_unboxed(double, value_obj, "imag", 0.0);
    return goc_box(double complex, real + imag * I);
}

static yyjson_mut_val* goc_json_serialize_runtime(yyjson_mut_doc* doc, void* val, goc_json_error** err_out);

static yyjson_mut_val* goc_json_array_to_json(yyjson_mut_doc* doc, void* val, goc_json_error** err_out) {
    goc_array* arr = (goc_array*)val;
    yyjson_mut_val* json_arr = yyjson_mut_arr(doc);
    size_t len = goc_array_len(arr);
    for (size_t i = 0; i < len; i++) {
        void* item = goc_array_get(arr, i);
        yyjson_mut_val* json_item = goc_json_serialize_runtime(doc, item, err_out);
        if (!json_item) return NULL;
        yyjson_mut_arr_add_val(json_arr, json_item);
    }
    return json_arr;
}

static yyjson_mut_val* goc_json_dict_to_json(yyjson_mut_doc* doc, void* val, goc_json_error** err_out) {
    goc_dict* dict = (goc_dict*)val;
    yyjson_mut_val* json_obj = yyjson_mut_obj(doc);
    goc_array* entries = goc_dict_entries(dict);
    size_t len = goc_array_len(entries);
    for (size_t i = 0; i < len; i++) {
        goc_dict_entry_t* entry = goc_array_get(entries, i);
        yyjson_mut_val* json_val = goc_json_serialize_runtime(doc, entry->val, err_out);
        if (!json_val) return NULL;
        yyjson_mut_obj_add_val(doc, json_obj, entry->key, json_val);
    }
    return json_obj;
}

static yyjson_mut_val* goc_json_serialize_runtime(yyjson_mut_doc* doc, void* val, goc_json_error** err_out) {
    if (!val) return yyjson_mut_null(doc);
    if (goc_schema_is_boxed_bool(val)) {
        return yyjson_mut_bool(doc, goc_unbox(bool, val));
    }
    if (goc_schema_is_boxed_byte(val)) {
        return yyjson_mut_sint(doc, goc_unbox(char, val));
    }
    if (goc_schema_is_boxed_ubyte(val)) {
        return yyjson_mut_uint(doc, goc_unbox(unsigned char, val));
    }
    if (goc_schema_is_boxed_int(val)) {
        return yyjson_mut_sint(doc, goc_unbox(int64_t, val));
    }
    if (goc_schema_is_boxed_uint(val)) {
        return yyjson_mut_uint(doc, goc_unbox(uint64_t, val));
    }
    if (goc_schema_is_boxed_real(val)) {
        return yyjson_mut_real(doc, goc_unbox(double, val));
    }
    if (goc_schema_is_boxed_complex(val)) {
        long double complex z = goc_unbox(long double complex, val);
        yyjson_mut_val* obj = yyjson_mut_obj(doc);
        yyjson_mut_val* payload = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_real(doc, payload, "real", creal(z));
        yyjson_mut_obj_add_real(doc, payload, "imag", cimag(z));
        yyjson_mut_obj_add_strcpy(doc, obj, "goc_schema", "goc/complex");
        yyjson_mut_obj_add_val(doc, obj, "goc_value", payload);
        return obj;
    }
    if (goc_schema_is_goc_array(val)) {
        return goc_json_array_to_json(doc, val, err_out);
    }
    if (goc_schema_is_goc_dict(val)) {
        return goc_json_dict_to_json(doc, val, err_out);
    }
    if (goc_schema_val_is_str(val)) {
        return yyjson_mut_strcpy(doc, (char*)val);
    }
    *err_out = goc_json_error_new(goc_json_current_path ? goc_json_current_path : "", "unsupported value type for JSON serialization");
    return NULL;
}

static yyjson_mut_val* goc_json_parse_json_string(yyjson_mut_doc* doc,
                                                    const char* json_str,
                                                    goc_json_error** err_out) {
    yyjson_read_err json_err;
    yyjson_doc* tmp = yyjson_read_opts((char*)json_str, strlen(json_str), 0, NULL, &json_err);
    if (!tmp) {
        if (err_out) *err_out = goc_json_error_new(goc_json_current_path ? goc_json_current_path : "", json_err.msg ? json_err.msg : "invalid JSON from to_json callback");
        return NULL;
    }
    yyjson_val* root = yyjson_doc_get_root(tmp);
    if (!root) {
        yyjson_doc_free(tmp);
        if (err_out) *err_out = goc_json_error_new(goc_json_current_path ? goc_json_current_path : "", "generated JSON has no root");
        return NULL;
    }
    yyjson_mut_val* result = yyjson_val_mut_copy(doc, root);
    yyjson_doc_free(tmp);
    if (!result && err_out) *err_out = goc_json_error_new(goc_json_current_path ? goc_json_current_path : "", "failed to copy JSON node from to_json callback");
    return result;
}

static yyjson_mut_val* goc_json_serialize_value(yyjson_mut_doc* doc,
                                                goc_schema* schema,
                                                void* val,
                                                const char* path,
                                                goc_json_error** err_out) {
    if (!schema) {
        if (err_out) *err_out = goc_json_error_new(path, "schema is required");
        return NULL;
    }

    const goc_schema* owner = NULL;
    goc_json_to_json_fn method = goc_json_to_json_get(schema);

    const char* saved_path = goc_json_current_path;
    goc_schema* saved_schema = goc_json_current_schema;
    goc_schema* saved_owner = goc_json_current_schema_owner;
    goc_json_error** saved_err_out = goc_json_err_out;
    goc_json_current_path = path ? path : "";
    goc_json_current_schema = schema;
    goc_json_current_schema_owner = (goc_schema*)(owner ? owner : schema);
    goc_json_err_out = err_out;

    yyjson_mut_val* result = NULL;
    if (method) {
        char* json_str = method(val);
        if (!json_str) {
            if (err_out) *err_out = goc_json_error_new(goc_json_current_path ? goc_json_current_path : "", "to_json callback returned NULL");
        } else {
            result = goc_json_parse_json_string(doc, json_str, err_out);
        }
    } else {
        void* internal_method = goc_json_method_at(schema, "_goc_json_to_json", &owner);
        if (internal_method) {
            result = ((goc_json_to_json_internal_fn)internal_method)(doc, val);
        } else if (err_out) {
            *err_out = goc_json_error_new(goc_json_current_path ? goc_json_current_path : "", "no serializer registered for schema");
        }
    }

    goc_json_current_path = saved_path;
    goc_json_current_schema = saved_schema;
    goc_json_current_schema_owner = saved_owner;
    goc_json_err_out = saved_err_out;
    return result;
}

static void* goc_json_value_from_yyjson(yyjson_doc* doc,
                                        yyjson_val* json,
                                        goc_json_error** err_out) {
    if (yyjson_is_null(json)) {
        return NULL;
    }
    if (yyjson_is_bool(json)) {
        return goc_box(bool, yyjson_get_bool(json));
    }
    if (yyjson_is_uint(json)) {
        return goc_box(uint64_t, yyjson_get_uint(json));
    }
    if (yyjson_is_sint(json)) {
        return goc_box(int64_t, yyjson_get_sint(json));
    }
    if (yyjson_is_real(json)) {
        return goc_box(double, yyjson_get_real(json));
    }
    if (yyjson_is_str(json)) {
        return goc_strdup(yyjson_get_str(json));
    }
    if (yyjson_is_arr(json)) {
        goc_array* arr = goc_array_make(0);
        yyjson_arr_iter iter = yyjson_arr_iter_with(json);
        yyjson_val* item;
        while ((item = yyjson_arr_iter_next(&iter))) {
            void* parsed = goc_json_value_from_yyjson(doc, item, err_out);
            if (*err_out) return NULL;
            goc_array_push(arr, parsed);
        }
        return arr;
    }
    if (yyjson_is_obj(json)) {
        goc_dict* dict = goc_dict_make(0);
        yyjson_obj_iter iter = yyjson_obj_iter_with(json);
        yyjson_val* key;
        while ((key = yyjson_obj_iter_next(&iter))) {
            const char* field = yyjson_get_str(key);
            yyjson_val* field_value = yyjson_obj_iter_get_val(key);
            void* parsed = goc_json_value_from_yyjson(doc, field_value, err_out);
            if (*err_out) return NULL;
            goc_dict_set(dict, goc_strdup(field), parsed);
        }

        void* schema_name_val = goc_dict_get(dict, "goc_schema", NULL);
        if (schema_name_val && goc_schema_val_is_str(schema_name_val)) {
            const char* schema_name = (const char*)schema_name_val;
            goc_schema* schema = goc_schema_lookup(schema_name);
            if (schema) {
            goc_json_read_json_fn read_fn = goc_json_read_json_get(schema);
            if (read_fn) {
                void* goc_value = goc_dict_get(dict, "goc_value", NULL);
                void* result = read_fn(goc_value);
                if (!goc_schema_is_valid(schema, result)) {
                    const char* schema_name = goc_schema_name(schema);
                    if (!schema_name) schema_name = "<unnamed>";
                    char* message = goc_sprintf("tagged object parse failed for schema '%s'", schema_name);
                    *err_out = goc_json_error_new("", message);
                    return NULL;
                }
                return result;
            }
        }
        }
        return dict;
    }

    *err_out = goc_json_error_new("", "unsupported JSON value");
    return NULL;
}

static void goc_json_init_builtin_methods(void) {
    goc_schema_method_set(goc_schema_null(),   "_goc_json_to_json", (void*)goc_json_to_json_null);
    goc_schema_method_set(goc_schema_bool(),   "_goc_json_to_json", (void*)goc_json_to_json_bool);
    goc_schema_method_set(goc_schema_byte(),   "_goc_json_to_json", (void*)goc_json_to_json_byte);
    goc_schema_method_set(goc_schema_ubyte(),  "_goc_json_to_json", (void*)goc_json_to_json_ubyte);
    goc_schema_method_set(goc_schema_int(),    "_goc_json_to_json", (void*)goc_json_to_json_int);
    goc_schema_method_set(goc_schema_uint(),   "_goc_json_to_json", (void*)goc_json_to_json_uint);
    goc_schema_method_set(goc_schema_real(),   "_goc_json_to_json", (void*)goc_json_to_json_real);
    goc_schema_method_set(goc_schema_str(),    "_goc_json_to_json", (void*)goc_json_to_json_str);
    goc_schema_method_set(goc_schema_arr_any(), "_goc_json_to_json", (void*)goc_json_to_json_array);
    goc_schema_method_set(goc_schema_dict_any(), "_goc_json_to_json", (void*)goc_json_to_json_dict);
    goc_json_set_methods(goc_schema_complex(), goc_json_to_json_complex, goc_json_complex_read_json);
    goc_schema_method_set(goc_schema_any(),    "_goc_json_to_json", (void*)goc_json_to_json_any);
}

static goc_json_result goc_json_stringify_internal(goc_schema* schema, void* val, bool pretty) {
    if (!schema) {
        return goc_json_result_err(goc_json_error_new("", "schema is required"));
    }

    goc_json_ensure_init();

    if (!goc_schema_is_valid(schema, val)) {
        goc_schema_error* schema_err = goc_schema_validate(schema, val);
        const char* path = schema_err ? goc_schema_error_path(schema_err) : "";
        const char* message = schema_err ? goc_schema_error_message(schema_err)
                                        : "value does not satisfy schema";
        return goc_json_result_err(goc_json_error_new(path, message));
    }

    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return goc_json_result_err(goc_json_error_new("", "failed to allocate JSON document"));
    }

    goc_json_error* err = NULL;
    yyjson_mut_val* root = goc_json_serialize_value(doc, schema, val, "", &err);
    if (!root) {
        yyjson_mut_doc_free(doc);
        return goc_json_result_err(err ? err : goc_json_error_new("", "serialization failed"));
    }

    yyjson_mut_doc_set_root(doc, root);
    char* buf;
    if (pretty) {
        buf = yyjson_mut_write_opts(doc, YYJSON_WRITE_PRETTY, NULL, NULL, NULL);
    } else {
        buf = yyjson_mut_write(doc, 0, NULL);
    }
    if (!buf) {
        yyjson_mut_doc_free(doc);
        return goc_json_result_err(goc_json_error_new("", "JSON write failed"));
    }

    char* result = goc_strdup(buf);
    free(buf);
    yyjson_mut_doc_free(doc);
    return goc_json_result_ok(result);
}

static goc_json_result goc_json_parse_internal(const char* json) {
    if (!json) {
        return goc_json_result_err(goc_json_error_new("", "json input is required"));
    }

    yyjson_read_err json_err;
    yyjson_doc* doc = yyjson_read_opts((char*)json, strlen(json), 0, NULL, &json_err);
    if (!doc) {
        return goc_json_result_err(goc_json_error_new("", json_err.msg ? json_err.msg : "invalid JSON"));
    }

    goc_json_error* err = NULL;
    void* res = goc_json_value_from_yyjson(doc, yyjson_doc_get_root(doc), &err);
    yyjson_doc_free(doc);
    if (err) {
        return goc_json_result_err(err);
    }
    return goc_json_result_ok(res);
}

goc_json_result goc_json_parse(const char* json) {
    goc_json_ensure_init();
    return goc_json_parse_internal(json);
}

goc_json_result goc_json_stringify(goc_schema* schema, void* val) {
    goc_json_ensure_init();
    return goc_json_stringify_internal(schema, val, false);
}

goc_json_result goc_json_stringify_pretty(goc_schema* schema, void* val) {
    goc_json_ensure_init();
    return goc_json_stringify_internal(schema, val, true);
}
