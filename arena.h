#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>

typedef struct {
    uint8_t *beg;
    uint8_t *end;
    uint8_t *limit;

    // jump buffer in case of OOM
    jmp_buf *oom;
} Arena;

enum {
    ARENA_SOFTFAIL = 1 << 0,
    ARENA_NOZERO = 1 << 1,
};

void arena_init(Arena *arena, void *memory, ptrdiff_t size);
void *arena_alloc(Arena *arena, ptrdiff_t size, ptrdiff_t align, ptrdiff_t count, uint32_t flags);
void arena_free(void *context, void *ptr, ptrdiff_t size);
void arena_clear(Arena *arena);

#define ARENA_ALLOC(arena, type, count, flags) ((type *)  arena_alloc(arena, sizeof(type), __alignof__(type), count, flags))
#define ARENA_ALLOC_ARRAY_EX(arena, type, count, flags) ARENA_ALLOC(arena, type, count, flags)
#define ARENA_ALLOC_ARRAY(arena, type, count) ARENA_ALLOC_ARRAY_EX(arena, type, count, 0)
#define ARENA_ALLOC1_EX(arena, type, flags) ARENA_ALLOC(arena, type, 1, flags)
#define ARENA_ALLOC1(arena, type) ARENA_ALLOC1_EX(arena, type, 0)

#endif /* ARENA_H */

#ifdef ARENA_IMPLEMENTATION

#include <assert.h>
#include <string.h>

static inline uintptr_t align_forward(uintptr_t start, ptrdiff_t align) {
    // `align` MUST be a power of two.
    return (start + align - 1) & ~(align - 1);
}

void arena_init(Arena *arena, void *memory, ptrdiff_t size) {
    arena->beg = memory;
    arena->end = arena->beg + size;
    arena->limit = arena->end;
    arena->oom = NULL;
}

#if defined(__clang__) || defined(__GNUC__)
__attribute__((malloc, alloc_size(2, 4), alloc_align(3)))
#endif
void *arena_alloc(Arena *arena, ptrdiff_t size, ptrdiff_t align, ptrdiff_t count, uint32_t flags) {
    // `align` must be a power of two.
    ptrdiff_t total = size * count;
    ptrdiff_t avail = arena->end - arena->beg;
    ptrdiff_t padding = -total & (align - 1);
    if (total > avail - padding) {
        if (flags & ARENA_SOFTFAIL) {
            return NULL;
        } else if (arena->oom) {
            longjmp(*arena->oom, 1);
        } else {
            abort();
        }
    }
    arena->end -= total + padding;
    void *result = arena->end;
    if (!(flags & ARENA_NOZERO)) {
        memset(result, 0, total);
    }
    return result;
}

void arena_clear(Arena *arena) {
    arena->end = arena->limit;
}

#endif /* ARENA_IMPLEMENTATION */
