#ifndef LRU_CACHE_H_
#define LRU_CACHE_H_

#include "common.h"

typedef struct LRUCacheEntry {
    uint64_t                     write_access_count;
    uint64_t                     block_num;
    uint8_t                      server_num;
    uint8_t                      volume_num;
    struct LRUCacheEntry        *lookup_next;
    struct LRUCacheEntry        *lookup_prev;
    struct LRUCacheEntry        *lru_next;
    struct LRUCacheEntry        *lru_prev;
} LRUCacheEntry __attribute__((aligned (64)));


typedef struct LRUCache {
    uint64_t           cache_size;         // The total number of entries
    uint64_t           entry_count;        // The number of entries currently being occupied
    uint64_t           lookup_table_size;  // The size of hash lookup table
    LRUCacheEntry     *cache_entry;
    LRUCacheEntry    **lookup_table;
    LRUCacheEntry     *lru_head;
    LRUCacheEntry     *lru_tail;
    uint64_t           read_lookups;
    uint64_t           write_lookups;
    uint64_t           read_hits;
    uint64_t           write_hits;
    uint64_t           num_writes;
    uint64_t           num_updates;
    uint64_t           num_removes;
    uint64_t           num_replaces;
}LRUCache;

int  lru_cache_init    (uint32_t cache_size, LRUCache *lru_cache);
int  lru_cache_lookup  (LRUCache *lru_cache, Request *req);
int  lru_cache_read_lookup  (LRUCache *lru_cache, Request *req);
int  lru_cache_peek (LRUCache *cache, Request *req);
int  lru_cache_insert  (LRUCache *lru_cache, uint64_t block_num,
        uint8_t server_num, uint8_t volume_num, Request *replaced_req);
int  lru_cache_remove  (LRUCache *lru_cache, Request *req);
int  lru_cache_update  (LRUCache *lru_cache, Request *req);
void lru_cache_destroy (LRUCache *lru_cache);
void lru_cache_test    ();

#endif
