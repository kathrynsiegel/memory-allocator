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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "./allocator_interface.h"
#include "./memlib.h"

// Don't call libc malloc!
#define malloc(...) (USE_MY_MALLOC)
#define free(...) (USE_MY_FREE)
#define realloc(...) (USE_MY_REALLOC)

// All blocks must have a specified minimum alignment.
// The alignment requirement (from config.h) is >= 8 bytes.
#ifndef ALIGNMENT
#define ALIGNMENT 8
#endif

// Trace classes are numbered 0..10. Use default value of -1.
#ifndef TRACE_CLASS
#define TRACE_CLASS -1
#endif

#define SMART_PTR(p) (void*)(((uint64_t)p)&~1)
#define SMART_PTR(p) (void*)(((uint64_t)p)&~1)
#define IS_SMALL(p) (((uint64_t)p)&1)


// Rounds up to the nearest multiple of ALIGNMENT.
#define ALIGN(size) (((size) + (ALIGNMENT-1)) & ~(ALIGNMENT-1))

// The smallest aligned size that will hold a size_t value.
#define SIZE_T_SIZE (ALIGN(sizeof(size_t)))

typedef struct used_header_t {
   size_t size;
} used_header_t;
#define HEADER_T_SIZE (ALLOC_ALIGN(sizeof(used_header_t)))

typedef struct free_list_t {
   struct free_list_t* next;
} free_list_t;

relocate_callback_t relocate_callback = NULL;
void* relocate_state = NULL;
void smart_register_relocate_callback(relocate_callback_t f, void* state)
{
   relocate_callback = f;
   relocate_state = state;
}

free_list_t *free_list16;
free_list_t *free_list32;
free_list_t *free_list64;
free_list_t *free_list128;
free_list_t *free_list256;
free_list_t *free_list512;
free_list_t *free_list1024;

// check - This checks our invariant that the size_t header before every
// block points to either the beginning of the next block, or the end of the
// heap.
int my_check() {
  char *p;
  char *lo = (char*)mem_heap_lo();
  char *hi = (char*)mem_heap_hi() + 1;
  size_t size = 0;

  p = lo;
  while (lo <= p && p < hi) {
    size = ALIGN(*(size_t*)p + SIZE_T_SIZE);
    p += size;
  }

  if (p != hi) {
    printf("Bad headers did not end at heap_hi!\n");
    printf("heap_lo: %p, heap_hi: %p, size: %lu, p: %p\n", lo, hi, size, p);
    return -1;
  }

  return 0;
}

// init - Initialize the malloc package.  Called once before any other
// calls are made.  Since this is a very simple implementation, we just
// return success.
int my_init() {
  void *brk = mem_heap_hi() + 1;
  int req_size = CACHE_ALIGN(brk) - (uint64_t)brk;
  mem_sbrk(req_size);
  /* align brk just once */
  free_list16 = NULL;
  free_list32 = NULL;
  free_list64 = NULL;
  free_list128 = NULL;
  free_list256 = NULL;
  free_list512 = NULL;
  free_list1024 = NULL;

  relocate_callback = NULL;
  return 0;
}

void * alloc_aligned(size_t size) {

  if (size > 32) {
    void *brk = mem_heap_hi() + 1;
    if (!ALIGNED(brk, 64)) {
       free_list_t *small = mem_sbrk(32);
       small->next = free_list32;
       free_list32 = small;
    }
    size = 64;
  } else if (size > 16) {
    size = 32;
  } else {
    size = 16;
  }

  void *p = mem_sbrk(size);
  assert(ALIGNED(p, size));
//  printf("alloc %p %ld\n", p, size);

  if (p == (void *)-1) {
    return NULL;
  }
  return p;
}

//  malloc - Allocate a block by incrementing the brk pointer.
//  Always allocate a block whose size is a multiple of the alignment.
void * my_malloc(size_t size) {
  /* add to free list with different size now! */
  free_list_t** head;
  if (size <= 16) {
    head = &free_list16;
  } else if (size <= 32) {
    head = &free_list32;
  } else {
    head = &free_list64;
  }
  void *p = NULL;
  if (*head) {
    p = *head;
    *head = (*head)->next;

    printf("smart_malloc from free list\n");
  }

  // If asking for large and there are small entries on free list
  // coalesce entries even if non-neighboring!  (NOTE TO STUDENTS:
  // You did not have to implement this part for the homework, but
  // the code is included for your reference.)
  if (!p && size > SMALL_SIZE && relocate_callback) {
    if (free_list32 && free_list32->next) {
      /* two entries available in small list */
      /* force them to merge */
      free_list_t* p1 = free_list32;
      free_list_t* p2 = p1->next;
      /* find which one is smaller of p1 and p2,
      * use smallest to ensure its not beyond the end of brk */
      if (p1 > p2) {
        p1 = p2;
        p2 = free_list32;
      }
      free_list32 = free_list32->next->next;

      /* find the alternate, potentially live element */
      if (ALIGNED(p1, LARGE_SIZE)) {
        p = p1;
        p1 += SMALL_SIZE;
      } else {
        p1 -= SMALL_SIZE;
        p = p1;
      }
      assert(ALIGNED(p, CACHE_ALIGNMENT));

      /* RELOCATE should ignore us if the entry is no longer VALID */
      /* We could ask whether one or the other is a valid object */
      // Any object is assumed to be relocatable.
      if (relocate_callback(relocate_state, p1, p2)) {
        memcpy(p2, p1, SMALL_SIZE);
      } else {
        /* if not found, even better - item is already dead! */
      }
    }
  }

  // allocate a new item
  if (!p) {
    p = alloc_aligned(size);
  }

  if (p == (void *)-1) {
    // Whoops, an error of some sort occurred.  We return NULL to let
    // the client code know that we weren't able to allocate memory.
    return NULL;
  }

  // printf("smart_malloc %d -> %p\n", size, p);

  if (size <= SMALL_SIZE)
    p = (void*)((uint64_t)p | 1);
  return p;
}

// free - Freeing a block does nothing.
void my_free(void *ptr) {
  if (ptr == NULL) {
    return;
  }
  /* add to free list with different size now! */
  free_list_t** head;
  // printf("smart_free %ld -> %p\n", IS_SMALL(p) ? 32 : 64, p);

  if (IS_SMALL(ptr)) {
    head = &free_list32;
  } else {
    head = &free_list64;
  }
  free_list_t* fn = SMART_PTR(ptr);
  fn->next = *head;
  *head = fn;
}

// realloc - Implemented simply in terms of malloc and free
void * my_realloc(void *ptr, size_t size) {
  void *newptr;
  size_t copy_size;

  // Allocate a new chunk of memory, and fail if that allocation fails.
  newptr = my_malloc(size);
  if (NULL == newptr)
    return NULL;

  // Get the size of the old block of memory.  Take a peek at my_malloc(),
  // where we stashed this in the SIZE_T_SIZE bytes directly before the
  // address we returned.  Now we can back up by that many bytes and read
  // the size.
  copy_size = *(size_t*)((uint8_t*)ptr - SIZE_T_SIZE);

  // If the new block is smaller than the old one, we have to stop copying
  // early so that we don't write off the end of the new block of memory.
  if (size < copy_size)
    copy_size = size;

  // This is a standard library call that performs a simple memory copy.
  memcpy(newptr, ptr, copy_size);

  // Release the old block.
  my_free(ptr);

  // Return a pointer to the new block.
  return newptr;
  // return NULL;
}

// call mem_reset_brk.
void my_reset_brk() {
  mem_reset_brk();
}

// call mem_heap_lo
void * my_heap_lo() {
  return mem_heap_lo();
}

// call mem_heap_hi
void * my_heap_hi() {
  return mem_heap_hi();
}
