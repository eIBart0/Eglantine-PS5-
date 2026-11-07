// SPDX-License-Identifier: GPL-3.0-or-later
/* mimalloc as the app's allocator during a game's session. headless/heap_arenas.inc sends
 * malloc, calloc, realloc, posix_memalign and free here once eden_mi_activate has run (at the
 * start of a session, headless/main.cpp); blocks the heap made before stay in it and are freed
 * there, and a request mimalloc has no memory for still goes to the heap.
 *
 * Why: Sony's mspaces never give back what is free inside them. Mario Kart World's heap held
 * 1.9-2.1 GiB with 1.5-1.65 in use, about 400 MiB free and kept from the pool the GPU shares.
 * mimalloc keeps its memory in 64 KiB slices of its arenas and purges slices that stay free
 * (decommits them, purge_delay later); here a 2 MiB block of direct memory goes back to the pool
 * once none of its 16 KiB pages is committed.
 *
 * mimalloc v3.6.1 (Microsoft Research, Daan Leijen, MIT licence), pinned in tools/deps.json, is
 * compiled here as one unit, as its src/static.c is, with the primitives below in place of its
 * prim/prim.c:
 *   - address space from one range reserved when the session starts (up to 16 GiB);
 *   - memory in 2 MiB blocks of direct memory, committed while any of their pages is
 *     (src/memory_pages.cpp CommitMemoryRangeFrom: the pool's top, else the guest's reserve);
 *   - thread data in pthread keys (MI_TLS_MODEL_PTHREADS): the PS5 build's TLS is emulated, and
 *     its first use in a thread allocates.
 * Testing switch "App memory allocator": EDEN_HEAP_MIMALLOC=0 keeps the heap for everything.
 */
#define MI_TLS_MODEL_PTHREADS 1
#ifndef MI_DEBUG
#define MI_DEBUG 0
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mimalloc.h"

#include "prim/os.c"

#include "mimalloc/heap/process.h"
#include "mimalloc/prim.h"

void *eden_heap_reserve(size_t size);             /* headless/heap_pages.cpp */
int eden_heap_commit(void *address, size_t size); /* zeroed memory; 0 when done */
int eden_heap_decommit(void *address, size_t size); /* 1 given back, 0 nothing done, -1 in part */
int sceKernelDebugOutText(int channel, const char *text);

#define EDEN_MI_BLOCK ((size_t)2 << 20)
#define EDEN_MI_PAGE ((size_t)16384)
#define EDEN_MI_MOST ((size_t)16 << 30)
#define EDEN_MI_BLOCKS (EDEN_MI_MOST / EDEN_MI_BLOCK) /* 8192 */

static uintptr_t eden_mi_base; /* the reserved range: set before the first allocation */
static size_t eden_mi_span;
static atomic_flag eden_mi_lock = ATOMIC_FLAG_INIT;
static uint64_t eden_mi_taken[EDEN_MI_BLOCKS / 64];  /* address space handed to mimalloc */
static uint64_t eden_mi_backed[EDEN_MI_BLOCKS / 64]; /* blocks with direct memory behind them */
static uint64_t eden_mi_pages[EDEN_MI_BLOCKS][2];    /* mimalloc's committed 16 KiB pages of each */
static atomic_size_t eden_mi_held_blocks, eden_mi_given_back_blocks, eden_mi_refused;
atomic_int eden_mi_active;

static void eden_mi_acquire(void) {
  while (atomic_flag_test_and_set_explicit(&eden_mi_lock, memory_order_acquire))
    sched_yield();
}
static void eden_mi_release(void) {
  atomic_flag_clear_explicit(&eden_mi_lock, memory_order_release);
}
static int eden_mi_bit(const uint64_t *bits, size_t at) {
  return (int)((bits[at / 64] >> (at % 64)) & 1u);
}
static void eden_mi_set_bit(uint64_t *bits, size_t at, int on) {
  if (on)
    bits[at / 64] |= (uint64_t)1 << (at % 64);
  else
    bits[at / 64] &= ~((uint64_t)1 << (at % 64));
}
static size_t eden_mi_block_count(void) {
  return eden_mi_span / EDEN_MI_BLOCK;
}

/* The pages of block `b` that [begin, end) covers, as its two 64-bit words. */
static void eden_mi_page_mask(size_t b, uintptr_t begin, uintptr_t end, uint64_t mask[2]) {
  const uintptr_t block_begin = eden_mi_base + b * EDEN_MI_BLOCK;
  const uintptr_t from = begin > block_begin ? begin : block_begin;
  const uintptr_t to = end < block_begin + EDEN_MI_BLOCK ? end : block_begin + EDEN_MI_BLOCK;
  mask[0] = mask[1] = 0;
  for (uintptr_t page = (from - block_begin) / EDEN_MI_PAGE; page < (to - block_begin + EDEN_MI_PAGE - 1) / EDEN_MI_PAGE;
       ++page)
    mask[page / 64] |= (uint64_t)1 << (page % 64);
}

static void eden_mi_give_back(size_t b) {
  if (!eden_mi_bit(eden_mi_backed, b))
    return;
  /* 0 (kept, dev-settings heap_return=off) or -1: the memory stays, and is used again. */
  if (eden_heap_decommit((void *)(eden_mi_base + b * EDEN_MI_BLOCK), EDEN_MI_BLOCK) == 1) {
    eden_mi_set_bit(eden_mi_backed, b, 0);
    atomic_fetch_sub_explicit(&eden_mi_held_blocks, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&eden_mi_given_back_blocks, 1, memory_order_relaxed);
  }
}

/* Under the lock. Gives [begin, end) memory; *zero tells whether all of it is new (zeroed). */
static int eden_mi_commit_locked(uintptr_t begin, uintptr_t end, bool *zero) {
  const size_t first = (begin - eden_mi_base) / EDEN_MI_BLOCK, last = (end - 1 - eden_mi_base) / EDEN_MI_BLOCK;
  bool fresh = true;
  for (size_t b = first; b <= last; ++b) {
    if (eden_mi_bit(eden_mi_backed, b)) {
      fresh = false;
      continue;
    }
    if (eden_heap_commit((void *)(eden_mi_base + b * EDEN_MI_BLOCK), EDEN_MI_BLOCK) != 0) {
      /* The blocks this call backed go back: mimalloc takes the whole range as refused. */
      for (size_t k = first; k < b; ++k)
        if ((eden_mi_pages[k][0] | eden_mi_pages[k][1]) == 0)
          eden_mi_give_back(k);
      atomic_fetch_add_explicit(&eden_mi_refused, 1, memory_order_relaxed);
      return ENOMEM;
    }
    eden_mi_set_bit(eden_mi_backed, b, 1);
    atomic_fetch_add_explicit(&eden_mi_held_blocks, 1, memory_order_relaxed);
  }
  for (size_t b = first; b <= last; ++b) {
    uint64_t mask[2];
    eden_mi_page_mask(b, begin, end, mask);
    eden_mi_pages[b][0] |= mask[0];
    eden_mi_pages[b][1] |= mask[1];
  }
  *zero = fresh;
  return 0;
}

/* Under the lock. The pages of [begin, end) are no longer mimalloc's to use; a block none of
 * whose pages is goes back to the pool. */
static void eden_mi_decommit_locked(uintptr_t begin, uintptr_t end) {
  const size_t first = (begin - eden_mi_base) / EDEN_MI_BLOCK, last = (end - 1 - eden_mi_base) / EDEN_MI_BLOCK;
  for (size_t b = first; b <= last; ++b) {
    uint64_t mask[2];
    eden_mi_page_mask(b, begin, end, mask);
    eden_mi_pages[b][0] &= ~mask[0];
    eden_mi_pages[b][1] &= ~mask[1];
    if ((eden_mi_pages[b][0] | eden_mi_pages[b][1]) == 0)
      eden_mi_give_back(b);
  }
}

static int eden_mi_inside(const void *address, size_t size) {
  const uintptr_t at = (uintptr_t)address;
  return eden_mi_span != 0 && at >= eden_mi_base && size <= eden_mi_span && at - eden_mi_base <= eden_mi_span - size;
}

// ---- mimalloc's primitives (include/mimalloc/prim.h) ----

void _mi_prim_mem_init(mi_os_mem_config_t *config) {
  config->page_size = EDEN_MI_PAGE;
  config->large_page_size = 0;
  config->alloc_granularity = EDEN_MI_BLOCK;
  config->physical_memory_in_kib = (size_t)12 << 20;
  config->virtual_address_bits = 47;
  config->has_overcommit = false;
  config->has_partial_free = false;
  config->has_virtual_reserve = true;
  config->has_transparent_huge_pages = false;
}

int _mi_prim_alloc(void *hint_addr, size_t size, size_t try_alignment, bool commit, bool allow_large, bool *is_large,
                   bool *is_zero, void **addr) {
  MI_UNUSED(hint_addr);
  MI_UNUSED(allow_large);
  *is_large = false;
  *is_zero = false;
  *addr = NULL;
  const size_t count = (size + EDEN_MI_BLOCK - 1) / EDEN_MI_BLOCK;
  const size_t blocks = eden_mi_block_count();
  if (count == 0 || count > blocks)
    return ENOMEM;
  /* Aligned as asked (it is only a hint), at least to a block. */
  size_t step = 1, start = 0;
  if (try_alignment > EDEN_MI_BLOCK && try_alignment <= ((size_t)1 << 30)) {
    step = try_alignment / EDEN_MI_BLOCK;
    const uintptr_t aligned = (eden_mi_base + try_alignment - 1) / try_alignment * try_alignment;
    start = (aligned - eden_mi_base) / EDEN_MI_BLOCK;
  }
  eden_mi_acquire();
  size_t found = blocks;
  for (size_t b = start; b + count <= blocks; b += step) {
    size_t i = 0;
    while (i < count && !eden_mi_bit(eden_mi_taken, b + i))
      ++i;
    if (i == count) {
      found = b;
      break;
    }
  }
  if (found == blocks) {
    eden_mi_release();
    return ENOMEM;
  }
  for (size_t i = 0; i < count; ++i)
    eden_mi_set_bit(eden_mi_taken, found + i, 1);
  const uintptr_t at = eden_mi_base + found * EDEN_MI_BLOCK;
  if (commit) {
    bool zero = false;
    if (eden_mi_commit_locked(at, at + count * EDEN_MI_BLOCK, &zero) != 0) {
      for (size_t i = 0; i < count; ++i)
        eden_mi_set_bit(eden_mi_taken, found + i, 0);
      eden_mi_release();
      return ENOMEM;
    }
    *is_zero = zero;
  }
  eden_mi_release();
  *addr = (void *)at;
  return 0;
}

int _mi_prim_free(void *addr, size_t size) {
  if (!eden_mi_inside(addr, size))
    return EINVAL;
  const uintptr_t at = (uintptr_t)addr;
  const size_t first = (at - eden_mi_base) / EDEN_MI_BLOCK;
  const size_t count = (size + EDEN_MI_BLOCK - 1) / EDEN_MI_BLOCK;
  eden_mi_acquire();
  eden_mi_decommit_locked(at, at + count * EDEN_MI_BLOCK);
  for (size_t i = 0; i < count; ++i)
    eden_mi_set_bit(eden_mi_taken, first + i, 0);
  eden_mi_release();
  return 0;
}

int _mi_prim_commit(void *addr, size_t size, bool *is_zero) {
  *is_zero = false;
  if (!eden_mi_inside(addr, size))
    return EINVAL;
  eden_mi_acquire();
  const int result = eden_mi_commit_locked((uintptr_t)addr, (uintptr_t)addr + size, is_zero);
  eden_mi_release();
  return result;
}

int _mi_prim_decommit(void *addr, size_t size, bool *needs_recommit) {
  *needs_recommit = true;
  if (!eden_mi_inside(addr, size))
    return EINVAL;
  eden_mi_acquire();
  eden_mi_decommit_locked((uintptr_t)addr, (uintptr_t)addr + size);
  eden_mi_release();
  return 0;
}

int _mi_prim_reset(void *addr, size_t size) {
  /* The pages stay committed (a purge decommits: mi_option_purge_decommits). */
  MI_UNUSED(addr);
  MI_UNUSED(size);
  return 0;
}

int _mi_prim_reuse(void *addr, size_t size) {
  MI_UNUSED(addr);
  MI_UNUSED(size);
  return 0;
}

int _mi_prim_guard(void *addr, size_t size, bool set, bool is_committed) {
  /* Guard pages only exist in MI_GUARDED and secure builds, which this is not. */
  MI_UNUSED(addr);
  MI_UNUSED(size);
  MI_UNUSED(set);
  MI_UNUSED(is_committed);
  return 0;
}

int _mi_prim_alloc_huge_os_pages(void *hint_addr, size_t size, int numa_node, bool *is_zero, void **addr) {
  MI_UNUSED(hint_addr);
  MI_UNUSED(size);
  MI_UNUSED(numa_node);
  *is_zero = false;
  *addr = NULL;
  return ENOMEM;
}

size_t _mi_prim_numa_node(void) {
  return 0;
}

size_t _mi_prim_numa_node_count(void) {
  return 1;
}

mi_msecs_t _mi_prim_clock_now(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
    return 0;
  return ((mi_msecs_t)t.tv_sec * 1000) + ((mi_msecs_t)t.tv_nsec / 1000000L);
}

void _mi_prim_process_info(mi_process_info_t *pinfo) {
  MI_UNUSED(pinfo);
}

void _mi_prim_out_stderr(const char *msg) {
  /* No stdio: it may allocate. */
  sceKernelDebugOutText(0, msg);
}

int _mi_prim_getenv(const char *name, char *result, size_t result_size) {
  const char *value = getenv(name);
  if (value == NULL)
    return 0;
  const size_t length = strlen(value);
  if (length + 1 > result_size)
    return -1;
  memcpy(result, value, length + 1);
  return 1;
}

bool _mi_prim_random_buf(void *buf, size_t buf_len) {
  MI_UNUSED(buf);
  MI_UNUSED(buf_len);
  return false; /* mimalloc falls back to its own weak random */
}

static _Atomic(pthread_key_t) eden_mi_theap_key = MI_ATOMIC_VAR_INIT(MI_PTHREAD_KEY_INVALID);

static void eden_mi_thread_done(void *value) {
  if (value != NULL)
    _mi_thread_done((mi_theap_t *)value);
}

void _mi_prim_thread_init_auto_done(void) {
  pthread_key_t key;
  if (pthread_key_create(&key, &eden_mi_thread_done) == 0)
    mi_atomic_store_release(&eden_mi_theap_key, key);
}

void _mi_prim_thread_done_auto_done(void) {
  pthread_key_t key = mi_atomic_exchange_relaxed(&eden_mi_theap_key, MI_PTHREAD_KEY_INVALID);
  if (key != MI_PTHREAD_KEY_INVALID)
    pthread_key_delete(key);
}

void _mi_prim_thread_associate_default_theap(mi_theap_t *theap) {
  pthread_key_t key = mi_atomic_load_relaxed(&eden_mi_theap_key);
  if (key != MI_PTHREAD_KEY_INVALID)
    pthread_setspecific(key, theap);
}

bool _mi_prim_thread_is_in_threadpool(void) {
  return false;
}

void _mi_prim_thread_yield(void) {
  sched_yield();
}

bool _mi_prim_process_is_killed(void) {
  return false;
}

bool _mi_is_redirected(void) {
  return false;
}

bool _mi_allocator_init(const char **message) {
  if (message != NULL)
    *message = NULL;
  return true;
}

void _mi_allocator_done(void) {}

// ---- the rest of mimalloc, as src/static.c lists it ----
#include "prim/prim-tls.c"

#include "util/libc.c"
#include "util/options.c"
#include "util/random.c"
#include "util/stats.c"
#include "util/threadlocal.c"

#include "arena/arena.c"
#include "arena/arena-alloc.c"
#include "arena/arena-page.c"
#include "arena/bitmap-chunk.c"
#include "arena/bitmap.c"

#include "alloc/alloc.c"
#include "alloc/alloc-generic.c"
#include "alloc/alloc-aligned.c"
#include "alloc/alloc-posix.c"
#include "alloc/page.c"
#include "alloc/page-queue.c"

#include "heap/heap.c"
#include "heap/process.c"
#include "heap/page-map.c"
#include "heap/subproc.c"
#include "heap/theap.c"

#include "profile/sample-guarded.c"
#include "profile/sample-profile.c"
#include "profile/pprof.c"

#if defined(__FreeBSD__)
/* The heap profiler's list of the process's mappings (profile/pprof.c): the PS5 has no libutil,
 * and the profiler is never started here; it takes NULL as "none". */
struct kinfo_vmentry *kinfo_getvmmap(pid_t pid, int *count) {
  (void)pid;
  if (count != NULL)
    *count = 0;
  return NULL;
}
#endif

// ---- what headless/heap_arenas.inc and the reports call ----

int eden_mi_owns(const void *address) {
  const uintptr_t at = (uintptr_t)address;
  return at - eden_mi_base < eden_mi_span; /* base and span are 0 until the range is reserved */
}

/* Reserves the range and starts mimalloc; 1 when it now takes the app's allocations. */
int eden_mi_activate(void) {
  static atomic_int tried;
  if (atomic_exchange(&tried, 1))
    return atomic_load(&eden_mi_active);
  const char *setting = getenv("EDEN_HEAP_MIMALLOC");
  if (setting != NULL && setting[0] == '0')
    return 0;
  void *range = NULL;
  size_t span = EDEN_MI_MOST;
  for (; span >= ((size_t)2 << 30); span /= 2)
    if ((range = eden_heap_reserve(span)) != NULL)
      break;
  if (range == NULL) {
    sceKernelDebugOutText(0, "EDEN_MIMALLOC no address range: the heap keeps every allocation\n");
    return 0;
  }
  eden_mi_base = (uintptr_t)range;
  eden_mi_span = span;
  mi_option_set(mi_option_arena_eager_commit, 0);
  mi_option_set(mi_option_allow_large_os_pages, 0);
  mi_option_set(mi_option_allow_thp, 0);
  mi_option_set(mi_option_purge_decommits, 1);
  /* Free slices go back within a tenth of a second, and only in whole blocks' worth. */
  mi_option_set(mi_option_purge_delay, 10);
  mi_option_set(mi_option_arena_purge_min_size, (long)(EDEN_MI_BLOCK / 1024));
  mi_option_set(mi_option_show_errors, 1);
  _mi_auto_process_init(); /* as the loader's constructor would have, before main */
  atomic_store_explicit(&eden_mi_active, 1, memory_order_release);
  char line[128];
  snprintf(line, sizeof(line), "EDEN_MIMALLOC active: %zu MiB of address space at %p\n", span >> 20, range);
  sceKernelDebugOutText(0, line);
  return 1;
}

/* What mimalloc holds of the pool and what it has given back since the session began. */
void eden_mi_usage(size_t *held, size_t *given_back, size_t *refused) {
  *held = atomic_load_explicit(&eden_mi_held_blocks, memory_order_relaxed) * EDEN_MI_BLOCK;
  *given_back = atomic_load_explicit(&eden_mi_given_back_blocks, memory_order_relaxed) * EDEN_MI_BLOCK;
  *refused = atomic_load_explicit(&eden_mi_refused, memory_order_relaxed);
}

/* Free memory back now (the pool is short, headless/performance.cpp TrimHeap): this thread's
 * cached pages and every arena's purgeable slices, without waiting for purge_delay. */
void eden_mi_collect(void) {
  if (atomic_load_explicit(&eden_mi_active, memory_order_acquire))
    mi_collect(true);
}

void *eden_mi_malloc(size_t size) {
  return mi_malloc(size);
}
void *eden_mi_calloc(size_t count, size_t size) {
  return mi_calloc(count, size);
}
void *eden_mi_realloc(void *address, size_t size) {
  return mi_realloc(address, size);
}
void eden_mi_free(void *address) {
  mi_free(address);
}
int eden_mi_posix_memalign(void **address, size_t alignment, size_t size) {
  return mi_posix_memalign(address, alignment, size);
}
size_t eden_mi_usable_size(const void *address) {
  return mi_usable_size(address);
}
