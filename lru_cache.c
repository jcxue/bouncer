#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "debug.h"
#include "common.h"
#include "lru_cache.h"

/*
 * Assuming the lookup_table_size is multiples of 512
 * so that the lookup_table would occupy multiples of
 * 4K pages.
 *
 * lookup_table_size is 8 times smaller than the
 * cache_size so that the open chaining would not
 * be longer than 8.
 */
uint32_t get_lookup_table_size (uint32_t cache_size)
{
    uint32_t lookup_table_size = 512;
    uint32_t multiple          = (cache_size >> 12) + 1;

    return (lookup_table_size * multiple);
}

int lru_cache_init (uint32_t cache_size, LRUCache *cache)
{
    cache->cache_size        = cache_size;
    check (cache_size>=2, "the SSD cache size should at least be 2.");

    cache->entry_count       = 0;
    if (cache_size < 64) {
        cache->lookup_table_size = cache_size;
    } else {
        cache->lookup_table_size = get_lookup_table_size (cache_size);
    }

    /* allocate space for cache_entry */
    cache->cache_entry = (LRUCacheEntry *) calloc (cache->cache_size,
            sizeof(LRUCacheEntry));
    check(cache->cache_entry!=NULL,
            "failed to allocate space for lru_cache->cache_entry.");

    /* allocate space for lookup_table */
    cache->lookup_table = (LRUCacheEntry **) calloc (
            cache->lookup_table_size, sizeof(LRUCacheEntry *));
    check(cache->lookup_table!=NULL,
            "failed to allocate lru_cache->lookup table.");

    cache->lru_head = NULL;
    cache->lru_tail = NULL;

    return 0;

error:

    return -1;
}

/* check if the request could hit in the cache.
 *
 * returns 0: cache miss
 *         1: cache hit plus updating LRU info
 *
 */
int lru_cache_lookup (LRUCache *cache, Request *req)
{
/*    if ((req->block_num == 3314217) && (req->server_num == 0)
            && (req->volume_num == 0)) {
        printf("debug\n");
    }*/
    int exists = 0;
    int table_slot = req->block_num % cache->lookup_table_size;
    LRUCacheEntry  *cache_entry  = cache->lookup_table[table_slot];

    if (req->req_type == 0) {
        cache->read_lookups += 1;
    } else {
        cache->write_lookups += 1;
    }

    while (cache_entry != NULL) {
        if ((cache_entry->block_num == req->block_num)   &&
            (cache_entry->server_num == req->server_num) &&
            (cache_entry->volume_num == req->volume_num)) {
            exists = 1;
            break;
        } else {
            cache_entry = cache_entry->lookup_next;
        }
    }

    if (exists) {
        if (req->req_type == 0) {
            cache->read_hits += 1;
        } else {
            cache->write_hits += 1;
            cache_entry->write_access_count += 1;
        }
    }

    if (exists)
    {
        /* update LRU information */
        if ((cache_entry->lru_prev == NULL) && (cache_entry->lru_next == NULL)) {
            /* the only entry in the cache, do nothing. */
        } else {
            if (cache_entry == cache->lru_tail) {
                cache->lru_tail = cache_entry->lru_prev;
                cache_entry->lru_prev->lru_next = NULL;
                cache->lru_head->lru_prev = cache_entry;
                cache_entry->lru_next = cache->lru_head;
                cache_entry->lru_prev = NULL;
                cache->lru_head = cache_entry;
            } else {
                if (cache_entry == cache->lru_head) {
                    /* do nothing */
                } else {
                    cache_entry->lru_prev->lru_next = cache_entry->lru_next;
                    cache_entry->lru_next->lru_prev = cache_entry->lru_prev;
                    cache_entry->lru_prev = NULL;
                    cache_entry->lru_next = cache->lru_head;
                    cache->lru_head->lru_prev = cache_entry;
                    cache->lru_head = cache_entry;
                }
            }
        }

        /* move the entry to the front of lookup chain */
        if ((cache_entry->lookup_prev == NULL) && (cache_entry->lookup_next == NULL)) {
            /* the only entry in the chain, do nothing */
        } else {
            if (cache->lookup_table[table_slot] == cache_entry) {
                /* it is the first entry already. do nothing. */
            } else {
                cache_entry->lookup_prev->lookup_next = cache_entry->lookup_next;
                if (cache_entry->lookup_next != NULL) {
                    cache_entry->lookup_next->lookup_prev =
                            cache_entry->lookup_prev;
                }

                cache_entry->lookup_prev = NULL;
                cache_entry->lookup_next = cache->lookup_table[table_slot];
                cache->lookup_table[table_slot]->lookup_prev = cache_entry;
                cache->lookup_table[table_slot] = cache_entry;
            } // else (the first entry in the chain)
        } // if (the only entry in the chain )

    } // if (exists)

    return exists;
}

int lru_cache_read_lookup (LRUCache *cache, Request *req)
{
    int exists = 0;
    int table_slot = req->block_num % cache->lookup_table_size;
    LRUCacheEntry  *cache_entry  = cache->lookup_table[table_slot];
    int num_lookups = 1;
    if (req->req_type == 0) {
        cache->read_lookups += 1;
    } else {
        cache->write_lookups += 1;
    }

    while (cache_entry != NULL) {
        num_lookups += 1;
        check (num_lookups <= cache->cache_size, "lookups more than cache size.");
        if ((cache_entry->block_num == req->block_num)   &&
            (cache_entry->server_num == req->server_num) &&
            (cache_entry->volume_num == req->volume_num)) {
            exists = 1;
            break;
        } else {
            cache_entry = cache_entry->lookup_next;
        }
    }

    if (exists) {
        if (req->req_type == 0) {
            cache->read_hits += 1;
        } else {
            cache->write_hits += 1;
        }
    }

    return exists;

error:

    return -1;
}

int lru_cache_peek (LRUCache *cache, Request *req)
{
    int exists = 0;
    int table_slot = req->block_num % cache->lookup_table_size;
    LRUCacheEntry  *cache_entry  = cache->lookup_table[table_slot];
    LRUCacheEntry  *first_entry  = cache_entry;
    int num_lookups = 0;

    while (cache_entry != NULL) {
        num_lookups += 1;
        check (num_lookups <= cache->cache_size, "lookups more than cache size.");
        if ((cache_entry->block_num == req->block_num)   &&
            (cache_entry->server_num == req->server_num) &&
            (cache_entry->volume_num == req->volume_num)) {
            exists = 1;
            break;
        } else {
            cache_entry = cache_entry->lookup_next;
            if (cache_entry == first_entry) {
                printf("loop detected\n.");
            }
        }
    }

    return exists;

error:

    return -1;
}

int lru_cache_update (LRUCache *cache, Request *req)
{
/*    if ((req->block_num == 3314217) && (req->server_num == 0)
            && (req->volume_num == 0)) {
        printf("debug\n");
    }*/
    int exists = 0;
    int table_slot = req->block_num % cache->lookup_table_size;
    LRUCacheEntry *cache_entry = cache->lookup_table[table_slot];

    while (cache_entry != NULL) {
        if ((cache_entry->block_num == req->block_num)
                && (cache_entry->server_num == req->server_num)
                && (cache_entry->volume_num == req->volume_num)) {
            exists = 1;
            break;
        } else {
            cache_entry = cache_entry->lookup_next;
        }
    }

    check (exists == 1, "failed to find entry to update.");

    cache->num_updates += 1;

    // updating LRU information
    if (cache_entry != cache->lru_head) {
        if (cache_entry->lru_prev != NULL) {
            cache_entry->lru_prev->lru_next = cache_entry->lru_next;
        }
        if (cache_entry->lru_next != NULL) {
            cache_entry->lru_next->lru_prev = cache_entry->lru_prev;
        }

        if (cache_entry == cache->lru_tail) {
            cache->lru_tail = cache_entry->lru_prev;
        }

        cache_entry->lru_prev = NULL;
        cache_entry->lru_next = cache->lru_head;
        cache->lru_head->lru_prev = cache_entry;
        cache->lru_head = cache_entry;
    }

    /* update lookup info, move it to the front of the chain */
    if (cache->lookup_table[table_slot] != cache_entry) {
        if (cache_entry->lookup_prev != NULL) {
            cache_entry->lookup_prev->lookup_next = cache_entry->lookup_next;
        }
        if (cache_entry->lookup_next != NULL) {
            cache_entry->lookup_next->lookup_prev = cache_entry->lookup_prev;
        }

        cache_entry->lookup_prev = NULL;
        cache_entry->lookup_next = cache->lookup_table[table_slot];

        if (cache->lookup_table[table_slot] != NULL) {
            cache->lookup_table[table_slot]->lookup_prev = cache_entry;
        }
        cache->lookup_table[table_slot] = cache_entry;
    }

    return 0;

error:

    return -1;
}

int lru_cache_remove (LRUCache *cache, Request *req)
{
/*    if ((req->block_num == 3314217) && (req->server_num == 0)
            && (req->volume_num == 0)) {
        printf("debug\n");
    }*/
    int exists = 0;
    int table_slot = req->block_num % cache->lookup_table_size;
    LRUCacheEntry *cache_entry = cache->lookup_table[table_slot];

    while (cache_entry != NULL) {
        if ((cache_entry->block_num == req->block_num)
                && (cache_entry->server_num == req->server_num)
                && (cache_entry->volume_num == req->volume_num)) {
            exists = 1;
            break;
        } else {
            cache_entry = cache_entry->lookup_next;
        }
    }

    check (exists == 1, "failed to find entry to remove.");

    cache->num_removes += 1;

    // updating LRU information

    if ((cache_entry == cache->lru_head) && (cache_entry == cache->lru_tail)) {
        // the only entry in the cache
        check (cache->entry_count == 1, "failed to verify to be the only entry.");
        cache->lru_head = NULL;
        cache->lru_tail = NULL;
        cache->entry_count = 0;
        cache->lookup_table[table_slot] = NULL;

        return 0;
    } else {
        if (cache_entry == cache->lru_head) {
            if (cache_entry->lru_next != NULL) {
                cache_entry->lru_next->lru_prev = NULL;
            }
            cache->lru_head = cache_entry->lru_next;
        } else if (cache_entry == cache->lru_tail) {
            if (cache_entry->lru_prev != NULL) {
                cache_entry->lru_prev->lru_next = NULL;
            }
            cache->lru_tail = cache_entry->lru_prev;
        } else {
            cache_entry->lru_prev->lru_next = cache_entry->lru_next;
            cache_entry->lru_next->lru_prev = cache_entry->lru_prev;
        }
    }
    cache_entry->lru_next = NULL;
    cache_entry->lru_prev = NULL;

    /* update lookup info */
    if (cache->lookup_table[table_slot] == cache_entry) {
        check(cache_entry->lookup_prev==NULL, "failed to verify lookup head.");
        if (cache_entry->lookup_next != NULL) {
            cache_entry->lookup_next->lookup_prev = NULL;
        }
        cache->lookup_table[table_slot] = cache_entry->lookup_next;
    } else if (cache_entry->lookup_next == NULL) {
        cache_entry->lookup_prev->lookup_next = NULL;
    } else {
        cache_entry->lookup_prev->lookup_next = cache_entry->lookup_next;
        cache_entry->lookup_next->lookup_prev = cache_entry->lookup_prev;
    }
    cache_entry->lookup_next = NULL;
    cache_entry->lookup_prev = NULL;

    /* move the last entry to the current position*/

    check (cache->entry_count >= 1, "failed to remove entry from empty cache.");
    LRUCacheEntry *entry = &(cache->cache_entry[cache->entry_count-1]);
/*    if ((entry->block_num == 3314217) && (entry->server_num == 0)
            && (entry->volume_num == 0)) {
        printf("debug\n");
    }*/

    if (cache_entry == entry) {
        cache->entry_count -= 1;
        return 0;
    }

    if (entry->lru_prev != NULL) {
        entry->lru_prev->lru_next = cache_entry;
    }

    if (entry->lru_next != NULL) {
        entry->lru_next->lru_prev = cache_entry;
    }

    if (entry->lookup_prev != NULL) {
        entry->lookup_prev->lookup_next = cache_entry;
    }

    if (entry->lookup_next != NULL) {
        entry->lookup_next->lookup_prev = cache_entry;
    }

    table_slot = entry->block_num % cache->lookup_table_size;
    if (cache->lookup_table[table_slot] == entry) {
        cache->lookup_table[table_slot] = cache_entry;
    }

    if (cache->lru_head == entry) {
        cache->lru_head = cache_entry;
    }

    if (cache->lru_tail == entry) {
        cache->lru_tail = cache_entry;
    }

    /*copy the content of the last entry to the current position */
    cache_entry->block_num = entry->block_num;
    cache_entry->server_num = entry->server_num;
    cache_entry->volume_num = entry->volume_num;
    cache_entry->write_access_count = entry->write_access_count;
    cache_entry->lookup_next = entry->lookup_next;
    cache_entry->lookup_prev = entry->lookup_prev;
    cache_entry->lru_next = entry->lru_next;
    cache_entry->lru_prev = entry->lru_prev;

    /* move entry to the lru head */
    if (cache->lru_head != cache_entry) {
        if (cache->lru_tail == cache_entry ){
            cache_entry->lru_prev->lru_next = NULL;
            cache->lru_tail = cache_entry->lru_prev;
            cache_entry->lru_prev = NULL;
            cache_entry->lru_next = cache->lru_head;
            cache->lru_head->lru_prev = cache_entry;
            cache->lru_head = cache_entry;
        } else {
            cache_entry->lru_prev->lru_next = cache_entry->lru_next;
            cache_entry->lru_next->lru_prev = cache_entry->lru_prev;
            cache_entry->lru_prev = NULL;
            cache_entry->lru_next = cache->lru_head;
            cache->lru_head->lru_prev = cache_entry;
            cache->lru_head = cache_entry;
        }
    }

    cache->entry_count -= 1;

    return 0;

error:

    return -1;
}

int lru_cache_insert  (LRUCache *cache, uint64_t block_num, uint8_t server_num,
                        uint8_t volume_num, Request *replaced_req)
{

/*    if ((block_num == 3314217) && (server_num == 0)
            && (volume_num == 0)) {
        printf("debug\n");
    }*/

    LRUCacheEntry *cache_entry = NULL;
    int            table_slot  = -1;
    int            ret         = 0;

    cache->num_writes += 1;

    if (cache->entry_count == cache->cache_size) {
        /* replacing an existing entry */
        cache_entry = cache->lru_tail;
        replaced_req->block_num = cache_entry->block_num;
        replaced_req->server_num = cache_entry->server_num;
        replaced_req->volume_num = cache_entry->volume_num;
        cache->num_replaces += 1;
        ret = 1;

        /* updating LRU info */
        cache->lru_tail = cache_entry->lru_prev;
        cache_entry->lru_prev->lru_next = NULL;

        cache_entry->lru_prev = NULL;
        cache_entry->lru_next = cache->lru_head;
        cache->lru_head->lru_prev = cache_entry;
        cache->lru_head = cache_entry;

        /* update lookup info */
        /* remove it from the original lookup chain */

        table_slot = cache_entry->block_num % cache->lookup_table_size;

        if (cache->lookup_table[table_slot] == cache_entry) {
            /*if it is the first one in the chain */
            if (cache_entry->lookup_next == NULL) {
                /* if it is the only one in the chain */
                cache->lookup_table[table_slot] = NULL;
            } else {
                /* if it is first one but not the only one */
                cache->lookup_table[table_slot] = cache_entry->lookup_next;
                cache_entry->lookup_next->lookup_prev = NULL;
            }
        } else {
            /* if it is not the first one in the chain */
            if (cache_entry->lookup_next == NULL) {
                /* if it is the last one in the chain */
                cache_entry->lookup_prev->lookup_next = NULL;
            } else {
                /* if it is neither the first one nor the last one */
                cache_entry->lookup_prev->lookup_next = cache_entry->lookup_next;
                cache_entry->lookup_next->lookup_prev = cache_entry->lookup_prev;
            }
        }

        /* insert it to the head of the new lookup chain */
        table_slot = block_num % cache->lookup_table_size;
        cache_entry->lookup_prev = NULL;
        cache_entry->lookup_next = cache->lookup_table[table_slot];
        if (cache->lookup_table[table_slot] != NULL) {
            cache->lookup_table[table_slot]->lookup_prev = cache_entry;
        }
        cache->lookup_table[table_slot] = cache_entry;
        cache_entry->block_num  = block_num;
        cache_entry->server_num = server_num;
        cache_entry->volume_num = volume_num;
        cache_entry->write_access_count = 0;
    } else {
        /* inserting a new entry */
        cache_entry = &(cache->cache_entry[cache->entry_count]);
        cache_entry->block_num  = block_num;
        cache_entry->server_num = server_num;
        cache_entry->volume_num = volume_num;
        cache_entry->write_access_count = 0;

        /* update LRU info */
        if (cache->lru_head == NULL) {
            /* very first entry */
            cache->lru_head = cache_entry;
            cache->lru_tail = cache_entry;
            cache_entry->lru_prev = NULL;
            cache_entry->lru_next = NULL;
        } else {
            cache_entry->lru_prev = NULL;
            cache_entry->lru_next = cache->lru_head;
            cache->lru_head->lru_prev = cache_entry;
            cache->lru_head = cache_entry;
        }

        /* update lookup info */
        table_slot = block_num % cache->lookup_table_size;

        cache_entry->lookup_prev = NULL;
        cache_entry->lookup_next = cache->lookup_table[table_slot];

        if (cache->lookup_table[table_slot] != NULL) {
            cache->lookup_table[table_slot]->lookup_prev = cache_entry;
        }
        cache->lookup_table[table_slot] = cache_entry;

        cache->entry_count += 1;
    }

    return ret;
}

void lru_cache_destroy (LRUCache *cache)
{
    if (cache->cache_entry != NULL) {
        free (cache->cache_entry);
    }

    if (cache->lookup_table != NULL) {
        free (cache->lookup_table);
    }

    free (cache);
}

void print_lru_cache (LRUCache *cache)
{
    int i;
    LRUCacheEntry *cache_entry = NULL;

    printf("++++++++++++++++++++++++++++++++++++\n");
    for (i = 0; i < cache->lookup_table_size; i++) {
        printf("SLOT %d: ", i);
        cache_entry = cache->lookup_table[i];
        while (cache_entry != NULL) {
            printf("%"PRIu64"\t", cache_entry->block_num);
            cache_entry = cache_entry->lookup_next;
        }
        printf("\n");
    }

    printf("LRU:\t");
    cache_entry = cache->lru_head;
    while (cache_entry != NULL) {
        printf("%"PRIu64"\t", cache_entry->block_num);
        cache_entry = cache_entry->lru_next;
    }
    printf("\n++++++++++++++++++++++++++++++++++++\n\n");

}

/*void lru_cache_test ()
{
    int ret = 0;
    int i;
    int block_array[20] = { 1, 1, 2, 1, 2, 3, 1, 2, 3, 4, 1, 2, 3, 4, 5, 1, 2,
            3, 4, 5 };
    Request req, replaced_req;

    LRUCache *lru_cache = (LRUCache *) calloc(1, sizeof(LRUCache));

    ret = lru_cache_init(16, lru_cache);

    for (i = 0; i < 20; i++) {
        req.block_num = block_array[i];
        req.server_num = 0;
        req.volume_num = 0;

        ret = lru_cache_lookup(lru_cache, &req);
        if (ret == 0) {
            lru_cache_insert(lru_cache, req.block_num, req.server_num,
                    req.volume_num, &replaced_req);
        }

        print_lru_cache(lru_cache);
    }

    lru_cache_destroy(lru_cache);
}*/

