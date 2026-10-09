// Compiles on x86 AND x64; the static_asserts pin the layout both sides rely on.
#include <cstddef>
#include <cstdio>
#include "pizzarivals_protocol.h"
static_assert(sizeof(PREvent) == 128, "event size");
static_assert(sizeof(PRInput) == 8, "input size");
static_assert(sizeof(PRSolid) == 20, "solid size");
static_assert(sizeof(PRHitbox) == 32, "hitbox size");
static_assert(sizeof(void*) == 4 || sizeof(void*) == 8, "");
int main() { std::printf("sizeof(PRShared)=%zu shm=%zu\n", sizeof(PRShared), (size_t)PR_SHM_SIZE); }
