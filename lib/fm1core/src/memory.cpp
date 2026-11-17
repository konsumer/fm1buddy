#include "memory.h"

#include <stdlib.h>

bool lowMemory = false;

bool roomFor(size_t n) {
  void* p = malloc(n);
  free(p);
  return p;
}
