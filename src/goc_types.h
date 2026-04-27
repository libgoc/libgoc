/* src/goc_types.h
 *
 * Shared internal libgoc type layouts for implementation files.
 * This header is not installed and is used only by libgoc internals.
 */

#ifndef GOC_TYPES_H
#define GOC_TYPES_H

#include <stddef.h>
#include <stdbool.h>

/* Forward declare goc_array instead of importing public header to avoid
 * circular dependency from include/goc_array.h. */
typedef struct goc_array goc_array;

struct goc_array {
    void** data;
    size_t head;
    size_t len;
    size_t cap;
};

struct goc_dict {
    void**     table;
    char**     table_keys;
    size_t     table_cap;
    size_t     occupied;
    goc_array* keys;
    size_t len;
};

#endif /* GOC_TYPES_H */
