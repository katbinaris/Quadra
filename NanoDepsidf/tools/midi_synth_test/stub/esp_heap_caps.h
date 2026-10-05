#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define heap_caps_malloc(n, caps) malloc(n)
#define heap_caps_calloc(n, size, caps) calloc(n, size)
#define heap_caps_free(p) free(p)
