/*
 * validator.h - 6.172 Malloc Validator
 *
 * Validates a malloc/free/realloc implementation.
 *
 * Copyright (c) 2010, R. Bryant and D. O'Hallaron, All rights reserved.
 * May not be used, modified, or copied without permission.
 */

#ifndef MM_VALIDATOR_H
#define MM_VALIDATOR_H

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "./config.h"
#include "./mdriver.h"
#include "./memlib.h"

// Returns true if p is R_ALIGNMENT-byte aligned
#if (__WORDSIZE == 64 )
#define IS_ALIGNED(p)  ((((uint64_t)(p)) % R_ALIGNMENT) == 0)
#else
#define IS_ALIGNED(p)  ((((uint32_t)(p)) % R_ALIGNMENT) == 0)
#endif

// Range list data structure

// Records the extent of each block's payload
typedef struct range_t {
  char *lo;              // low payload address
  char *hi;              // high payload address
  struct range_t *next;  // next list element
} range_t;

// The following routines manipulate the range list, which keeps
// track of the extent of every allocated block payload. We use the
// range list to detect any overlapping allocated blocks.


// add_range - As directed by request opnum in trace tracenum,
// we've just called the student's malloc to allocate a block of
// size bytes at addr lo. After checking the block for correctness,
// we create a range struct for this block and add it to the range list.
static int add_range(const malloc_impl_t *impl, range_t **ranges, char *lo,
    int size, int tracenum, int opnum) {
  char *hi = lo + size - 1;

  // You can use this as a buffer for writing messages with sprintf.
  // char msg[MAXLINE];

  assert(size > 0);

  // Payload addresses must be R_ALIGNMENT-byte aligned
  if (!IS_ALIGNED(lo)) {
    printf("lo not aligned. lo = %p\n", lo);
    malloc_error(tracenum, 0, "add range failed");
  }

  // The payload must lie within the extent of the heap
  if (lo < (char*)mem_heap_lo() || hi > (char*)mem_heap_hi())
    malloc_error(tracenum, 0, "impl add range failed: payload not in heap.");

  // The payload must not overlap any other payloads
  range_t *p = *ranges;
  while (p != NULL) {
    if ((p->lo <= hi && p->hi >= hi) || (p-> lo <= lo && p->hi >= lo)) {
      malloc_error(tracenum, 0, "impl add range failed: payload overlaps another range.");
    }
    p = p->next;
  }

  // Everything looks OK, so remember the extent of this block by creating a
  // range struct and adding it the range list.
  range_t *range = (range_t*)malloc(sizeof(range_t));
  range->lo = lo;
  range->hi = hi;
  range->next = *ranges;
  *ranges = range;

  return 1;
}

// remove_range - Free the range record of block whose payload starts at lo
static int remove_range(range_t **ranges, char *lo) {
  range_t *p = NULL;
  range_t *prevpp = *ranges;

  if (prevpp == NULL)
    return 0;

  // Check to see if the range is at the head of the list.
  if (prevpp->lo == lo) {
    *ranges = prevpp->next;
    free(prevpp);
    return 1;
  }

  p = prevpp->next;

  // Iterate the linked list until you find the range with a matching lo
  // payload and remove it. 
  while (p->lo != lo) {
    prevpp = p;
    p = prevpp->next;
  }
  
  // p is now pointing to the range with the payload. Cut it out of the loop.
  prevpp->next = p->next;
  free(p);
  return 1;
}

// clear_ranges - free all of the range records for a trace
static void clear_ranges(range_t **ranges) {
  range_t *p = *ranges;
  range_t *pnext;

  while (p != NULL) {
    pnext = p->next;
    free(p);
    p = pnext;
  }

  *ranges = NULL;
}

// eval_mm_valid - Check the malloc package for correctness
int eval_mm_valid(const malloc_impl_t *impl, trace_t *trace, int tracenum) {
  int i = 0;
  int index = 0;
  int size = 0;
  int oldsize = 0;
  char *newp = NULL;
  char *oldp = NULL;
  char *p = NULL;
  range_t *ranges = NULL;

  // Reset the heap.
  impl->reset_brk();

  // Call the mm package's init function
  if (impl->init() < 0) {
    malloc_error(tracenum, 0, "impl init failed.");
    return 0;
  }

  // Interpret each operation in the trace in order
  for (i = 0; i < trace->num_ops; i++) {
    index = trace->ops[i].index;
    size = trace->ops[i].size;

    switch (trace->ops[i].type) {
      case ALLOC:  // malloc

        // Call the student's malloc
        if ((p = (char *) impl->malloc(size)) == NULL) {
          malloc_error(tracenum, i, "impl malloc failed.");
          return 0;
        }

        // Test the range of the new block for correctness and add it
        // to the range list if OK. The block must be aligned properly,
        // and must not overlap any currently allocated block.
        if (add_range(impl, &ranges, p, size, tracenum, i) == 0)
          return 0;

        // Fill the allocated region with some unique data that you can check
        // for if the region is copied via realloc.
        // (project 3)
        // Just a sequence of numbers, starting at 0.
        for (int j = 0; j < size; j++) {
          p[j] = (uint8_t)j;
        }

        // Remember region
        trace->blocks[index] = p;
        trace->block_sizes[index] = size;
        break;

      case REALLOC:  // realloc

        // Call our realloc
        oldp = (char *) trace->blocks[index];
        if ((newp = (char *) impl->realloc(oldp, size)) == NULL) {
          malloc_error(tracenum, i, "impl realloc failed.");
          return 0;
        }

        // Remove the old region from the range list
        if (remove_range(&ranges, oldp) == 0) {
          printf("remove_range failed: no range to remove!");
          return 0;
        }

        // Check new block for correctness and add it to range list
        if (add_range(impl, &ranges, newp, size, tracenum, i) == 0)
          return 0;

        // Make sure that the new block contains the data from the old block,
        // and then fill in the new block with new data that you can use to
        // verify the block was copied if it is resized again.
        oldsize = trace->block_sizes[index];
        if (size < oldsize)
          oldsize = size;
        
        // (project3)
        // Check that the new block contains the data from the old block, then
        // fill the new extra space with stuff.
        for (int j = 0; j < size; j++) {
          if (j < oldsize) {
            if ((uint8_t) newp[j] != (uint8_t) j) {
              
              // if a value in [0, oldsize) doesn't match, something went wrong
              printf("impl realloc failed: data at location %p[%d] "
                    "not copied properly. Expected value: %d, new value: %d\n",
                    newp, j, (uint8_t) j, (uint8_t) newp[j]);
              malloc_error(tracenum, i, "impl realloc failed"); 
              
              // throw an error
              assert(0);
            } 
          } else {
            newp[j] = (uint8_t)j;
          }
        }

        // Remember region
        trace->blocks[index] = newp;
        trace->block_sizes[index] = size;
        break;

      case FREE:  // free

        // Remove region from list and call student's free function
        p = trace->blocks[index];
        if (remove_range(&ranges, p) == 0) {
          printf("remove_range failed: no range to remove!");
          return 0;
        }

        impl->free(p);
        break;

      case WRITE:  // write

        break;

      default:
        app_error("Nonexistent request type in eval_mm_valid");
    }
  }

  // Free ranges allocated and reset the heap.
  impl->reset_brk();
  clear_ranges(&ranges);

  // As far as we know, this is a valid malloc package
  return 1;
}
#endif  // MM_VALIDATOR_H
