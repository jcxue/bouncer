#ifndef GHOST_CACHE_H_
#define GHOST_CACHE_H_

#include <inttypes.h>
#include "common.h"

typedef struct GhostCacheEntry {
    uint64_t block_num;
    uint8_t  server_num;
    uint8_t  volume_num;
    uint32_t counter[12];
    uint32_t last_access_sub_window_ind;
    struct GhostCacheEntry *lookup_prev;
    struct GhostCacheEntry *lookup_next;
    struct GhostCacheEntry *lru_prev;
    struct GhostCacheEntry *lru_next;
}GhostCacheEntry __attribute__((aligned (64)));

typedef struct GhostCache {
    uint64_t cache_size;
    uint32_t threshold;
    uint8_t  num_sub_windows;
    uint64_t entry_count; // threshold for starting pruning
    uint32_t num_inserts;
    uint32_t num_replaces;
    GhostCacheEntry *cache_entry;
    GhostCacheEntry **lookup_table;
    GhostCacheEntry *lru_head;
    GhostCacheEntry *lru_tail;
}GhostCache;


int ghost_cache_init (uint64_t cache_size, uint32_t threshold,
        uint8_t num_sub_windows, GhostCache *cache);

int  ghost_cache_access  (GhostCache *cache, Request *req);

void ghost_cache_destroy (GhostCache *cache);

#endif
