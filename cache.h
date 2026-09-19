#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void cache_init(size_t max_bytes);
int cache_get(const char *key, char **out_data, size_t *out_size);
void cache_put(const char *key, const char *data, size_t size);
void cache_destroy(void);

#ifdef __cplusplus
}
#endif

#endif
