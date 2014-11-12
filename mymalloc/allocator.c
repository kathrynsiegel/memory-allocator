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
#define BUCKET_SIZE(i) ((1 << ((i) + MIN_SIZE_LOG_2)) - (HEADER_SIZE))
#define FITS_INTO_BUCKET(size, bucket_idx) ((size) <= (BUCKET_SIZE(bucket_idx)))


// the linked list data structure that holds the blocks we want to free
typedef struct free_list_t {
  unsigned int bucket_size: 30;
  unsigned int prev_bucket_size: 30;
  unsigned int is_free: 4;
  struct free_list_t* next;
} free_list_t;

#define HEADER_SIZE 8

int get_bucket_num(size_t size);
void coalesceEntries(free_list_t* list);
void subdivideAndAssignBucket(size_t size, free_list_t* head);
void subdivideBucket(size_t size, free_list_t* head);
void * alloc_aligned(size_t size);
void coalesceHelper(free_list_t* list_a, free_list_t* list_b);
void removeFromFreeList(free_list_t* bucket, int list_num);
void removeFromFreeListAlt(free_list_t* bucket, free_list_t** list);
void *alloc_alignedalt(int bucket_idx);

free_list_t *free_lists[NUM_BUCKETS];
free_list_t *top_element_bucket;

/** 
* init - Initialize the malloc package.  Called once before any other
* calls are made.  
*/
int my_init() {
  for (int i = 0; i < NUM_BUCKETS; i++) {
    free_lists[i] = NULL;
  }
  top_element_bucket = NULL;

  // align brk just once
  void *brk = mem_heap_hi() + 1;
  int req_size = CACHE_ALIGN(brk) - (uint64_t)brk;
  mem_sbrk(req_size);
  
  return 0;
}

/** The bucket size is the FLOOR of log(size).
* Note that we leave room for an 8 byte header.
*/
int get_bucket_num(size_t size) {
  int i = 0;
  size += HEADER_SIZE - 1; // room for 8 byte header
  size >>= 5;
  while (size) {
    i++;
    size >>= 1;
  }

  // difference between trace 6/7 allocator and normal allocator
  // is the way bucketing is handled
  if (TRACE_CLASS == 6 || TRACE_CLASS == 7) {
    return i;
  }

  if (i == 0)
    return 0;
  return i - 1; 
}

/**
* malloc - Allocate a block by incrementing the brk pointer.
* Always allocate a block whose size is a multiple of the alignment.
*/
void * my_malloc(size_t size) {
  if (TRACE_CLASS == 6 || TRACE_CLASS == 7) {
    /* add to free list with different size */
    void *p = NULL;
    int bucket_idx = get_bucket_num(size);
    
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
      p = alloc_alignedalt(bucket_idx);

      // TODO: maybe do this inside alloc_aligned?
      new_list = (free_list_t*)p;

      if (top_element_bucket != NULL) {
        new_list->prev_bucket_size = top_element_bucket->bucket_size;
        top_element_bucket->bucket_size = bucket_idx;
      } else {
        new_list->prev_bucket_size = -1;
      }
    }

    // fill header info and increment the pointer by HEADER_SIZE
    // we made a free_list, but don't use it like a free_list: the cast is strictly to
    // align the header in the right place.
    new_list->bucket_size = bucket_idx;  // set size
    new_list->is_free = 0;  // in use
    return (void*)((char*)p + HEADER_SIZE);
  } else {
    /* add to free list with different size */
    void *p = NULL;
    free_list_t* head;
    int bucket_idx = get_bucket_num(size);
    size = ALIGN(size);
    if (size < 32 - HEADER_SIZE)
      size = 32 - HEADER_SIZE;
    
    if (free_lists[bucket_idx] != NULL) {
      // Check for a bucket in the free list which chould contain size most
      // exactly. If we find something here, we're good.
      head = free_lists[bucket_idx];
      // if (TRACE_CLASS == 6 || TRACE_CLASS == 7) { //DONE
      //   p = (void*)head;
      //   free_lists[bucket_idx] = free_lists[bucket_idx]->next;
      // } else {
        
        free_list_t* prev = NULL;

        // loop through the free list. As soon as we find something which holds
        // SIZE, assign it and remove it from the list.
        int count = 0;
        while (head != NULL && count < 80) {
          if (size <= head->bucket_size) {
            // we've found something that fits
            if (prev == NULL) {
              // if it's the first item in the list, just pop off
              free_lists[bucket_idx] = free_lists[bucket_idx]->next;
            } else {
              // otherwise, cut out of the list
              prev->next = head->next;
            }
            p = (void *) head;
            break;
          }

          // if we didn't find one, move on
          prev = head;
          head = head->next;
          if (TRACE_CLASS == 8 || TRACE_CLASS == 6 || TRACE_CLASS == 5)
            count++;
        }
      // }
    } 

    // if we didn't find anything yet, check larger buckets
    if (!p) {
      bucket_idx++;

      if (free_lists[bucket_idx] != NULL) {
        
        // The next bigger size should be a pretty good fit
        head = free_lists[bucket_idx];
        free_lists[bucket_idx] = free_lists[bucket_idx]->next;
        p = (void *) head;

      } else {
        
        // Find an open bucket that is at least two times larger than the one we need
        int open_bucket = bucket_idx + 1;
        for (; open_bucket < NUM_BUCKETS; ++open_bucket) {
          if (free_lists[open_bucket] != NULL) {
            break;
          }
        }

        // If we have a free bucket, but it's too big, subdivide it and assign p.
        if (open_bucket < NUM_BUCKETS) {
          // break a chunk off a larger bucket and assign that
          head = free_lists[open_bucket];
          subdivideAndAssignBucket(size, head);
          p = (void *) head;
          
          // something went wrong if P is null
          if (p == NULL)
            return NULL;
        }
      }
    }

    free_list_t* new_list;

    if (p) {
      if (p == (void *)-1) {
        // an error occurred, and we couldn't allocate memory.
        return NULL;
      }
      new_list = head;

    // If p still has not been assigned, we need new heap space. 
    } else {
      // allocate a new item.
      p = alloc_aligned(size);
      new_list = (free_list_t*)p;
      if (top_element_bucket != NULL) {
        new_list->prev_bucket_size = top_element_bucket->bucket_size;
      } else {
        new_list->prev_bucket_size = 0;
      }
      top_element_bucket = new_list;
      new_list->bucket_size = size;
    }

    // if (new_list->bucket_size > 0x10000000)
      /*printf("Big bucket size: %d\n", new_list->bucket_size);*/

    // fill header info and increment the pointer by HEADER_SIZE
    // we made a free_list, but don't use it like a free_list: the cast is strictly to
    // align the header in the right place.
    new_list->is_free = 0x0;  // indicate that it's in use
    /*printf(" * malloc bucket %p, size %d\n", new_list, new_list->bucket_size);*/
    return (void*)((char*)p + HEADER_SIZE);
  }
  
}

/* 
 * Remove a bucket from a linked list, using the power of O(n) search.
 * TODO: A doubly-linked free list would be helpful
 */
void removeFromFreeList(free_list_t* bucket, int list_num) {
  if (bucket == free_lists[list_num]) {
    // if this bucket is at the head of its list, great!
    free_lists[list_num] = bucket->next;
  } else {

    // otherwise we have to iterate through everything...
    free_list_t* prev = free_lists[list_num];
    while (prev != NULL && prev->next != bucket) {
      // if we hit the end of the list, it's not here.
      if (prev->next == NULL)
        break;
      
      prev = prev->next;
    }
    
    // cut BUCKET out from the LL, but leave the LIST head untouched.
    if (prev != NULL) {
      prev->next = bucket->next;
    }
  }

  /*printf("removing from free list %p\n", bucket);*/

  bucket->next = NULL;
  return;
  
}

void removeFromFreeListAlt(free_list_t* bucket, free_list_t** list) {
  if (bucket == *list) {
    // if this bucket is at the head of its list, great!
    *list = (*list)->next;
    return;
  }

  // otherwise we have to iterate through everything...
  free_list_t* prev = *list;
  while (prev != NULL && prev->next != bucket) {
    // if we hit the end of the list, something is wrong.
    if (prev->next == NULL) {
      return;
    }
    prev = prev->next;
  }
  
  // cut BUCKET out from the LL, but leave the LIST head untouched.
  if (prev != NULL) {
    prev->next = bucket->next;
  }
}

/* 
 * Add a bucket to a linked list in sorted order.
 */
void addToFreeList(free_list_t* bucket) {
  /*printf("adding to free list bucket %p, size %x\n", bucket, bucket->bucket_size);*/
  int bucket_num = get_bucket_num(bucket->bucket_size);
  /*printf("bucket_num: %d\n", bucket_num);*/
  free_list_t* list = free_lists[bucket_num];

  if (list == NULL) {
    bucket->next = NULL;
    free_lists[bucket_num] = bucket;
    /*printf("    added bucket %p, size %x to null free list %d\n",
        bucket, bucket->bucket_size, bucket_num);*/
    return;
  }

  // If the bucket fits at the head of the list, great!
  if (TRACE_CLASS == 5 || TRACE_CLASS == 8) {
    bucket->next = list;
    free_lists[bucket_num] = bucket;
  } else {
    if (list->bucket_size >= bucket->bucket_size) {
      bucket->next = list;
      free_lists[bucket_num] = bucket;
      /*printf("    added bucket %p, size %x to head of free list %d\n",
          bucket, bucket->bucket_size, bucket_num);*/
      return;
    }

    // Otherwise, iterate until we find something larger than the bucket or reach
    // the end of the list
    while (list->next != NULL) {
      if (list->next->bucket_size >= bucket->bucket_size)
        break;
      list = list->next;
    }

    bucket->next = list->next;
    list->next = bucket;
  }
  
  /*printf("    added to free list bucket %p, size %d\n", bucket, bucket->bucket_size);*/
}

/*
 * Given a large bucket, divide it into two chunks: one just big enough to hold
 * SIZE, and one with the rest of the space.
 * 
 * size: the size of the object we need to fit
 * head: the big bucket to divide
 */
void subdivideAndAssignBucket(size_t size, free_list_t* head) {
  int big_bucket_size = head->bucket_size;
  int big_bucket_i = get_bucket_num(big_bucket_size);

  if (head->bucket_size < ALIGN(size)) {
    printf("trying to subdivide bucket too small\n");
  }

  // size must be aligned
  size = ALIGN(size);
  int jump_size = size + HEADER_SIZE;

  // cut out HEAD from its free list
  removeFromFreeList(head, big_bucket_i);

  // make room for the first new bucket (the "leftovers"). This one contains all
  // the space not needed for SIZE. Jump forward in memory by the total
  // required size (including the header)
  free_list_t* new_bucket = (free_list_t*)((char*)head + jump_size);

  // set fields for the leftover bucket
  new_bucket->bucket_size = big_bucket_size - jump_size;
  new_bucket->prev_bucket_size = size;

  // find the bucket after the leftover bucket
  //if (new_bucket == mem_heap_hi() - (new_bucket->bucket_size + HEADER_SIZE)) {
  if (head == top_element_bucket) {
    // is it on top of the heap?
    top_element_bucket = new_bucket;
  } else {
    // find our successor
    free_list_t* bucket_after = (free_list_t*)((char*)new_bucket + 
        new_bucket->bucket_size + HEADER_SIZE);

    // tell it that we are now smaller
    bucket_after->prev_bucket_size = new_bucket->bucket_size;
  }

  /*printf("subdivide %p (size %x) into %p\n", head, head->bucket_size, new_bucket);*/
  
  // put the leftover bucket on the stack
  addToFreeList(new_bucket);
  new_bucket->is_free = 0x1;  // free = true

  // set fields for the reassigned head
  head->bucket_size = size;
}

/*
 * Given a large bucket, divide it into chunks until it just barely holds SIZE.
 * Recursive: only divides bucket in half at any level.
 * 
 * size: the size of the object we need to fit
 * head: the big bucket to divide
 */
void subdivideBucket(size_t size, free_list_t* head) {
  int big_bucket_i = head->bucket_size;
  int small_bucket_i = big_bucket_i - 1;
  
  // cut out HEAD from its free list
  removeFromFreeListAlt(head, &free_lists[big_bucket_i]);

  // make room for the first new bucket: jump forward in memory by the total required 
  // size (including the header)
  free_list_t* new_bucket = (free_list_t*)((char*)head + 
      BUCKET_SIZE(small_bucket_i) + HEADER_SIZE);

  // set fields for the first new bucket
  new_bucket->bucket_size = small_bucket_i;
  new_bucket->prev_bucket_size = small_bucket_i;
  new_bucket->is_free = 0x1;  // free = true

  // find the bucket after this new bucket
  if (new_bucket == mem_heap_hi() - (BUCKET_SIZE(small_bucket_i) + HEADER_SIZE)) {
    top_element_bucket->bucket_size = small_bucket_i;
  } else {
    free_list_t* bucket_after = (free_list_t*)((char*)head + 
      BUCKET_SIZE(big_bucket_i) + HEADER_SIZE);
    // tell it that we are now smaller
    bucket_after->prev_bucket_size = small_bucket_i;
  }
  
  // put the first smaller bucket on the stack
  new_bucket->next = free_lists[small_bucket_i];

  // add it to the front of the free list
  free_lists[small_bucket_i] = new_bucket;

  // set fields for the reassigned head
  head->bucket_size = small_bucket_i;

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
 * Grows the heap by enough memory to hold a bucket of size SIZE.
 */
void *alloc_aligned(size_t size) {
  void *p = mem_sbrk(ALIGN(size) + HEADER_SIZE);
  if (p == (void *)-1) {
    return NULL;
  }
  return p;
}

void *alloc_alignedalt(int bucket_idx) {
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

  if (TRACE_CLASS == 6 || TRACE_CLASS == 7) {
    // Cast the pointer to a free list pointer - this means including the header
    // we'd previously ignored
    free_list_t * flist = (free_list_t*)((char*)ptr - HEADER_SIZE);
    int bucket_size = flist->bucket_size;
    flist->is_free = 0x1;

    // push it onto the stack of free lists for this bucket size
    flist->next = free_lists[bucket_size];
    free_lists[bucket_size] = flist;
    
    // coalesce entries now
    coalesceEntries(flist);
  } else {
     // Cast the pointer to a free list pointer - this means including the header
    // we'd previously ignored
    free_list_t * flist = (free_list_t*)((char*)ptr - HEADER_SIZE);
    /*printf(" * freeing bucket %p, size %d\n", flist, flist->bucket_size);*/
    /*if (flist->is_free == 0x1)
      printf("Trying to free free object!\n");*/

    addToFreeList(flist);
    flist->is_free = 0x1;

    // coalesce entries now
    //coalesceEntries(flist);
  }

 
}

/*
 * Coalesces two adjacent free buckets and makes them into a larger
 * bucket. Recurses... ?
 */
void coalesceEntries(free_list_t* list) {
  if (TRACE_CLASS == 6 || TRACE_CLASS == 7) {
    int prev_bucket_size = list->prev_bucket_size;
    int b_num = list->bucket_size;
    // Check the bucket behind this one
    if (prev_bucket_size == b_num) {
      free_list_t* prev_list = (free_list_t*)((char*)list -
          (BUCKET_SIZE(b_num) + HEADER_SIZE));
      if (prev_list->is_free == 0x1) {
        coalesceHelper(prev_list, list);
      }
    } else {
      // check the bucket in front
      free_list_t* next_list = (free_list_t*)((char*)list + 
          BUCKET_SIZE(b_num) + HEADER_SIZE);
      if ((int)(next_list->bucket_size) == b_num && next_list->is_free == 0x1 && mem_heap_hi() > (void*)(next_list + HEADER_SIZE)) {
        coalesceHelper(list, next_list);
      }
    }
  } else {
    int prev_bucket_size = list->prev_bucket_size;
    int my_size = list->bucket_size;
    
    // Check the bucket after this one
    free_list_t* next_list = (free_list_t*)((char*)list + my_size + HEADER_SIZE);

    if (list != top_element_bucket && next_list->is_free == 0x1) {
      coalesceHelper(list, next_list);

      if (top_element_bucket == next_list)
        top_element_bucket = list;
    }
    
    // Check the bucket before this one
    if (prev_bucket_size > 0) {
      free_list_t* prev_list = (free_list_t*)((char*)list -
          (prev_bucket_size + HEADER_SIZE));
      
      if (prev_list->is_free == 0x1) {
        coalesceHelper(prev_list, list);

        if (top_element_bucket == list)
          top_element_bucket = prev_list;
      }
    }
  }
  

  // TODO: recurse?
}

/*
 * Takes two buckets adjacent in memory, removes them from their respective free
 * lists, and joins them into a larger bucket.
 */
void coalesceHelper(free_list_t* list_a, free_list_t* list_b) {
  if (TRACE_CLASS == 6 || TRACE_CLASS == 7) {
    int bucket_size = list_a->bucket_size;
    int new_bucket_num = bucket_size + 1;
    // remove both from bucket_idx
    // again with the O(n) search... TODO a doubly-linked list
    removeFromFreeListAlt(list_a, &free_lists[bucket_size]);
    removeFromFreeListAlt(list_b, &free_lists[bucket_size]);

    // update size of first
    list_a->bucket_size = new_bucket_num;

    // update prev_size of the node after list_b
    free_list_t* next_list = (free_list_t*)((char*)list_b +
        BUCKET_SIZE(bucket_size) + HEADER_SIZE);

    next_list->prev_bucket_size = new_bucket_num;
    
    // add to bucket bucket_num + 1
    free_list_t* new_bucket_list = free_lists[new_bucket_num];
    list_a->next = new_bucket_list;
    free_lists[new_bucket_num] = list_a;
  } else {
    /*printf("coalesce %p and %p\n", list_a, list_b);
    printf("list a size: %x\n", list_a->bucket_size);
    printf("list b size: %x\n", list_b->bucket_size);*/
    int bucket_a_num = get_bucket_num(list_a->bucket_size);
    int bucket_b_num = get_bucket_num(list_b->bucket_size);
    size_t new_size = list_a->bucket_size + list_b->bucket_size;

    // again with the O(n) search... TODO a doubly-linked list
    removeFromFreeList(list_a, bucket_a_num);
    removeFromFreeList(list_b, bucket_b_num);

    // update size of list_a 
    list_a->bucket_size = new_size;
    /*printf("list a new size: %x\n", list_a->bucket_size);*/

    // update prev_size of the node after list_b
    free_list_t* next_list = (free_list_t*)((char*)list_b +
        list_b->bucket_size + HEADER_SIZE);

    next_list->prev_bucket_size = new_size;
    
    // add to appropriate free list
    free_list_t* new_bucket_list = free_lists[get_bucket_num(new_size)];
    list_a->next = new_bucket_list;
    free_lists[get_bucket_num(new_size)] = list_a;
  }
  
}

int coalesceEntriesForRealloc(free_list_t* list) {
  int prev_bucket_size = list->prev_bucket_size;
  int b_num = list->bucket_size;
  // Check the bucket behind this one
  if (prev_bucket_size == b_num) {
    free_list_t* prev_list = (free_list_t*)((char*)list -
        (BUCKET_SIZE(b_num) + HEADER_SIZE));
    if (prev_list->is_free == 0x1) {
      // remove prev_list from free list
      removeFromFreeListAlt(prev_list, &free_lists[b_num]);
      // adjust list pointer
      list = prev_list;
      //change header
      list->bucket_size = b_num + 1;
      list->is_free = 0x0;
      return 1;
    }
  } else {
    // check the bucket in front
    free_list_t* next_list = (free_list_t*)((char*)list + 
        BUCKET_SIZE(b_num) + HEADER_SIZE);
    if ((int)(next_list->bucket_size) == b_num && next_list->is_free == 0x1 && mem_heap_hi() > (void*)(next_list + HEADER_SIZE)) {
      // remove next_list from free list
      removeFromFreeListAlt(next_list, &free_lists[b_num]);
      // change header
      list->bucket_size = b_num + 1;
      return 1;
    }
  }
  return 0;
}


/*
 * realloc - Given an allocated chunk, move its contents to a new memory block
 * large enough to hold SIZE. Implemented simply in terms of malloc and free.
 */
void * my_realloc(void *ptr, size_t size) {
  if (TRACE_CLASS == 6 || TRACE_CLASS == 7) {
    void *newptr;

    // Get the size of the old block of memory.
    free_list_t * flist = (free_list_t*)((char*)ptr - HEADER_SIZE);
    int bucket_size = flist->bucket_size;
    size_t old_size = BUCKET_SIZE(bucket_size);

    // If the new block is smaller than the old one, the pointer stays the same.
    if (size < old_size) {
      if (BUCKET_SIZE(get_bucket_num(size)) < old_size)
        subdivideBucket(size, flist);
      return ptr;
    }

    newptr = my_malloc(size);

    // This is a standard library call that performs a simple memory copy.
    memcpy(newptr, ptr, old_size);

    // Release the old block.
    my_free(ptr);

    // Return a pointer to the new block.
    return newptr;
  } else {
    // Get the size of the old block of memory.
    size = ALIGN(size);
    free_list_t * flist = (free_list_t*)((char*)ptr - HEADER_SIZE);
    size_t old_size = flist->bucket_size;

    // If the new block is smaller than the old one, the pointer stays the same.
    if (size <= old_size) {
      // If they would go into two different buckets, free the end chunk of the
      // realloc'd space for use by others
      if (get_bucket_num(size) < get_bucket_num(old_size) - 1)
        subdivideAndAssignBucket(size, flist);

      return ptr;
    }

    // If the old block is at the end of the stack, just extend the stack
    if (flist == top_element_bucket) {
      mem_sbrk(size - old_size);
      flist->bucket_size = size;
      return ptr;
    }

    void *newptr = my_malloc(size);

    // This is a standard library call that performs a simple memory copy.
    memcpy(newptr, ptr, old_size);

    // Release the old block.
    my_free(ptr);

    // Return a pointer to the new block.
    return newptr;
  }
  
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
