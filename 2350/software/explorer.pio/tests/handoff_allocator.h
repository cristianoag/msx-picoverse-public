#include <stddef.h>
void *handoff_malloc(size_t size);
void *handoff_calloc(size_t count, size_t size);
void handoff_free(void *ptr);
