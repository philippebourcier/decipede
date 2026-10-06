#include "stack_mon.h"

#define MAGIC 0x5A5A5A5Au

static uint32_t main_stack[MAIN_STACK_SIZE / 4] __attribute__((aligned(8)));

void stack_run(int (*fn)(void)) {
    for (unsigned i = 0; i < MAIN_STACK_SIZE / 4; i++) main_stack[i] = MAGIC;
    uintptr_t bottom = (uintptr_t)main_stack;
    uintptr_t top = bottom + sizeof(main_stack);
    // Limit first (the current SP is above it), then move SP and call fn.
    __asm volatile(
        "msr msplim, %0\n"
        "mov sp, %1\n"
        "blx %2\n"
        "1: b 1b\n"
        :
        : "r"(bottom), "r"(top), "r"(fn)
        : "memory");
    __builtin_unreachable();
}

uint32_t stack_free_min(void) {
    unsigned i = 0;
    while (i < MAIN_STACK_SIZE / 4 && main_stack[i] == MAGIC) i++;
    return i * 4;
}
