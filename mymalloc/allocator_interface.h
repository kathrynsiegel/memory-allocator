/**
 * Copyright (c) 2012 MIT License by 6.172 Staff
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 **/

#include <stdlib.h>

#ifndef _ALLOCATOR_INTERFACE_H
#define _ALLOCATOR_INTERFACE_H

// Called by the memory allocator when it needs to relocate
// a potentially live object.  Can return NULL if old object is no longer needed.
// typedef void* (*relocate_callback_t)(void* state, void* old, void* new);

/* Function pointers for a malloc implementation.  This is used to allow a
 * single validator to operate on both libc malloc, a buggy malloc, and the
 * student "mm" malloc.
 */
typedef struct {
  int (*init)(void);
  void *(*malloc)(size_t size);
  void *(*realloc)(void *ptr, size_t size);
  void (*free)(void *ptr);
  int (*check)();
  void (*reset_brk)(void);
  void *(*heap_lo)(void);
  void *(*heap_hi)(void);

   /* non-standard API */
   // void (*register_relocate_callback)(relocate_callback_t f, void* state);

  char* name;
  char aligned;
  char smart;
} malloc_impl_t;

int libc_init();
void * libc_malloc(size_t size);
void * libc_realloc(void *ptr, size_t size);
void libc_free(void *ptr);
int libc_check();
void libc_reset_brk();
void * libc_heap_lo();
void * libc_heap_hi();

static const malloc_impl_t libc_impl =
{ .name = "libc", .aligned = 0,
  .init = &libc_init, .malloc = &libc_malloc, .realloc = &libc_realloc,
  .free = &libc_free, .check = &libc_check, .reset_brk = &libc_reset_brk,
  .heap_lo = &libc_heap_lo, .heap_hi = &libc_heap_hi};


/* alignment helpers, alignment must be power of 2 */
#define ALIGNED(x, alignment) ((((uint64_t)x) & ((alignment)-1)) == 0)
#define ALIGN_FORWARD(x, alignment) \
    ((((uint64_t)x) + ((alignment)-1)) & (~((uint64_t)(alignment)-1)))
#define ALIGN_BACKWARD(x, alignment) (((uint64_t)x) & (~((uint64_t)(alignment)-1)))
#define PAD(length, alignment) (ALIGN_FORWARD((length), (alignment)) - (length))
#define ALIGN_MOD(addr, size, alignment) \
    ((((uint64_t)addr)+(size)-1) & ((alignment)-1))
#define CROSSES_ALIGNMENT(addr, size, alignment) \
    (ALIGN_MOD(addr, size, alignment) < (size)-1)
/* number of bytes you need to shift addr forward so that it's !CROSSES_ALIGNMENT */
#define ALIGN_SHIFT_SIZE(addr, size, alignment) \
    (CROSSES_ALIGNMENT(addr, size, alignment) ?   \
        ((size) - 1 - ALIGN_MOD(addr, size, alignment)) : 0)

// All blocks must have a specified minimum alignment.
// The alignment requirement (from config.h) is >= 8 bytes.
#define ALLOC_ALIGNMENT 8
#define ALLOC_ALIGN(size) ALIGN_FORWARD(size, ALLOC_ALIGNMENT)

#define CACHE_ALIGNMENT 64
#define CACHE_ALIGN(size) ALIGN_FORWARD(size, CACHE_ALIGNMENT)

typedef struct free_list_t {
   struct free_list_t* next;
} free_list_t;

int my_init();
int get_bucket_size(size_t size);
void * my_malloc(size_t size);
int coalesceEntries(size_t size, void* p);
void subdivideBucket(size_t size, int bucket_idx, free_list_t* head);
void * alloc_aligned(int bucket_idx);
void * my_realloc(void *ptr, size_t size);
void my_free(void *ptr);
int my_check();
void my_reset_brk();
void * my_heap_lo();
void * my_heap_hi();

static const malloc_impl_t my_impl =
{ .init = &my_init, .malloc = &my_malloc, .realloc = &my_realloc,
  .free = &my_free, .check = &my_check, .reset_brk = &my_reset_brk,
  .heap_lo = &my_heap_lo, .heap_hi = &my_heap_hi};

int bad_init();
void * bad_malloc(size_t size);
void * bad_realloc(void *ptr, size_t size);
void bad_free(void *ptr);
int bad_check();
void bad_reset_brk();
void * bad_heap_lo();
void * bad_heap_hi();

static const malloc_impl_t bad_impl =
{ .init = &bad_init, .malloc = &bad_malloc, .realloc = &bad_realloc,
  .free = &bad_free, .check = &bad_check, .reset_brk = &bad_reset_brk,
  .heap_lo = &bad_heap_lo, .heap_hi = &bad_heap_hi};
// int wrapped_init();
// void * wrapped_malloc(size_t size);
// void * wrapped_realloc(void *ptr, size_t size);
// void wrapped_free(void *ptr);
// int wrapped_check();
// void wrapped_reset_brk();
// void * wrapped_heap_lo();
// void * wrapped_heap_hi();

// static const malloc_impl_t wrapped_impl =
// { .name = "wrapped", .aligned = 1,
//   .init = &wrapped_init, .malloc = &wrapped_malloc, .realloc = &wrapped_realloc,
//   .free = &wrapped_free, .check = &wrapped_check, .reset_brk = &wrapped_reset_brk,
//   .heap_lo = &wrapped_heap_lo, .heap_hi = &wrapped_heap_hi};

// int aligned_init();
// void * aligned_malloc(size_t size);
// void * aligned_realloc(void *ptr, size_t size);
// void aligned_free(void *ptr);
// int aligned_check();
// void aligned_reset_brk();
// void * aligned_heap_lo();
// void * aligned_heap_hi();

// static const malloc_impl_t aligned_impl =
// { .name = "aligned", .aligned = 1,
//   .init = &aligned_init, .malloc = &aligned_malloc, .realloc = &aligned_realloc,
//   .free = &aligned_free, .check = &aligned_check, .reset_brk = &aligned_reset_brk,
//   .heap_lo = &aligned_heap_lo, .heap_hi = &aligned_heap_hi};


int smart_init();
void * smart_malloc(size_t size);
void * smart_realloc(void *ptr, size_t size);
void smart_free(void *ptr);
int smart_check();
void smart_reset_brk();
void * smart_heap_lo();
void * smart_heap_hi();
// void smart_register_relocate_callback(relocate_callback_t f, void* state);

#define SMALL_SIZE 32
#define LARGE_SIZE 64
#define IS_LARGE(p) !(IS_SMALL(p))

#ifdef STARTER
#define YOUR_TASK(x) x
#define SMART_PTR(p) YOUR_TASK(p)
#define IS_SMALL(p) YOUR_TASK(0)
#else
/* XXX solution to be removed */
#define SMART_PTR(p) (void*)(((uint64_t)p)&~1)
#define IS_SMALL(p) (((uint64_t)p)&1)
#endif

// static const malloc_impl_t smart_impl =
// { .name = "smart", .aligned = 1, .smart = 1,
//   .init = &smart_init, .malloc = &smart_malloc, .realloc = &smart_realloc,
//   .free = &smart_free, .check = &smart_check, .reset_brk = &smart_reset_brk,
//   .heap_lo = &smart_heap_lo, .heap_hi = &smart_heap_hi,
//   .register_relocate_callback = &smart_register_relocate_callback};

// #define FIXED_SIZE LARGE_SIZE
// int fixed_init();
// void * fixed_malloc(size_t size);
// void * fixed_realloc(void *ptr, size_t size);
// void fixed_free(void *ptr);
// int fixed_check();
// void fixed_reset_brk();
// void * fixed_heap_lo();
// void * fixed_heap_hi();

// static const malloc_impl_t fixed_impl =
// { .name = "fixed", .aligned = 1,
//   .init = &fixed_init, .malloc = &fixed_malloc, .realloc = &fixed_realloc,
//   .free = &fixed_free, .check = &fixed_check, .reset_brk = &fixed_reset_brk,
//   .heap_lo = &fixed_heap_lo, .heap_hi = &fixed_heap_hi};

#endif  // _ALLOCATOR_INTERFACE_H
