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

// relocate_callback_t relocate_callback = NULL;
// void* relocate_state = NULL;
// void smart_register_relocate_callback(relocate_callback_t f, void* state)
// {
//   relocate_callback = f;
//   relocate_state = state;
// }

#define MAX_SIZE_LOG_2 29
#define MIN_SIZE_LOG_2 5
#define NUM_BUCKETS MAX_SIZE_LOG_2 - MIN_SIZE_LOG_2
#define SIZE_CACHE_LINE 64

#define SIZE_PTR(p) (void*)(((uint64_t*)p-1))

#define BUCKET_SIZE(i) (1<<((i)+MIN_SIZE_LOG_2))

#define FITS_INTO_BUCKET(size, bucket_idx) ((size) <= (BUCKET_SIZE(bucket_idx)-8))

free_list_t *free_lists[NUM_BUCKETS];
/*
free_list_t *free_list32;
free_list_t *free_list64;
free_list_t *free_list128;
free_list_t *free_list256;
free_list_t *free_list512;
free_list_t *free_list1024;
*/

// init - Initialize the malloc package.  Called once before any other
// calls are made.  Since this is a very simple implementation, we just
// return success.
int my_init() {
  for (int i = 0; i < NUM_BUCKETS; i++) {
    free_lists[i] = NULL;
  }

  // for (int i = 0; i < NUM_BUCKETS; i++) {
  //   BUCKET_SIZE(i) = 1<<(5 + i);
  // }

  void *brk = mem_heap_hi() + 1;
  int req_size = CACHE_ALIGN(brk) - (uint64_t)brk;
  mem_sbrk(req_size);
  /* align brk just once */
  
  return 0;
}

int get_bucket_size(size_t size) {
  int i = 0;
  size >>= 5;
  while (size) {
    i++;
    size >>= 1;
  }
  return i;
}

//  malloc - Allocate a block by incrementing the brk pointer.
//  Always allocate a block whose size is a multiple of the alignment.
void * my_malloc(size_t size) {
  /* add to free list with different size now! */
  free_list_t** head = NULL;
  void *p = NULL;
  int bucket_idx = get_bucket_size(size);

  if (free_lists[bucket_idx] != NULL) {
    head = &free_lists[bucket_idx];
    p = *head;
    *head = (*head)->next;
  } else {
    // Find an open bucket that is larger than the one we need
    int open_bucket;
    for (open_bucket = bucket_idx + 1; open_bucket <= NUM_BUCKETS; ++open_bucket) {
      if (free_lists[open_bucket] != NULL) {
        head = &free_lists[open_bucket];
        break;
      }
    }
    // if (head != NULL) {
    //   // We have a free bucket, but it's too big: subdivide it and assign p.
    //   p = *head;
    //   subdivideBucket(size, open_bucket, *head);
    // } else {
      // If asking for large and there are small entries on free list
      // coalesce entries even if non-neighboring
      // coalesceEntries(size, p);
    //}
  } 

  // If p still has not been assigned, we need new heap space. 
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

  // fill header info and increment pointer by 8 bytes
  *(uint64_t*)p = bucket_idx;
  p = (void*)((uint64_t*)p + 1); 

  return p;
}

/*
 * Takes a size which it needs to allocate to pointer p.
 * Moves two smaller buckets next to each other and makes them into a larger
 * bucket. Recurses until there exists a bucket larger than size, and stores 
 * the address of the bucket in p.
 */
int coalesceEntries(size_t size, void* p) {
  free_list_t* cur_free_list;
  size_t large_size;
  size_t small_size;
  // check if we can coalesce two smaller entries
  // if (relocate_callback) {
    int can_coalesce = 0;

    for (int i = NUM_BUCKETS-2; i > 0; i--) {
      if (size < BUCKET_SIZE(i+1) && free_lists[i] && free_lists[i]->next) {
        cur_free_list = free_lists[i];
        large_size = BUCKET_SIZE(i+1);
        small_size = BUCKET_SIZE(i);
        can_coalesce = 1;
      }
    }

    // two entries are available in a smaller list: force them to merge
    if (can_coalesce) {
      free_list_t* p1 = cur_free_list;
      free_list_t* p2 = p1->next;

      /* find which one is smaller of p1 and p2,
       * use smallest to ensure its not beyond the end of brk */
      if (p1 > p2) {
        p1 = p2;
        p2 = cur_free_list;
      }
      // Remove the two buckets from the free list
      cur_free_list = cur_free_list->next->next;

      /* find the alternate, potentially live element */
      if (ALIGNED(p1, large_size)) {
        p = p1;
        p1 += small_size;
      } else {
        p1 -= small_size;
        p = p1;
      }
      // assert(ALIGNED(p, CACHE_ALIGNMENT));

      /* RELOCATE should ignore us if the entry is no longer VALID
       * We could ask whether one or the other is a valid object
       * Any object is assumed to be relocatable. */
      // if (relocate_callback(relocate_state, p1, p2)) {
        // memcpy(p2, p1, small_size);
      // } else {
        /* if not found, even better - item is already dead! */
      // }
      // Having reallocated, return TRUE if there is now a bucket large enough
      // to hold SIZE.
      if (large_size > size) {
        return 1;
      } 
      // Recurse otherwise.
      coalesceEntries(size, p);
    }
  // }

  // We failed to coalesce.
  return 0;
}

/*
 * Given a large bucket, divide it into chunks until it just barely holds SIZE.
 * Recursive: only divides bucket in half at any level.
 * 
 * size: the size of the object we need to fit
 * bucket_idx: the index of the smallest bucket we can find
 * bucketp: pointer to the start of the bucket
 */
void * subdivideBucket(size_t size, int bucket_idx, free_list_t* head) {
  // Handle when bucket size > 1024
  if (!FITS_INTO_BUCKET(size, bucket_idx)) {
    // make sure it's actually too big.
    printf("ERROR! subdivideBucket called with too small bucket!!");
    return NULL;
  } else if (bucket_idx == 0 || !FITS_INTO_BUCKET(size, bucket_idx-1)) {
    return NULL;
  }

  // Advance the pointer of the relevant free list, to let it know we're
  // stealing this chunk of memory
  free_lists[bucket_idx] = head->next;
  // our new size is smaller
  size_t new_bucket_size = BUCKET_SIZE(bucket_idx-1);
  // make room for the new bucket
  free_list_t* new_bucket = (free_list_t*)(head + new_bucket_size);
  // put the first smaller bucket on the stack
  new_bucket->next = free_lists[bucket_idx-1];
  free_lists[bucket_idx-1] = new_bucket;
  // put the reassigned head on the stack
  head->next = new_bucket;
  free_lists[bucket_idx-1] = head;

  bucket_idx -= 1;

  // If size is too big for the next smaller index, we're done
  if (bucket_idx == 0 || !FITS_INTO_BUCKET(size, bucket_idx))
    return NULL;

  // otherwise recurse
  subdivideBucket(size, bucket_idx, head);
  return NULL;
}

void * alloc_aligned(size_t size) {
  // TODO: this
  // for (int i = 0; i < NUM_BUCKETS; i++) {
  //   if (FITS_INTO_BUCKET(size, i))
  //     size = BUCKET_SIZE(i);
  // }
  // if (size < SIZE_CACHE_LINE/2) {
  //   void *brk = mem_heap_hi() + 1;
  //   if (!ALIGNED(brk, SIZE_CACHE_LINE)) {
  //      free_list_t *small = mem_sbrk(SIZE_CACHE_LINE/2);
  //      small->next = free_lists[0];
  //      free_lists[0] = small;
  //   }
  // }

  // // align if needed
  // void *brk = mem_heap_hi() + 1;
  // int req_size = CACHE_ALIGN(brk) - (uint64_t)brk;
  // if (req_size > 0) {
  //   mem_sbrk(req_size);
  // }

  void *p = mem_sbrk(size);

  // assert(ALIGNED(p, size));
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
  uint64_t * sizeptr = SIZE_PTR(ptr);
  uint64_t bucket = *sizeptr;
  free_list_t** head = &free_lists[bucket];
  free_list_t* fn = (free_list_t*) sizeptr;

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
