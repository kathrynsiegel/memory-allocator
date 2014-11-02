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
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "./allocator_interface.h"
#include "./memlib.h"

// Don't call libc malloc!
#define malloc(...) (USE_ALIGNED_MALLOC)
#define free(...) (USE_ALIGNED_FREE)
#define realloc(...) (USE_ALIGNED_REALLOC)

typedef struct used_header_t {
   size_t size;
} used_header_t;
#define HEADER_T_SIZE (ALLOC_ALIGN(sizeof(used_header_t)))

typedef struct free_list_t {
   struct free_list_t* next;
} free_list_t;

free_list_t *free_list;

// aligned_init - Does nothing.
int aligned_init() {
  return 0;
}

// aligned_check - No checker.
int aligned_check() {
  return 1;
}

// aligned_malloc - Allocate a block by incrementing the brk pointer.
// Always allocate a block whose size is not a multiple of the alignment,
// and may not fit the requested allocation size.
void * aligned_malloc(size_t size) {
   int aligned_size = ALLOC_ALIGN(size + HEADER_T_SIZE);
   void *p = NULL;
   used_header_t *hdr;
   void *brk = mem_heap_hi() + 1;
   // what is the minimum cache line occupation of size

   // if the object will cross cache lines anyways we don't have to put the header
   // on a separate cache line
   if (CACHE_ALIGN(size) ==
       (ALIGN_FORWARD(brk + aligned_size, CACHE_ALIGNMENT) -
        ALIGN_BACKWARD(brk + sizeof(used_header_t), CACHE_ALIGNMENT))) {
      p = mem_sbrk(aligned_size);
      if (p == (void *) -1) {
         return NULL;
      }

      hdr = p;
      p += sizeof(used_header_t);
   } else {
      int req_size = CACHE_ALIGN(brk + sizeof(used_header_t)) - (uint64_t)brk +
         ALLOC_ALIGN(size);
      p = mem_sbrk(req_size);
      if (p == (void *) -1) {
         return NULL;
      }

      p = (void*)ALIGN_FORWARD(p, CACHE_ALIGNMENT);
      // We store the size of the block we've allocated in the 
      // HEADER_T_SIZE bytes just before the pointer
      hdr = p - sizeof(used_header_t);
   }

  // allocate header on one cache line
  // but it will have to be at the END of the cache line
  // how would we know where was the original brk located?
  // we are missing padding bytes!!
   hdr->size = size;
   return p;
}

void
aligned_free(void* p) {
  free_list_t *fn = p;
  fn->next = free_list;
  free_list = fn;
}

void * aligned_realloc(void *ptr, size_t size) {
  /* no-op if same size! */
  return ptr;
}

// call mem_reset_brk.
void aligned_reset_brk() {
  mem_reset_brk();
}

// call mem_heap_lo
void * aligned_heap_lo() {
  return mem_heap_lo();
}

// call mem_heap_hi
void * aligned_heap_hi() {
  return mem_heap_hi();
}
