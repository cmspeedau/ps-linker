#ifndef STORE_LOGIC_H
#define STORE_LOGIC_H
#include <stddef.h>
#include <stdint.h>

void store_format_size(uint64_t bytes, char *out, size_t outlen);
/* Derive the ms0: destination for a catalog item.
   Returns 0 on success, -1 if the type is not installable, -2 if the path
   does not fit in `out`. */
int store_derive_path(const char *url, const char *title,
                      char *out, size_t outlen);

#endif
