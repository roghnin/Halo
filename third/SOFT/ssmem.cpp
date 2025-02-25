/*
 *   File: ssmem.c
 *   Author: Vasileios Trigonakis <vasileios.trigonakis@epfl.ch>
 *   Description: a simple object-based memory allocator with epoch-based
 *                garbage collection
 *   ssmem.c is part of ASCYLIB
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2014 Vasileios Trigonakis <vasileios.trigonakis@epfl.ch>
 *	      	      Distributed Programming Lab (LPD), EPFL
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 *of this software and associated documentation files (the "Software"), to deal
 *in the Software without restriction, including without limitation the rights
 *to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *copies of the Software, and to permit persons to whom the Software is
 *furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 *all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *SOFTWARE.
 *
 */

#include <assert.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>

#include "ssmem.h"
ssmem_ts_t *ssmem_ts_list = nullptr;
volatile uint32_t ssmem_ts_list_len = 0;
__thread volatile ssmem_ts_t *ssmem_ts_local = nullptr;
__thread size_t ssmem_num_allocators = 0;
__thread ssmem_list_t *ssmem_allocator_list = nullptr;
std::string PMEM_LOC1 = "/mnt/pmem0/";
unsigned long PM_POOL_SZ1 = 256UL * 1024 * 1024 * 1024;
VMEM *vmp1 = vmem_create(PMEM_LOC1.c_str(), PM_POOL_SZ1);
inline int ssmem_get_id() {
  if (ssmem_ts_local != nullptr) {
    return ssmem_ts_local->id;
  }
  return -1;
}

static ssmem_list_t *ssmem_list_node_new(void *mem, ssmem_list_t *next,
                                         bool isPM);
static void ssmem_zero_memory(ssmem_allocator_t *a);

/*
 * explicitely subscribe to the list of threads in order to used timestamps for
 * GC
 */
void ssmem_gc_thread_init(ssmem_allocator_t *a, int id, bool isPM) {
  a->ts = (ssmem_ts_t *)ssmem_ts_local;
  if (a->ts == nullptr) {
    if (isPM)
      a->ts = (ssmem_ts_t *)vmem_aligned_alloc(vmp1, CACHE_LINE_SIZE,
                                               sizeof(ssmem_ts_t));
    else
      a->ts = (ssmem_ts_t *)aligned_alloc(CACHE_LINE_SIZE, sizeof(ssmem_ts_t));
    assert(a->ts != nullptr);
    ssmem_ts_local = a->ts;

    a->ts->id = id;
    a->ts->version = 0;

    do {
      a->ts->next = ssmem_ts_list;
    } while (CAS_U64((volatile uint64_t *)&ssmem_ts_list, (uint64_t)a->ts->next,
                     (uint64_t)a->ts) != (uint64_t)a->ts->next);

    __attribute__((unused)) uint32_t null = FAI_U32(&ssmem_ts_list_len);
  }
}

ssmem_free_set_t *ssmem_free_set_new(size_t size, ssmem_free_set_t *next,
                                     bool isPM);

/*
 * initialize allocator a with a custom free_set_size
 * If the thread is not subscribed to the list of timestamps (used for GC),
 * additionally subscribe the thread to the list
 */
void ssmem_alloc_init_fs_size(ssmem_allocator_t *a, size_t size,
                              size_t free_set_size, int id, bool isPM) {
  ssmem_num_allocators++;
  ssmem_allocator_list =
      ssmem_list_node_new((void *)a, ssmem_allocator_list, isPM);

  // #if SSMEM_TRANSPARENT_HUGE_PAGES
  //   if (isPM) {
  //     a->mem = vmem_aligned_alloc(vpm, CACHE_LINE_SIZE, size);
  //   } else {
  //     int ret = posix_memalign(&a->mem, CACHE_LINE_SIZE, size);
  //   }
  // #else
  if (isPM)
    a->mem = (void *)vmem_aligned_alloc(vmp1, CACHE_LINE_SIZE, size);
  else
    a->mem = (void *)aligned_alloc(CACHE_LINE_SIZE, size);

  //#endif
  assert(a->mem != nullptr);

  a->mem_curr = 0;
  a->mem_size = size;
  a->tot_size = size;
  a->fs_size = free_set_size;

  ssmem_zero_memory(a);

  struct ssmem_list *new_mem_chunks =
      ssmem_list_node_new(a->mem, nullptr, isPM);
  BARRIER(new_mem_chunks);

  a->mem_chunks = new_mem_chunks;
  BARRIER(&a->mem_chunks);
  ssmem_gc_thread_init(a, id, isPM);

  a->free_set_list = ssmem_free_set_new(a->fs_size, nullptr, isPM);
  a->free_set_num = 1;

  a->collected_set_list = nullptr;
  a->collected_set_num = 0;

  a->available_set_list = nullptr;

  a->released_mem_list = nullptr;
  a->released_num = 0;
}

/*
 * initialize allocator a with the default SSMEM_GC_FREE_SET_SIZE
 * If the thread is not subscribed to the list of timestamps (used for GC),
 * additionally subscribe the thread to the list
 */
void ssmem_alloc_init(ssmem_allocator_t *a, size_t size, int id, bool isPM) {
  return ssmem_alloc_init_fs_size(a, size, SSMEM_GC_FREE_SET_SIZE, id, isPM);
}

/*
 *
 */
static ssmem_list_t *ssmem_list_node_new(void *mem, ssmem_list_t *next,
                                         bool isPM) {
  ssmem_list_t *mc;
  if (isPM)
    mc = (ssmem_list_t *)vmem_malloc(vmp1, sizeof(ssmem_list_t));
  else
    mc = (ssmem_list_t *)malloc(sizeof(ssmem_list_t));

  assert(mc != nullptr);
  mc->obj = mem;
  mc->next = next;
  return mc;
}

/*
 *
 */
inline ssmem_released_t *ssmem_released_node_new(void *mem,
                                                 ssmem_released_t *next,
                                                 bool isPM) {
  ssmem_released_t *rel;
  if (isPM)
    rel = (ssmem_released_t *)vmem_malloc(
        vmp1, sizeof(ssmem_released_t) + (ssmem_ts_list_len * sizeof(size_t)));
  else
    rel = (ssmem_released_t *)malloc(sizeof(ssmem_released_t) +
                                     (ssmem_ts_list_len * sizeof(size_t)));

  assert(rel != nullptr);
  rel->mem = mem;
  rel->next = next;
  rel->ts_set = (size_t *)(rel + 1);

  return rel;
}

/*
 *
 */
ssmem_free_set_t *ssmem_free_set_new(size_t size, ssmem_free_set_t *next,
                                     bool isPM) {
  /* allocate both the ssmem_free_set_t and the free_set with one call */
  ssmem_free_set_t *fs;
  if (isPM)
    fs = (ssmem_free_set_t *)vmem_aligned_alloc(
        vmp1, CACHE_LINE_SIZE,
        sizeof(ssmem_free_set_t) + (size * sizeof(uintptr_t)));
  else
    fs = (ssmem_free_set_t *)aligned_alloc(
        CACHE_LINE_SIZE, sizeof(ssmem_free_set_t) + (size * sizeof(uintptr_t)));
  assert(fs != nullptr);

  fs->size = size;
  fs->curr = 0;

  fs->set = (uintptr_t *)(((uintptr_t)fs) + sizeof(ssmem_free_set_t));
  fs->ts_set = nullptr; /* will get a ts when it becomes full */
  fs->set_next = next;

  return fs;
}

/*
 *
 */
ssmem_free_set_t *ssmem_free_set_get_avail(ssmem_allocator_t *a, size_t size,
                                           ssmem_free_set_t *next, bool isPM) {
  ssmem_free_set_t *fs;
  if (a->available_set_list != nullptr) {
    fs = a->available_set_list;
    a->available_set_list = fs->set_next;

    fs->curr = 0;
    fs->set_next = next;

    // printf("[ALLOC] got free_set from available_set : %p\n", fs);

  } else {
    fs = ssmem_free_set_new(size, next, isPM);
  }

  return fs;
}

/*
 *
 */
static void ssmem_free_set_free(ssmem_free_set_t *set, bool isPM) {
  if (isPM) {
    vmem_free(vmp1, set->ts_set);
    vmem_free(vmp1, set);
  } else {
    free(set->ts_set);
    free(set);
  }
}

/*
 *
 */
static inline void ssmem_free_set_make_avail(ssmem_allocator_t *a,
                                             ssmem_free_set_t *set) {
  /* printf("[ALLOC] added to avail_set : %p\n", set); */
  set->curr = 0;
  set->set_next = a->available_set_list;
  a->available_set_list = set;
}

/*
 * terminated allocator a and free its memory
 */
void ssmem_alloc_term(ssmem_allocator_t *a, bool isPM) {
  /* printf("[ALLOC] term() : ~ total mem used: %zu bytes = %zu KB = %zu MB\n",
   */
  /* 	 a->tot_size, a->tot_size / 1024, a->tot_size / (1024 * 1024)); */
  ssmem_list_t *mcur = a->mem_chunks;
  do {
    ssmem_list_t *mnxt = mcur->next;
    if (isPM) {
      vmem_free(vmp1, mcur->obj);
      vmem_free(vmp1, mcur);
    } else {
      free(mcur->obj);
      free(mcur);
    }

    mcur = mnxt;
  } while (mcur != nullptr);

  ssmem_list_t *prv = ssmem_allocator_list;
  ssmem_list_t *cur = ssmem_allocator_list;
  while (cur != nullptr && (uintptr_t)cur->obj != (uintptr_t)a) {
    prv = cur;
    cur = cur->next;
  }

  if (cur == nullptr) {
    printf(
        "[ALLOC] ssmem_alloc_term: could not find %p in the "
        "ssmem_allocator_list\n",
        a);
  } else if (cur == prv) {
    ssmem_allocator_list = cur->next;
  } else {
    prv->next = cur->next;
  }

  if (--ssmem_num_allocators == 0) {
    if (isPM)
      vmem_free(vmp1, a->ts);
    else
      free(a->ts);
  }

  /* printf("[ALLOC] free(free_set)\n"); fflush(stdout); */
  /* freeing free sets */
  ssmem_free_set_t *fs = a->free_set_list;
  while (fs != nullptr) {
    ssmem_free_set_t *nxt = fs->set_next;
    ssmem_free_set_free(fs, isPM);
    fs = nxt;
  }

  /* printf("[ALLOC] free(collected_set)\n"); fflush(stdout); */
  /* freeing collected sets */
  fs = a->collected_set_list;
  while (fs != nullptr) {
    ssmem_free_set_t *nxt = fs->set_next;
    ssmem_free_set_free(fs, isPM);
    fs = nxt;
  }

  /* printf("[ALLOC] free(available_set)\n"); fflush(stdout); */
  /* freeing available sets */
  fs = a->available_set_list;
  while (fs != nullptr) {
    ssmem_free_set_t *nxt = fs->set_next;
    ssmem_free_set_free(fs, isPM);
    fs = nxt;
  }

  /* freeing the relased memory */
  ssmem_released_t *rel = a->released_mem_list;
  while (rel != nullptr) {
    ssmem_released_t *next = rel->next;
    if (isPM) {
      vmem_free(vmp1, rel->mem);
      vmem_free(vmp1, rel);
    } else {
      free(rel->mem);
      free(rel);
    }

    rel = next;
  }
}

/*
 * terminate all allocators
 */
void ssmem_term(bool isPM) {
  while (ssmem_allocator_list != nullptr) {
    ssmem_alloc_term((ssmem_allocator_t *)ssmem_allocator_list->obj, isPM);
  }
}

/*
 *
 */
void ssmem_ts_next() { ssmem_ts_local->version++; }

/*
 *
 */
size_t *ssmem_ts_set_collect(size_t *ts_set, bool isPM) {
  if (ts_set == nullptr) {
    if (isPM)
      ts_set = (size_t *)vmem_malloc(vmp1, ssmem_ts_list_len * sizeof(size_t));
    else
      ts_set = (size_t *)malloc(ssmem_ts_list_len * sizeof(size_t));
    assert(ts_set != nullptr);
  }

  ssmem_ts_t *cur = ssmem_ts_list;
  while (cur != nullptr && cur->id < ssmem_ts_list_len) {
    ts_set[cur->id] = cur->version;
    cur = cur->next;
  }

  return ts_set;
}

/*
 *
 */
void ssmem_ts_set_print(size_t *set) {
  printf("[ALLOC] set: [");
  for (unsigned int i = 0; i < ssmem_ts_list_len; i++) {
    printf("%zu | ", set[i]);
  }
  printf("]\n");
}

#if !defined(PREFETCHW)
#if defined(__x86_64__) | defined(__i386__)
#define PREFETCHW(x) \
  asm volatile("prefetchw %0" ::"m"(*(unsigned long *)(x))) /* write */
#elif defined(__sparc__)
#define PREFETCHW(x) __builtin_prefetch((const void *)(x), 1, 3)
#elif defined(__tile__)
#include <tmc/alloc.h>
#include <tmc/sync.h>
#include <tmc/udn.h>
#define PREFETCHW(x) tmc_mem_prefetch((x), 64)
#else
#warning "You need to define PREFETCHW(x) for your architecture"
#endif
#endif
std::atomic_size_t IDPM(0);
std::atomic_size_t IDDRAM(0);
/*
 *
 */
void *ssmem_alloc(ssmem_allocator_t *&a, size_t size, bool isPM) {
  if (a == nullptr && isPM) {
    auto r = vmem_malloc(vmp1, sizeof(ssmem_allocator_t));
    a = (ssmem_allocator_t *)r;
    ssmem_alloc_init(a, SSMEM_DEFAULT_MEM_SIZE, IDPM++, 1);
  }
  if (a == nullptr && !isPM) {
    a = (ssmem_allocator_t *)malloc(sizeof(ssmem_allocator_t));
    ssmem_alloc_init(a, SSMEM_DEFAULT_MEM_SIZE, IDDRAM++, 0);
  }
  void *m = nullptr;

  /* 1st try to use from the collected memory */
  ssmem_free_set_t *cs = a->collected_set_list;
  if (cs != nullptr) {
    m = (void *)cs->set[--cs->curr];
    PREFETCHW(m);

    if (cs->curr <= 0) {
      a->collected_set_list = cs->set_next;
      a->collected_set_num--;

      ssmem_free_set_make_avail(a, cs);
    }
  } else {
    if ((a->mem_curr + size) >= a->mem_size) {
#if SSMEM_MEM_SIZE_DOUBLE == 1
      a->mem_size <<= 1;
      if (a->mem_size > SSMEM_MEM_SIZE_MAX) {
        a->mem_size = SSMEM_MEM_SIZE_MAX;
      }
#endif
      /* printf("[ALLOC] out of mem, need to allocate (chunk = %llu MB)\n", */
      /* 	 a->mem_size / (1LL<<20)); */
      if (size > a->mem_size) {
        /* printf("[ALLOC] asking for large mem. chunk\n"); */
        while (a->mem_size < size) {
          if (a->mem_size > SSMEM_MEM_SIZE_MAX) {
            fprintf(
                stderr,
                "[ALLOC] asking for memory chunk larger than max (%llu MB) \n",
                SSMEM_MEM_SIZE_MAX / (1024 * 1024LL));
            assert(a->mem_size <= SSMEM_MEM_SIZE_MAX);
          }
          a->mem_size <<= 1;
        }
        /* printf("[ALLOC] new mem size chunk is %llu MB\n", a->mem_size / (1024
         * * 1024LL)); */
      }
      // #if SSMEM_TRANSPARENT_HUGE_PAGES
      //       int ret = posix_memalign(&a->mem, CACHE_LINE_SIZE, a->mem_size);
      //       assert(ret == 0);
      // #else
      if (isPM)
        a->mem = (void *)vmem_aligned_alloc(vmp1, CACHE_LINE_SIZE, a->mem_size);
      else
        a->mem = (void *)aligned_alloc(CACHE_LINE_SIZE, a->mem_size);

      //#endif
      assert(a->mem != nullptr);

      a->mem_curr = 0;

      a->tot_size += a->mem_size;

      ssmem_zero_memory(a);

      struct ssmem_list *new_mem_chunks =
          ssmem_list_node_new(a->mem, a->mem_chunks, isPM);
      BARRIER(new_mem_chunks);

      a->mem_chunks = new_mem_chunks;
      BARRIER(&a->mem_chunks);
    }

    m = (void *)((char *)(a->mem) + a->mem_curr);
    a->mem_curr += size;
  }

#if SSMEM_TS_INCR_ON == SSMEM_TS_INCR_ON_ALLOC || \
    SSMEM_TS_INCR_ON == SSMEM_TS_INCR_ON_BOTH
  ssmem_ts_next();
#endif
  return m;
}

/* return > 0 iff snew is > sold for each entry */
static int ssmem_ts_compare(size_t *s_new, size_t *s_old) {
  int is_newer = 1;
  for (unsigned int i = 0; i < ssmem_ts_list_len; i++) {
    if (s_new[i] <= s_old[i]) {
      is_newer = 0;
      break;
    }
  }
  return is_newer;
}

/* return > 0 iff s_1 is > s_2 > s_3 for each entry */
static int __attribute__((unused))
ssmem_ts_compare_3(size_t *s_1, size_t *s_2, size_t *s_3) {
  int is_newer = 1;
  for (unsigned int i = 0; i < ssmem_ts_list_len; i++) {
    if (s_1[i] <= s_2[i] || s_2[i] <= s_3[i]) {
      is_newer = 0;
      break;
    }
  }
  return is_newer;
}

static void ssmem_ts_set_print_no_newline(size_t *set);

/*
 *
 */
int ssmem_mem_reclaim(ssmem_allocator_t *a, bool isPM) {
  if (__builtin_expect(a->released_num > 0, 0)) {
    ssmem_released_t *rel_cur = a->released_mem_list;
    ssmem_released_t *rel_nxt = rel_cur->next;

    if (rel_nxt != nullptr &&
        ssmem_ts_compare(rel_cur->ts_set, rel_nxt->ts_set)) {
      rel_cur->next = nullptr;
      a->released_num = 1;
      /* find and collect the memory */
      do {
        rel_cur = rel_nxt;
        rel_nxt = rel_nxt->next;
        if (isPM) {
          vmem_free(vmp1, rel_cur->mem);
          vmem_free(vmp1, rel_cur);
        } else {
          free(rel_cur->mem);
          free(rel_cur);
        }
      } while (rel_nxt != nullptr);
    }
  }

  ssmem_free_set_t *fs_cur = a->free_set_list;
  if (fs_cur->ts_set == nullptr) {
    return 0;
  }
  ssmem_free_set_t *fs_nxt = fs_cur->set_next;
  int gced_num = 0;

  if (fs_nxt == nullptr ||
      fs_nxt->ts_set == nullptr) /* need at least 2 sets to compare */
  {
    return 0;
  }

  if (ssmem_ts_compare(fs_cur->ts_set, fs_nxt->ts_set)) {
    gced_num = a->free_set_num - 1;
    /* take the the suffix of the list (all collected free_sets) away from the
free_set list of a and set the correct num of free_sets*/
    fs_cur->set_next = nullptr;
    a->free_set_num = 1;

    /* find the tail for the collected_set list in order to append the new
free_sets that were just collected */
    ssmem_free_set_t *collected_set_cur = a->collected_set_list;
    if (collected_set_cur != nullptr) {
      while (collected_set_cur->set_next != nullptr) {
        collected_set_cur = collected_set_cur->set_next;
      }

      collected_set_cur->set_next = fs_nxt;
    } else {
      a->collected_set_list = fs_nxt;
    }
    a->collected_set_num += gced_num;
  }

  /* if (gced_num) */
  /*   { */
  /*     printf("//collected %d sets\n", gced_num); */
  /*   } */
  return gced_num;
}

/*
 *
 */
void ssmem_free(ssmem_allocator_t *a, void *obj, bool isPM) {
  ssmem_free_set_t *fs = a->free_set_list;
  if ((uintptr_t)fs->curr == (uintptr_t)fs->size) {
    fs->ts_set = ssmem_ts_set_collect(fs->ts_set, isPM);
    ssmem_mem_reclaim(a, isPM);

    /* printf("[ALLOC] free_set is full, doing GC / size of garbage pointers:
     * %10zu = %zu KB\n", garbagep, garbagep / 1024); */
    // printf("???\n");
    ssmem_free_set_t *fs_new =
        ssmem_free_set_get_avail(a, a->fs_size, a->free_set_list, isPM);
    // printf("%lu\n", fs_new);
    a->free_set_list = fs_new;
    a->free_set_num++;
    fs = fs_new;
  }

  fs->set[fs->curr++] = (uintptr_t)obj;
#if SSMEM_TS_INCR_ON == SSMEM_TS_INCR_ON_FREE || \
    SSMEM_TS_INCR_ON == SSMEM_TS_INCR_ON_BOTH
  ssmem_ts_next();
#endif
}

/*
 *
 */
inline void ssmem_release(ssmem_allocator_t *a, void *obj, bool isPM) {
  ssmem_released_t *rel_list = a->released_mem_list;
  ssmem_released_t *rel = ssmem_released_node_new(obj, rel_list, isPM);
  rel->ts_set = ssmem_ts_set_collect(rel->ts_set, isPM);
  int rn = ++a->released_num;
  a->released_mem_list = rel;
  if (rn >= SSMEM_GC_RLSE_SET_SIZE) {
    ssmem_mem_reclaim(a, isPM);
  }
}

/*
 *
 */
static void ssmem_ts_set_print_no_newline(size_t *set) {
  printf("[");
  if (set != nullptr) {
    for (unsigned int i = 0; i < ssmem_ts_list_len; i++) {
      printf("%zu|", set[i]);
    }
  } else {
    printf(" no timestamp yet ");
  }
  printf("]");
}

/*
 *
 */
void ssmem_free_list_print(ssmem_allocator_t *a) {
  printf("[ALLOC] free_set list (%zu sets): \n", a->free_set_num);

  int n = 0;
  ssmem_free_set_t *cur = a->free_set_list;
  while (cur != nullptr) {
    printf("(%-3d | %p::", n++, cur);
    ssmem_ts_set_print_no_newline(cur->ts_set);
    printf(") -> \n");
    cur = cur->set_next;
  }
  printf("nullptr\n");
}

/*
 *
 */
void ssmem_collected_list_print(ssmem_allocator_t *a) {
  printf("[ALLOC] collected_set list (%zu sets): \n", a->collected_set_num);

  int n = 0;
  ssmem_free_set_t *cur = a->collected_set_list;
  while (cur != nullptr) {
    printf("(%-3d | %p::", n++, cur);
    ssmem_ts_set_print_no_newline(cur->ts_set);
    printf(") -> \n");
    cur = cur->set_next;
  }
  printf("nullptr\n");
}

/*
 *
 */
void ssmem_available_list_print(ssmem_allocator_t *a) {
  printf("[ALLOC] avail_set list: \n");

  int n = 0;
  ssmem_free_set_t *cur = a->available_set_list;
  while (cur != nullptr) {
    printf("(%-3d | %p::", n++, cur);
    ssmem_ts_set_print_no_newline(cur->ts_set);
    printf(") -> \n");
    cur = cur->set_next;
  }
  printf("nullptr\n");
}

/*
 *
 */
void ssmem_all_list_print(ssmem_allocator_t *a, int id) {
  printf("[ALLOC] [%-2d] free_set list: %-4zu / collected_set list: %-4zu\n",
         id, a->free_set_num, a->collected_set_num);
}

/*
 *
 */
void ssmem_ts_list_print() {
  printf("[ALLOC] ts list (%u elems): ", ssmem_ts_list_len);
  ssmem_ts_t *cur = ssmem_ts_list;
  while (cur != nullptr) {
    printf("(id: %-2zu / version: %zu) -> ", cur->id, cur->version);
    cur = cur->next;
  }

  printf("nullptr\n");
}

void ssmem_zero_memory(ssmem_allocator_t *a) {
#if SSMEM_ZERO_MEMORY == 1
  memset(a->mem, 0, a->mem_size);
  for (size_t i = 0; i < a->mem_size / CACHE_LINE_SIZE; i += CACHE_LINE_SIZE) {
    BARRIER((int8_t *)a->mem +
            i);  // An asynchronous flush would be sufficient here, since
                 // another BARRIER will be placed next, after creating a new
                 // node for the mem_chunks list
  }
#endif
}