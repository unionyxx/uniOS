#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

int atoi(const char *str);
unsigned long strtoul(const char *nptr, char **endptr, int base);

void srand(unsigned int seed);
int rand(void);

#ifdef __cplusplus
}
#endif
