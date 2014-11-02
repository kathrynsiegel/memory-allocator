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

// Rounds up to the nearest multiple of ALIGNMENT.
#define ALIGN(size) (((size) + (ALIGNMENT-1)) & ~(ALIGNMENT-1))

// The smallest aligned size that will hold a size_t value.
#define SIZE_T_SIZE (ALIGN(sizeof(size_t)))

// typedef struct used_header_t {
//    size_t size;
// } used_header_t;
// #define HEADER_T_SIZE (ALLOC_ALIGN(sizeof(used_header_t)))

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

#define BUCKET_1 0x0
#define BUCKET_2 0x1
#define BUCKET_3 0x2
#define BUCKET_4 0x3
#define BUCKET_5 0x4
#define BUCKET_6 0x5
#define BUCKET_7 0x6
#define LAST_BUCKET BUCKET_7

#define SIZE_PTR(p) (void*)(((uint64_t)p&~0xF))
#define IS_SIZE32(p) ((uint8_t)(*(SIZE_PTR(p))) == BUCKET_1)
#define IS_SIZE64(p) ((uint8_t)(*(SIZE_PTR(p))) == BUCKET_2)
#define IS_SIZE128(p) ((uint8_t)(*(SIZE_PTR(p))) == BUCKET_3)
#define IS_SIZE256(p) ((uint8_t)(*(SIZE_PTR(p))) == BUCKET_4)
#define IS_SIZE512(p) ((uint8_t)(*(SIZE_PTR(p))) == BUCKET_5)
#define IS_SIZE1024(p) ((uint8_t)(*(SIZE_PTR(p))) == BUCKET_6)
#define IS_SIZE_BIG(p) ((uint8_t)(*(SIZE_PTR(p))) == BUCKET_7)

#define SIZE_32 32
#define SIZE_64 64
#define SIZE_128 128
#define SIZE_256 256
#define SIZE_512 512
#define SIZE_1024 1024

static int BUCKET_SIZES[LAST_BUCKET];

free_list_t *free_lists[];
free_list_t *free_list32;
free_list_t *free_list64;
free_list_t *free_list128;
free_list_t *free_list256;
free_list_t *free_list512;
free_list_t *free_list1024;

// init - Initialize the malloc package.  Called once before any other
// calls are made.  Since this is a very simple implementation, we just
// return success.
int my_init() {
  for (int i = BUCKET_1; i <= BUCKET_7; i++) {
    free_lists[i] = NULL;
  }
  free_list32 = NULL;
  free_list64 = NULL;
  free_list128 = NULL;
  free_list256 = NULL;
  free_list512 = NULL;
  free_list1024 = NULL;

  BUCKET_SIZES = {32, 64, 128, 256, 512, 1024};

  void *brk = mem_heap_hi() + 1;
  int req_size = CACHE_ALIGN(brk) - (uint64_t)brk;
  mem_sbrk(req_size);
  /* align brk just once */
  
  return 0;
}

int get_bucket_size(size_t size) {
  if (size < SIZE_32) {
    return BUCKET_1;
  } else if (size < SIZE_64) {
    return BUCKET_2;
  } else if (size < SIZE_128) {
    return BUCKET_3;
  } else if (size < SIZE_256) {
    return BUCKET_4;
  } else if (size < SIZE_512) {
    return BUCKET_5;
  } else if (size < SIZE_1024) {
    return BUCKET_6;
  } else {
    return BUCKET_7;
  }
}

//  malloc - Allocate a block by incrementing the brk pointer.
//  Always allocate a block whose size is a multiple of the alignment.
void * my_malloc(size_t size) {
  /* add to free list with different size now! */
  free_list_t** head = NULL;
  uint8_t bucket;
  void *p = NULL;
  int bucket_idx = get_bucket_size(size);

  if (free_lists[bucket_idx] != NULL) {
    head = &free_lists[bucket_idx];
    p = *head;
    *head = (*head)->next;
  } else {
    // Find an open bucket that is larger than the one we need
    int open_bucket;
    for (open_bucket = bucket_idx; open_bucket <= LAST_BUCKET; ++open_bucket) {
      if (free_lists[open_bucket] != NULL) {
        head = &free_lists[open_bucket];
        break;
      }
    }
    if (head != NULL) {
      // We have a free bucket, but it's too big: subdivide it and assign p.
      p = *head;
      subdivideBucket(size, head);
    } else {
      // If asking for large and there are small entries on free list
      // coalesce entries even if non-neighboring
      coalesceEntries(size, p);
    }
  } 

  // If p still has not been assigned, we need new heap space. 
  // allocate a new item
  if (!p) {
    p = alloc_aligned(size);
  }

  /*
  if (size < SIZE_32) {
    head = &free_list32;
    bucket = BUCKET_1;
  } else if (size < SIZE_64) {
    head = &free_list64;
    bucket = BUCKET_2;
  } else if (size < SIZE_128) {
    head = &free_list128;
    bucket = BUCKET_3;
  } else if (size < SIZE_256) {
    head = &free_list256;
    bucket = BUCKET_4;
  } else if (size < SIZE_512) {
    head = &free_list512;
    bucket = BUCKET_5;
  } else if (size < SIZE_1024) {
    head = &free_list1024;
    bucket = BUCKET_6;
  } else {
    bucket = BUCKET_7;
  }
  */

  if (p == (void *)-1) {
    // Whoops, an error of some sort occurred.  We return NULL to let
    // the client code know that we weren't able to allocate memory.
    return NULL;
  }

  // printf("smart_malloc %d -> %p\n", size, p);

  // increment pointer by four bytes
  p = (void*)((uint64_t)p + 0xF); //TODO

  // then fill that byte with info containing size
  void * sizeptr = SIZE_PTR(p);
  uint8_t size = (uint8_t)(*sizeptr);
  size = bucket;

  return p;
}

void * coalesceEntries(size_t size, void* p) {
  free_list_t* curr_free_list;
  size_t large_size;
  size_t small_size;
  // check if we can coalesce two smaller entries
  if (relocate_callback) {
    bool can_coalesce = false;

    for (int i = BUCKET_1; i < LAST_BUCKET-1; i++) {
      if (size < BUCKET_SIZES[i+1] && free_lists[i] && free_lists[i]->next) {
        curr_free_list = free_lists[i];
        large_size = BUCKET_SIZES[i+1];
        small_size = BUCKET_SIZES[i];
        can_coalesce = true;
      }
    }

    /*
    if (size < SIZE_64 && free_list32 && free_list32->next) {
      curr_free_list = free_list32;
      large_size = SIZE_64;
      small_size = SIZE_32;
    } else if (size < SIZE_128 && free_list64 && free_list64->next) {
      curr_free_list = free_list64;
      large_size = SIZE_128;
      small_size = SIZE_64;
    } else if (size < SIZE_256 && free_list128 && free_list128->next) {
      curr_free_list = free_list128;
      large_size = SIZE_256;
      small_size = SIZE_128;
    } else if (size < SIZE_512 && free_list256 && free_list256->next) {
      curr_free_list = free_list256
      large_size = SIZE_512
      small_size = SIZE_256; 
    } else if (size < SIZE_1024) {
      curr_free_list = free_list512;
      large_size = SIZE_1024;
      small_size = SIZE_512
    } else { // TODO(larger) 
      can_coalesce = false;
    }
    */

    if (can_coalesce) {
      /* two entries available in small list */
      /* force them to merge */
      p1 = curr_free_list;
      p2 = p1->next;
      /* find which one is smaller of p1 and p2,
      * use smallest to ensure its not beyond the end of brk */
      if (p1 > p2) {
        p1 = p2;
        p2 = curr_free_list;
      }
      // Remove the two buckets from the free list
      curr_free_list = curr_free_list->next->next;

      /* find the alternate, potentially live element */
      if (ALIGNED(p1, large_size)) {
        p = p1;
        p1 += small_size;
      } else {
        p1 -= small_size;
        p = p1;
      }
      assert(ALIGNED(p, CACHE_ALIGNMENT));

      /* RELOCATE should ignore us if the entry is no longer VALID */
      /* We could ask whether one or the other is a valid object */
      // Any object is assumed to be relocatable.
      if (relocate_callback(relocate_state, p1, p2)) {
        memcpy(p2, p1, small_size);
      } else {
        /* if not found, even better - item is already dead! */
      }
    }
  }
}

void * subdivideBucket(size_t size, void* p) {
  free_list_t* curr_free_list;
  size_t large_size;
  size_t small_size;
  // check if we can coalesce two smaller entries
  if (relocate_callback) {
    bool can_coalesce = false;

    for (int i = BUCKET_1; i < LAST_BUCKET-1; i++) {
      if (size < BUCKET_SIZES[i+1] && free_lists[i] && free_lists[i]->next) {
        curr_free_list = free_lists[i];
        large_size = BUCKET_SIZES[i+1];
        small_size = BUCKET_SIZES[i];
        can_coalesce = true;
      }
    }

    /*
    if (size < SIZE_64 && free_list32 && free_list32->next) {
      curr_free_list = free_list32;
      large_size = SIZE_64;
      small_size = SIZE_32;
    } else if (size < SIZE_128 && free_list64 && free_list64->next) {
      curr_free_list = free_list64;
      large_size = SIZE_128;
      small_size = SIZE_64;
    } else if (size < SIZE_256 && free_list128 && free_list128->next) {
      curr_free_list = free_list128;
      large_size = SIZE_256;
      small_size = SIZE_128;
    } else if (size < SIZE_512 && free_list256 && free_list256->next) {
      curr_free_list = free_list256
      large_size = SIZE_512
      small_size = SIZE_256; 
    } else if (size < SIZE_1024) {
      curr_free_list = free_list512;
      large_size = SIZE_1024;
      small_size = SIZE_512
    } else { // TODO(larger) 
      can_coalesce = false;
    }
    */

    if (can_coalesce) {
      /* two entries available in small list */
      /* force them to merge */
      p1 = curr_free_list;
      p2 = p1->next;
      /* find which one is smaller of p1 and p2,
      * use smallest to ensure its not beyond the end of brk */
      if (p1 > p2) {
        p1 = p2;
        p2 = curr_free_list;
      }
      // Remove the two buckets from the free list
      curr_free_list = curr_free_list->next->next;

      /* find the alternate, potentially live element */
      if (ALIGNED(p1, large_size)) {
        p = p1;
        p1 += small_size;
      } else {
        p1 -= small_size;
        p = p1;
      }
      assert(ALIGNED(p, CACHE_ALIGNMENT));

      /* RELOCATE should ignore us if the entry is no longer VALID */
      /* We could ask whether one or the other is a valid object */
      // Any object is assumed to be relocatable.
      if (relocate_callback(relocate_state, p1, p2)) {
        memcpy(p2, p1, small_size);
      } else {
        /* if not found, even better - item is already dead! */
      }
    }
  }

void * alloc_aligned(size_t size) {
  if (size < SIZE_32) {
    size = SIZE_32;
  } if (size < SIZE_64) {
    void *brk = mem_heap_hi() + 1;
    if (!ALIGNED(brk, SIZE_64)) {
       free_list_t *small = mem_sbrk(SIZE_32);
       small->next = free_list32;
       free_list32 = small;
    }
    size = SIZE_64;
  } else if (size < SIZE_128 && free_list64 && free_list64->next) {
    size = SIZE_128; 
  } else if (size < SIZE_256 && free_list128 && free_list128->next) {
    size = SIZE_256;
  } else if (size < SIZE_512 && free_list256 && free_list256->next) {
    size = SIZE_512;
  } else if (size < SIZE_1024) {
    size = SIZE_1024;
  } //else { // TODO(larger) 
  // }

  // align if needed
  void *brk = mem_heap_hi() + 1;
  int req_size = CACHE_ALIGN(brk) - (uint64_t)brk;
  if (req_size > 0) {
    mem_sbrk(req_size);
  }

  void *p = mem_sbrk(size);

  assert(ALIGNED(p, size));
  //  printf("alloc %p %ld\n", p, size);

  if (p == (void *)-1) {
    return NULL;
  }
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

  void * sizeptr = SIZE_PTR(p);
  uint8_t bucket = (uint8_t)(*sizeptr);
  switch (bucket) {
    case BUCKET_1:
      head = &free_list32;
      break;
    case BUCKET_2:
      head = &free_list64;
      break;
    case BUCKET_3:
      head = &free_list128;
      break;
    case BUCKET_4:
      head = &free_list256;
      break;
    case BUCKET_5:
      head = &free_list512;
      break;
    case BUCKET_6:
      head = &free_list1024;
      break;
    default:
      subdivideBucket(SIZE_1024, ptr);
      head = &free_list1024;
      break;
  }

  free_list_t* fn = sizeptr;
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
