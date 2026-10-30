#ifndef STORE_LOGIC_H
#define STORE_LOGIC_H
#include <stddef.h>
#include <stdint.h>

#define STORE_CATEGORY_LIMIT 8
#define STORE_CATEGORY_LEN 32

/* Add a unique, non-empty catalog category, preserving first-seen order.
   Returns 1 if added, 0 if empty/duplicate/full or arguments are invalid. */
int store_category_add(char categories[STORE_CATEGORY_LIMIT][STORE_CATEGORY_LEN],
                       int *count, const char *category);

void store_format_size(uint64_t bytes, char *out, size_t outlen);
/* Derive the ms0: destination for a catalog item.
   Returns 0 on success, -1 if the type is not installable, -2 if the path
   does not fit in `out`. */
int store_derive_path(const char *url, const char *title,
                      char *out, size_t outlen);

#endif
