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
#define HEADER_SIZE SIZE_T_SIZE
#define MAX_SIZE_LOG_2 29
#define MIN_SIZE_LOG_2 5
#define NUM_BUCKETS MAX_SIZE_LOG_2 - MIN_SIZE_LOG_2
#define SIZE_CACHE_LINE 64
#define BUCKET_SIZE(i) ((1 << ((i) + MIN_SIZE_LOG_2)) - (HEADER_SIZE))
#define FITS_INTO_BUCKET(size, bucket_idx) ((size) <= (BUCKET_SIZE(bucket_idx)))

typedef struct free_list_t {
  size_t bucket_i;
  struct free_list_t* next;
} free_list_t;

int get_bucket_size(size_t size);
int coalesceEntries(size_t size, void* p);
void subdivideBucket(size_t size, int bucket_idx);
void * alloc_aligned(int bucket_idx);

free_list_t *free_lists[NUM_BUCKETS];

// init - Initialize the malloc package.  Called once before any other
// calls are made.  
int my_init() {
  for (int i = 0; i < NUM_BUCKETS; i++) {
    free_lists[i] = NULL;
  }

  /* align brk just once */
  void *brk = mem_heap_hi() + 1;
  int req_size = CACHE_ALIGN(brk) - (uint64_t)brk;
  mem_sbrk(req_size);
  
  return 0;
}

// The bucket size is the ceiling of log(size).
// Note that we leave room for an 8 byte header.
int get_bucket_size(size_t size) {
  int i = 0;
  size += HEADER_SIZE - 1; // room for 8 byte header
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
  /* add to free list with different size */
  free_list_t** head = NULL;
  void *p = NULL;
  int bucket_idx = get_bucket_size(size);
  
  if (free_lists[bucket_idx] != NULL) {
    // If there is a bucket of exactly the right size available, we're good
    head = &free_lists[bucket_idx];
    p = *head;
    free_lists[bucket_idx] = free_lists[bucket_idx]->next;

  } else {

    // Find an open bucket that is larger than the one we need
    int open_bucket;
    for (open_bucket = bucket_idx + 1; open_bucket <= NUM_BUCKETS; ++open_bucket) {
      if (free_lists[open_bucket] != NULL) {
        head = &free_lists[open_bucket];
        break;
      }
    }

    // If we have a free bucket, but it's too big, subdivide it and assign p.
    if (head != NULL) {
      // split a larger bucket into appropriately-sized chunks
      subdivideBucket(size, open_bucket);

      // there should now be a bucket of the right size: assign p.
      p = free_lists[bucket_idx];
      free_lists[bucket_idx] = free_lists[bucket_idx]->next;
      
      // something went wrong if P is null
      if (p == NULL)
        return NULL;
    }
  } 

  // If p still has not been assigned, we need new heap space. 
  if (!p) {
    // allocate a new item.
    p = alloc_aligned(bucket_idx);
  }

  if (p == (void *)-1) {
    // an error occurred, and we couldn't allocate memory.
    return NULL;
  }

  // fill header info and increment the pointer by HEADER_SIZE
  // we make a free_list, but don't actually use it: the cast is strictly to
  // align the header in the right place.
  free_list_t* new_list = (free_list_t*)p;
  new_list->bucket_i = bucket_idx;
  return (void*)((char*)p + HEADER_SIZE);
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
  int can_coalesce = 0;

  for (int i = NUM_BUCKETS-2; i > 0; i--) {
    if (FITS_INTO_BUCKET(size, i+1) && free_lists[i] && free_lists[i]->next) {
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
  // We failed to coalesce.
  return 0;
}

/*
 * Given a large bucket, divide it into chunks until it just barely holds SIZE.
 * Recursive: only divides bucket in half at any level.
 * 
 * size: the size of the object we need to fit
 * big_bucket_i: the index of the big bucket to divide
 */
void subdivideBucket(size_t size, int big_bucket_i) {
  // Advance the pointer of the relevant free list, to let it know we're
  // stealing this chunk of memory
  free_list_t *big_bucket_head = free_lists[big_bucket_i];
  free_lists[big_bucket_i] = free_lists[big_bucket_i]->next;
  int small_bucket_i = big_bucket_i - 1;

  // our new size is smaller
  size_t new_bucket_size = BUCKET_SIZE(small_bucket_i);
  
  // make room for the new bucket: make sure we increment by the total required 
  // size (including the header)
  free_list_t* new_bucket = (free_list_t*)((char*)big_bucket_head + 
      (new_bucket_size + HEADER_SIZE));

  // put the first smaller bucket on the stack
  new_bucket->next = free_lists[small_bucket_i];
  free_lists[small_bucket_i] = new_bucket;

  // put the reassigned head on the stack
  // big_bucket_head is now a small bucket
  big_bucket_head->next = new_bucket;
  free_lists[small_bucket_i] = big_bucket_head;

  // If size is too big for the next smaller index, we're done
  if (big_bucket_i == 1 || size > BUCKET_SIZE(small_bucket_i-1)) {
    return;
  }

  // otherwise recurse
  subdivideBucket(size, small_bucket_i);
}

/*
 * Called when we need to increase the heap size.
 * Grows the heap by enough memory to hold a bucket of category BUCKET_IDX.
 */
void *alloc_aligned(int bucket_idx) {
  void *p = mem_sbrk(BUCKET_SIZE(bucket_idx) + HEADER_SIZE);
  if (p == (void *)-1) {
    return NULL;
  }
  return p;
}

/* 
 * free - Frees a block, and adds its space to a linked list of freed buckets for
 * reuse.
 */
void my_free(void *ptr) {
  if (ptr == NULL) {
    return;
  }
  // Cast the pointer to a free list pointer - this means including the header
  // we'd previously ignored
  free_list_t * flist = (free_list_t*)((char*)ptr - HEADER_SIZE);
  int bucket_i = flist->bucket_i;

  // push it onto the stack of free lists for this bucket size
  flist->next = free_lists[bucket_i];
  free_lists[bucket_i] = flist;
}

/*
 * realloc - Given an allocated chunk, move its contents to a new memory block
 * large enough to hold SIZE. Implemented simply in terms of malloc and free.
 */
void * my_realloc(void *ptr, size_t size) {
  void *newptr;

  // Allocate a new chunk of memory, and fail if that allocation does.
  newptr = my_malloc(size);
  if (newptr == NULL)
    return NULL;

  // Get the size of the old block of memory.  Take a peek at my_malloc(),
  // where we stashed this in the HEADER_SIZE bytes directly before the
  // address we returned.  Now we can back up by that many bytes and read
  // the bucket type.
  free_list_t * flist = (free_list_t*)((char*)ptr - HEADER_SIZE);
  int bucket_i = flist->bucket_i;
  size_t copy_size = BUCKET_SIZE(bucket_i);

  //printf("realloc to size %d, from %p (bucket %d) to %p (bucket %d)\n",
  //    (int)size, ptr, bucket_i, newptr, get_bucket_size(size));

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
}

// XXX: This is never used.
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
    size = ALIGN(*(size_t*)p + HEADER_SIZE);
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
