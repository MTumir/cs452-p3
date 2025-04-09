#include <stdio.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/errno.h>
#else
#include <errno.h>
#endif

#include "lab.h"

#define handle_error_and_die(msg) \
    do                            \
    {                             \
        perror(msg);              \
        raise(SIGKILL);          \
    } while (0)

size_t btok(size_t bytes)
{
    if (!bytes) {
        return -1;
    }

    size_t k_val = 0;
    size_t mult = UINT64_C(1) << k_val;
    while (bytes > mult) {
        k_val++;                          // Increase by 1
        mult = UINT64_C(1) << k_val;      // 2^k
    }
    return k_val;
}

struct avail *buddy_calc(struct buddy_pool *pool, struct avail *buddy)
{
    if (!pool || !buddy) {
        return NULL;
    }

    size_t address = (size_t)buddy - (size_t)pool->base;    // Remove base address for XOR operation
    size_t operand = UINT64_C(1) << buddy->kval;            // 2^k

    // Add base address back in after XOR operation
    return (struct avail *)((address ^ operand) + (size_t)pool->base);
}

void *buddy_malloc(struct buddy_pool *pool, size_t size)
{
    if (!pool) {
        return NULL;
    }

    // Get the k_val for the given size with extra space for overhead
    size_t k = btok(size + sizeof(&pool->avail->tag) + sizeof(&pool->avail->kval));

    //R1 - [Find block.]    
    size_t j = 0;      // k_val of smallest empty block greater than or equal to k
    for (size_t temp_j = k; temp_j <= pool->kval_m; temp_j++) {
        if (pool->avail[temp_j].next != &pool->avail[temp_j]) {
            j = temp_j;
            break;
        }
    }

    if (j == 0) {
        perror("Not enough memory, returning NULL.");
        return NULL;
    }

    //R2 - [Remove from list.]
    struct avail *L = pool->avail[j].next;  // Block we discovered in R1
    struct avail *P = L->next;
    pool->avail[j].next = P;
    P->prev = &pool->avail[j];
    L->tag = BLOCK_RESERVED;
    
    //R3 - [Split required?]
    while (j > k) {
        //R4 - [Split.]
        j--;
        P = (struct avail *)((size_t)L + (UINT64_C(1) << j));
        P->tag = BLOCK_AVAIL;
        P->kval = j;
        P->next = P->prev = &pool->avail[j];
        pool->avail[j].next = pool->avail[j].prev = P;
    }

    L->kval = j;
    return (void *)((char *)L + sizeof(struct avail));
}

void buddy_free(struct buddy_pool *pool, void *ptr)
{
    if (!pool || !ptr) {
        return;
    }

    // Derive Knuth notation from parameters
    struct avail *L = (struct avail *)(ptr - sizeof(struct avail));
    size_t k = L->kval;

    //S1 - [Is buddy available?]
    struct avail *P = buddy_calc(pool, L);
    while (true) {
        // If any of the following conditions are true, go to S3.
        if (k == pool->kval_m) {
            break;
        } else if (P->tag == BLOCK_RESERVED) {
            break;
        } else if (P->tag == BLOCK_AVAIL && P->kval != k) {
            break;
        }

        //S2 - [Combine with buddy.]
        P->prev->next = P->next;
        P->next->prev = P->prev;
        L->kval++;
        k = L->kval;

        if (P < L) {
            L = P;
        }

        // Return to S1.
        P = buddy_calc(pool, L);
    }

    //S3 - [Put on list.]
    L->tag = BLOCK_AVAIL;
    P = pool->avail[k].next;
    L->next = P;
    P->prev = L;
    L->kval = k;
    L->prev = &pool->avail[k];
    pool->avail[k].next = L;
}

void buddy_init(struct buddy_pool *pool, size_t size)
{
    size_t kval = 0;
    if (size == 0)
        kval = DEFAULT_K;
    else
        kval = btok(size);

    if (kval < MIN_K)
        kval = MIN_K;
    if (kval > MAX_K)
        kval = MAX_K - 1;

    //make sure pool struct is cleared out
    memset(pool,0,sizeof(struct buddy_pool));
    pool->kval_m = kval;
    pool->numbytes = (UINT64_C(1) << pool->kval_m);
    //Memory map a block of raw memory to manage
    pool->base = mmap(
        NULL,                               /*addr to map to*/
        pool->numbytes,                     /*length*/
        PROT_READ | PROT_WRITE,             /*prot*/
        MAP_PRIVATE | MAP_ANONYMOUS,        /*flags*/
        -1,                                 /*fd -1 when using MAP_ANONYMOUS*/
        0                                   /* offset 0 when using MAP_ANONYMOUS*/
    );
    if (MAP_FAILED == pool->base)
    {
        handle_error_and_die("buddy_init avail array mmap failed");
    }

    //Set all blocks to empty. We are using circular lists so the first elements just point
    //to an available block. Thus the tag, and kval feild are unused burning a small bit of
    //memory but making the code more readable. We mark these blocks as UNUSED to aid in debugging.
    for (size_t i = 0; i <= kval; i++)
    {
        pool->avail[i].next = pool->avail[i].prev = &pool->avail[i];
        pool->avail[i].kval = i;
        pool->avail[i].tag = BLOCK_UNUSED;
    }

    //Add in the first block
    pool->avail[kval].next = pool->avail[kval].prev = (struct avail *)pool->base;
    struct avail *m = pool->avail[kval].next;
    m->tag = BLOCK_AVAIL;
    m->kval = kval;
    m->next = m->prev = &pool->avail[kval];
}

void buddy_destroy(struct buddy_pool *pool)
{
    int rval = munmap(pool->base, pool->numbytes);
    if (-1 == rval)
    {
        handle_error_and_die("buddy_destroy avail array");
    }
    //Zero out the array so it can be reused it needed
    memset(pool,0,sizeof(struct buddy_pool));
}

#define UNUSED(x) (void)x

// CR - Commented out printb to prevent warning [-Wunused-function]
// /**
//  * This function can be useful to visualize the bits in a block. This can
//  * help when figuring out the buddy_calc function!
//  */
// static void printb(unsigned long int b)
// {
//      size_t bits = sizeof(b) * 8;
//      unsigned long int curr = UINT64_C(1) << (bits - 1);
//      for (size_t i = 0; i < bits; i++)
//      {
//           if (b & curr)
//           {
//                printf("1");
//           }
//           else
//           {
//                printf("0");
//           }
//           curr >>= 1L;
//      }
// }
