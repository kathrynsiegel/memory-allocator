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
#define MAX_SIZE_LOG_2 29
#define MIN_SIZE_LOG_2 5
#define NUM_BUCKETS MAX_SIZE_LOG_2 - MIN_SIZE_LOG_2
#define SIZE_CACHE_LINE 64
#define BUCKET_SIZE(i) (1<<((i)+MIN_SIZE_LOG_2))
#define FITS_INTO_BUCKET(size, bucket_idx) ((size) <= (BUCKET_SIZE(bucket_idx)-8))


// the linked list data structure that holds the blocks we want to free
typedef struct free_list_t {
  unsigned int bucket_num: 30;
  unsigned int prev_bucket_idx: 30;
  unsigned int is_free: 2;
  struct free_list_t* next;
} free_list_t;

int get_bucket_size(size_t size);
void coalesceEntries(free_list_t* list);
void subdivideBucket(size_t size, int bucket_idx, free_list_t* head);
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
  size += 8; // room for 8 byte header
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
    head = &free_lists[bucket_idx];
    p = *head;
    free_lists[bucket_idx] = free_lists[bucket_idx]->next;
  } else {
    // Find an open bucket that is larger than the one we need
    int open_bucket;
    for (open_bucket = bucket_idx + 1; open_bucket <= NUM_BUCKETS; ++open_bucket) {
      if (free_lists[open_bucket] != NULL) {
        head = &free_lists[open_bucket];
        // We have a free bucket, but it's too big: subdivide it and assign p.
        p = *head;
        subdivideBucket(size, open_bucket, *head);
        break;
      }
    }
  } 

  // If p still has not been assigned, we need new heap space. 
  // allocate a new item
  if (!p) {
    p = alloc_aligned(bucket_idx);
  }

  if (p == (void *)-1) {
    // Whoops, an error of some sort occurred.  We return NULL to let
    // the client code know that we weren't able to allocate memory.
    return NULL;
  }

  // fill header info and increment pointer by 8 bytes
  free_list_t* new_list = (free_list_t*)p;
  new_list->bucket_num = bucket_idx;
  new_list->is_free = 0;
  if (heap_top != NULL) {
    new_list->prev_bucket_idx = heap_top->bucket_num;
  }
  return p+SIZE_T_SIZE;
}

/*
 * Given a large bucket, divide it into chunks until it just barely holds SIZE.
 * Recursive: only divides bucket in half at any level.
 * 
 * size: the size of the object we need to fit
 * bucket_idx: the index of the smallest bucket we can find
 * bucketp: pointer to the start of the bucket
 */
void subdivideBucket(size_t size, int bucket_idx, free_list_t* head) {
  // Advance the pointer of the relevant free list, to let it know we're
  // stealing this chunk of memory
  head = free_lists[bucket_idx];
  free_lists[bucket_idx] = free_lists[bucket_idx]->next;
  // our new size is smaller
  size_t new_bucket_size = BUCKET_SIZE(bucket_idx-1);
  // make room for the new bucket
  free_list_t* new_bucket = (free_list_t*)((char*)head + new_bucket_size);
  // set fields for the new bucket
  new_bucket->bucket_num = bucket_idx-1;
  new_bucket->prev_bucket_idx = bucket_idx-1;
  new_bucket->is_free = 0x1;
  // set fields for the bucket after this new bucket
  free_list_t* bucket_after = (free_list_t*)((char*)head + BUCKET_SIZE(bucket_idx));
  bucket_after->prev_bucket_idx = bucket_idx-1;
  // put the first smaller bucket on the stack
  new_bucket->next = free_lists[bucket_idx-1];
  // reassign front of list
  free_lists[bucket_idx-1] = new_bucket;
  // set fields for the reassigned head
  head->bucket_num = bucket_idx-1;
  // put the reassigned head on the stack
  head->next = new_bucket;
  free_lists[bucket_idx-1] = head;
  // If size is too big for the next smaller index, we're done
  if (bucket_idx == 1 || size > BUCKET_SIZE(bucket_idx-2)-8) {
    free_lists[bucket_idx-1] = free_lists[bucket_idx-1]->next;
    return;
  }

  // otherwise recurse
  subdivideBucket(size, bucket_idx-1, head);
}

void * alloc_aligned(int bucket_idx) {
  void *p = mem_sbrk(BUCKET_SIZE(bucket_idx));
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
  free_list_t * flist = (free_list_t*)(ptr-SIZE_T_SIZE);
  int bucket = flist->bucket_num; 
  flist->is_free = 0x1;
  free_list_t* bucket_list = free_lists[bucket];
  flist->next = bucket_list;
  free_lists[bucket] = flist;
  coalesceEntries(flist);
}

/*
 * Coalesces two adjacent free buckets and makes them into a larger
 * bucket. Recurses.
 */
void coalesceEntries(free_list_t* list) {
  int prev_bucket_num = list->prev_bucket_idx;
  int b_num = list->bucket_num;
  if (prev_bucket_num == b_num) {
    free_list_t* prev_list = (free_list_t*)((char*)list - BUCKET_SIZE(prev_bucket_num));
    if (prev_list->is_free == 0x1) {
      // coalesceHelper(prev_list, list);
    }
  } else {
    free_list_t* next_list = (free_list_t*)((char*)list + BUCKET_SIZE(b_num));
    if (next_list->is_free == 0x1 && next_list->bucket_num == b_num) {
      // coalesceHelper(list, next_list);
    }
  }
}

void coalesceHelper(free_list_t* list_a, free_list_t* list_b) {
  int bucket_idx = list_a->bucket_num;
  // remove both from bucket_idx
  free_list_t* bucket_list = free_lists[bucket_idx];
    
  free_list_t* prev_item = NULL;
  while (bucket_list != NULL) {
    if (bucket_list == list_a || bucket_list == list_b) {
      if (prev_item != NULL) {
        prev_item->next = bucket_list->next;
      } else {
        free_lists[bucket_idx] = free_lists[bucket_idx]->next;
      }
    } else {
      prev_item = bucket_list;
    }
    bucket_list = bucket_list->next;
  }
  printf("here\n");
  // update size of first
  list_a->bucket_num = bucket_idx+1;
  // update prev_size of the node after list_b
  free_list_t* next_list = (free_list_t*)((char*)list_b + BUCKET_SIZE(bucket_idx));
  next_list->prev_bucket_idx = bucket_idx+1;
  
  // add to bucket bucket_idx+1
  free_list_t* new_bucket_list = free_lists[bucket_idx+1];
  list_a->next = new_bucket_list;
  free_lists[bucket_idx+1] = list_a;
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
