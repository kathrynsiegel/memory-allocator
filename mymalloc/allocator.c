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


// the linked list data structure that holds the blocks we want to free
typedef struct free_list_t {
  unsigned int bucket_num: 30;
  unsigned int prev_bucket_num: 30;
  unsigned int is_free: 2;
  struct free_list_t* next;
} free_list_t;

int get_bucket_size(size_t size);
void coalesceEntries(free_list_t* list);
void subdivideBucket(size_t size, free_list_t* head);
void * alloc_aligned(int bucket_idx);
void coalesceHelper(free_list_t* list_a, free_list_t* list_b);

free_list_t *free_lists[NUM_BUCKETS];
free_list_t * heap_top;

// init - Initialize the malloc package.  Called once before any other
// calls are made.  
int my_init() {
  for (int i = 0; i < NUM_BUCKETS; i++) {
    free_lists[i] = NULL;
  }
  heap_top = NULL;

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
  void *p = NULL;
  int bucket_idx = get_bucket_size(size);
  
  if (free_lists[bucket_idx] != NULL) {
    // If there is a bucket of exactly the right size available, we're good
    p = free_lists[bucket_idx];
    free_lists[bucket_idx] = free_lists[bucket_idx]->next;

  } else {
    
    // Find an open bucket that is larger than the one we need
    int open_bucket;
    for (open_bucket = bucket_idx + 1; open_bucket < NUM_BUCKETS; ++open_bucket) {
      if (free_lists[open_bucket] != NULL) {
        break;
      }
    }

    // If we have a free bucket, but it's too big, subdivide it and assign p.
    if (open_bucket < NUM_BUCKETS) {
      // split a larger bucket into appropriately-sized chunks
      subdivideBucket(size, free_lists[open_bucket]);

      // there should now be a bucket of the right size: assign p.
      p = free_lists[bucket_idx];
      free_lists[bucket_idx] = free_lists[bucket_idx]->next;
      
      // something went wrong if P is null
      if (p == NULL)
        return NULL;
    }
  } 

  free_list_t* new_list;

  // If p still has not been assigned, we need new heap space. 
  if (p) {
    if (p == (void *)-1) {
      // an error occurred, and we couldn't allocate memory.
      return NULL;
    }

    // p is first cast to a free_list_t* in order to store metadata
    new_list = (free_list_t*)p;

  } else {
    // allocate a new item.
    p = alloc_aligned(bucket_idx);

    // TODO: maybe do this inside alloc_aligned?
    new_list = (free_list_t*)p;

    if (heap_top != NULL) {
      new_list->prev_bucket_num = heap_top->bucket_num;
    }
  }

  // fill header info and increment the pointer by HEADER_SIZE
  // we made a free_list, but don't use it like a free_list: the cast is strictly to
  // align the header in the right place.
  new_list->bucket_num = bucket_idx;  // set size
  new_list->is_free = 0;  // in use
  return (void*)((char*)p + HEADER_SIZE);
}

/* 
 * Remove a bucket from a linked list, using the power of O(n) search.
 * TODO: A doubly-linked free list would be immensely helpful
 */
void removeFromFreeList(free_list_t* bucket, free_list_t** list) {
  if (bucket == *list) {
    // if this bucket is at the head of its list, great!
    *list = (*list)->next;
    return;
  }

  // otherwise we have to iterate through everything...
  free_list_t* prev = *list;
  while (prev->next != bucket) {
    // if we hit the end of the list, something is wrong.
    if (prev->next == NULL) {
      printf("Error: bucket %p not found in list %p.\n", bucket, list);
      return;
    }
    prev = prev->next;
  }
  
  // cut BUCKET out from the LL, but leave the LIST head untouched.
  prev->next = bucket->next;
}

/*
 * Given a large bucket, divide it into chunks until it just barely holds SIZE.
 * Recursive: only divides bucket in half at any level.
 * 
 * size: the size of the object we need to fit
 * head: the big bucket to divide
 */
void subdivideBucket(size_t size, free_list_t* head) {
  int big_bucket_i = head->bucket_num;
  int small_bucket_i = big_bucket_i - 1;
  
  // cut out HEAD from its free list
  removeFromFreeList(head, &free_lists[big_bucket_i]);

  // make room for the first new bucket: jump forward in memory by the total required 
  // size (including the header)
  free_list_t* new_bucket = (free_list_t*)((char*)head + 
      BUCKET_SIZE(small_bucket_i) + HEADER_SIZE);

  // set fields for the first new bucket
  new_bucket->bucket_num = small_bucket_i;
  new_bucket->prev_bucket_num = small_bucket_i;
  new_bucket->is_free = 0x1;  // free = true

  // find the bucket after this new bucket
  free_list_t* bucket_after = (free_list_t*)((char*)head + 
      BUCKET_SIZE(big_bucket_i) + HEADER_SIZE);

  // tell it that we are now smaller
  bucket_after->prev_bucket_num = small_bucket_i;
  
  // put the first smaller bucket on the stack
  new_bucket->next = free_lists[small_bucket_i];

  // add it to the front of the free list
  free_lists[small_bucket_i] = new_bucket;

  // set fields for the reassigned head
  head->bucket_num = small_bucket_i;

  // put the reassigned head on the stack
  // *head is now a small bucket
  head->next = new_bucket;
  free_lists[small_bucket_i] = head;

  // If size is too big for the next smaller index, we're done
  if (big_bucket_i == 1 || size > BUCKET_SIZE(small_bucket_i-1)) {
    return;
  }

  // otherwise recurse
  subdivideBucket(size, head);
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
  int bucket_num = flist->bucket_num;
  flist->is_free = 0x1;

  // push it onto the stack of free lists for this bucket size
  flist->next = free_lists[bucket_num];
  free_lists[bucket_num] = flist;
  
  // coalesce entries now
  coalesceEntries(flist);
}

/*
 * Coalesces two adjacent free buckets and makes them into a larger
 * bucket. Recurses.
 */
void coalesceEntries(free_list_t* list) {
  int prev_bucket_num = list->prev_bucket_num;
  int b_num = list->bucket_num;

  // Check the bucket behind this one
  if (prev_bucket_num == b_num) {
    free_list_t* prev_list = (free_list_t*)((char*)list -
        (BUCKET_SIZE(b_num) + HEADER_SIZE));

    if (prev_list->is_free == 0x1) {
      coalesceHelper(prev_list, list);
    }

  } else {
    // check the bucket in front
    free_list_t* next_list = (free_list_t*)((char*)list + 
        BUCKET_SIZE(b_num) + HEADER_SIZE);

    if (next_list->is_free == 0x1 && next_list->bucket_num == b_num) {
      coalesceHelper(list, next_list);
    }
  }
}

/*
 * Takes two buckets of the same size, adjacent in memory, removes them 
 * both from their free list, and joins them into a larger bucket.
 */
void coalesceHelper(free_list_t* list_a, free_list_t* list_b) {
  int bucket_num = list_a->bucket_num;
  int new_bucket_num = bucket_num + 1;

  // remove both from bucket_idx
  // again with the O(n) search... TODO a doubly-linked list
  removeFromFreeList(list_a, &free_lists[bucket_num]);
  removeFromFreeList(list_b, &free_lists[bucket_num]);

  /*
  // old logic to do the same thing (probably a little faster)
  free_list_t* bucket_list = free_lists[bucket_num];
  free_list_t* prev_item = NULL;

  while (bucket_list != NULL) {
    if (bucket_list == list_a || bucket_list == list_b) {
      if (prev_item != NULL) {
        prev_item->next = bucket_list->next;
      } else {
        free_lists[bucket_num] = free_lists[bucket_num]->next;
      }

    } else {
      prev_item = bucket_list;
    }

    bucket_list = bucket_list->next;
  }
  */

  // update size of first
  list_a->bucket_num = new_bucket_num;

  // update prev_size of the node after list_b
  free_list_t* next_list = (free_list_t*)((char*)list_b +
      BUCKET_SIZE(bucket_num) + HEADER_SIZE);

  next_list->prev_bucket_num = new_bucket_num;
  
  // add to bucket bucket_num + 1
  free_list_t* new_bucket_list = free_lists[new_bucket_num];
  list_a->next = new_bucket_list;
  free_lists[new_bucket_num] = list_a;
}

/*
 * realloc - Given an allocated chunk, move its contents to a new memory block
 * large enough to hold SIZE. Implemented simply in terms of malloc and free.
 */
void * my_realloc(void *ptr, size_t size) {
  void *newptr;

  // Get the size of the old block of memory.
  free_list_t * flist = (free_list_t*)((char*)ptr - HEADER_SIZE);
  int bucket_num = flist->bucket_num;
  size_t old_size = BUCKET_SIZE(bucket_num);

  // If the new block is smaller than the old one, do nothing.
  if (size < old_size)
    return ptr;

  //printf("realloc to size %d, from %p (bucket %d) to %p (bucket %d)\n",
  //    (int)size, ptr, bucket_i, newptr, get_bucket_size(size));

  // Allocate a new chunk of memory, and fail if that allocation does.
  newptr = my_malloc(size);
  if (newptr == NULL)
    return NULL;

  // This is a standard library call that performs a simple memory copy.
  memcpy(newptr, ptr, old_size);

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
