#include <stdio.h>
#include <stdlib.h>

#include "debug.h"
#include "ghost_cache.h"

int ghost_cache_init (uint64_t cache_size, uint32_t threshold,
        uint8_t num_sub_windows, GhostCache *cache)
{
    cache->cache_size = cache_size;
    cache->threshold = threshold;
    cache->num_sub_windows = num_sub_windows;

    cache->lru_head = NULL;
    cache->lru_tail = NULL;

    cache->cache_entry = (GhostCacheEntry *) calloc (cache_size,
            sizeof(GhostCacheEntry));
    check (cache->cache_entry!=NULL, "failed to allocate cache_entry.");

    cache->lookup_table = (GhostCacheEntry **) calloc (cache_size,
            sizeof(GhostCacheEntry *));
    check (cache->lookup_table!=NULL, "failed to allocate lookup_table.");

    return 0;

error:

    return -1;
}

/* Return values:
 *      0  -  do nothing
 *      1  -  should be allocated
 *     -1  -  on error
 */

int ghost_cache_access  (GhostCache *cache, Request *req)
{
    int ret = 0, i, ind;
    uint64_t miss_count = 0;
    uint32_t lookup_table_slot = req->block_num % cache->cache_size;
    GhostCacheEntry *entry     = cache->lookup_table[lookup_table_slot];
    uint32_t last_sub_window_ind;
    uint32_t sub_window_ind_diff;
    uint8_t num_sub_windows = cache->num_sub_windows;
    uint8_t curr_sub_window_counter_ind = req->sub_window_ind
            % num_sub_windows;

/*    printf ("block_num = %"PRIu64" sever_num = %"PRIu8" "
            "volume_num = %"PRIu8"\n", req->block_num, req->server_num,
            req->volume_num);*/

/*    if (lookup_table_slot == 368640) {
        printf("debug\n");
    }*/

    while (entry != NULL) {
        if ((entry->block_num == req->block_num)
                && (entry->volume_num == req->volume_num)
                && (entry->server_num == req->server_num)) {
            break;
        }
        entry = entry->lookup_next;
    }

    if (entry != NULL) {
        // hits in ghost cache
        // find the miss_count
        last_sub_window_ind = entry->last_access_sub_window_ind;
        sub_window_ind_diff = req->sub_window_ind - last_sub_window_ind;
        uint8_t last_sub_window_counter_ind = last_sub_window_ind
                % num_sub_windows;

        if (sub_window_ind_diff >= num_sub_windows) {
            for (i = 0; i < num_sub_windows; i++) {
                entry->counter[i] = 0;
            }
        } else if (sub_window_ind_diff != 0) {
            ind = last_sub_window_counter_ind;
            ind = (ind + 1) % num_sub_windows;
            while (ind != curr_sub_window_counter_ind) {
                entry->counter[ind] = 0;
                ind = (ind + 1) % num_sub_windows;
            }
            entry->counter[ind] = 0;
        }

        for (i = 0; i < num_sub_windows; i++) {
            miss_count += entry->counter[i];
        }

        if (miss_count >= cache->threshold) {
            ret = 1;
        }  else {
            ret = 0;
        }

        entry->counter[curr_sub_window_counter_ind] += 1;
        entry->last_access_sub_window_ind = req->sub_window_ind;

        // update LRU information
        if ((cache->lru_head == entry) && (cache->lru_tail == entry)) {
            // only entry in the cache, do nothing
        } else {
            if (cache->lru_head == entry) {
                // do nothing
            } else {
                if (cache->lru_tail == entry) {
                    entry->lru_prev->lru_next = NULL;
                    cache->lru_tail = entry->lru_prev;
                    entry->lru_prev = NULL;
                    entry->lru_next = cache->lru_head;
                    cache->lru_head->lru_prev = entry;
                    cache->lru_head = entry;
                } else {
                    entry->lru_prev->lru_next = entry->lru_next;
                    entry->lru_next->lru_prev = entry->lru_prev;
                    entry->lru_prev = NULL;
                    entry->lru_next = cache->lru_head;
                    cache->lru_head->lru_prev = entry;
                    cache->lru_head = entry;
                }
            }
        }

        // move the entry to the front of the lookup list
        if (cache->lookup_table[lookup_table_slot] == entry) {
            // do nothing
        } else {
            entry->lookup_prev->lookup_next = entry->lookup_next;
            if (entry->lookup_next != NULL) {
                entry->lookup_next->lookup_prev = entry->lookup_prev;
            }
            entry->lookup_prev = NULL;
            entry->lookup_next = cache->lookup_table[lookup_table_slot];
            cache->lookup_table[lookup_table_slot]->lookup_prev = entry;
            cache->lookup_table[lookup_table_slot] = entry;
        }

/*        if (lookup_table_slot == 368640) {
            GhostCacheEntry *tmp_entry = cache->lookup_table[lookup_table_slot];
            while (tmp_entry != NULL) {
                tmp_entry = tmp_entry->lookup_next;
            }
        }*/
    } // hits in ghost cache
    else {
        // misses in ghost cache
        if (cache->entry_count == cache->cache_size) {
            // replacing an existing entry
            cache->num_replaces += 1;
            entry = cache->lru_tail;

            // remove entry from the old lookup list
            lookup_table_slot = entry->block_num % cache->cache_size;
            if (cache->lookup_table[lookup_table_slot] == entry) {
                // the first entry in the lookup list
                cache->lookup_table[lookup_table_slot] = entry->lookup_next;
                if (entry->lookup_next != NULL) {
                    entry->lookup_next->lookup_prev = NULL;
                }
            } else {
                // not the first entry
                entry->lookup_prev->lookup_next = entry->lookup_next;
                if (entry->lookup_next != NULL) {
                    entry->lookup_next->lookup_prev = entry->lookup_prev;
                }
            }

            // set new values to the entry
            entry->block_num = req->block_num;
            entry->server_num = req->server_num;
            entry->volume_num = req->volume_num;
            entry->counter[curr_sub_window_counter_ind] = 1;
            entry->last_access_sub_window_ind = req->sub_window_ind;

            // move the new entry to the new lookup position
            lookup_table_slot = entry->block_num % cache->cache_size;
            if (cache->lookup_table[lookup_table_slot] == NULL) {
                cache->lookup_table[lookup_table_slot] = entry;
                entry->lookup_next = NULL;
                entry->lookup_prev = NULL;
            } else {
                cache->lookup_table[lookup_table_slot]->lookup_prev = entry;
                entry->lookup_next = cache->lookup_table[lookup_table_slot];
                entry->lookup_prev = NULL;
                cache->lookup_table[lookup_table_slot] = entry;
            }

            // update LRU info
            if (entry == cache->lru_head) {
                // the only entry in the cache, do nothing
            } else {
                entry->lru_prev->lru_next = NULL;
                cache->lru_tail = entry->lru_prev;
                entry->lru_prev = NULL;
                entry->lru_next = cache->lru_head;
                cache->lru_head->lru_prev = entry;
                cache->lru_head = entry;
            }

        } else {
            // insert a new entry
            cache->num_inserts += 1;
            entry = &(cache->cache_entry[cache->entry_count]);
            cache->entry_count += 1;

            // update LRU info
            if ((cache->lru_head == NULL) && (cache->lru_tail == NULL)) {
                entry->lru_prev = NULL;
                entry->lru_next = NULL;
                cache->lru_head = entry;
                cache->lru_tail = entry;
            } else {
                entry->lru_prev = NULL;
                entry->lru_next = cache->lru_head;
                cache->lru_head->lru_prev = entry;
                cache->lru_head = entry;
            }

            // update entry information
            entry->block_num = req->block_num;
            entry->server_num = req->server_num;
            entry->volume_num = req->volume_num;
            entry->counter[curr_sub_window_counter_ind] = 1;
            entry->last_access_sub_window_ind = req->sub_window_ind;

            // update lookup table
            lookup_table_slot = entry->block_num % cache->cache_size;
            if (cache->lookup_table[lookup_table_slot] == NULL) {
                cache->lookup_table[lookup_table_slot] = entry;
                entry->lookup_next = NULL;
                entry->lookup_prev = NULL;
            } else {
                cache->lookup_table[lookup_table_slot]->lookup_prev = entry;
                entry->lookup_next = cache->lookup_table[lookup_table_slot];
                entry->lookup_prev = NULL;
                cache->lookup_table[lookup_table_slot] = entry;
            }
/*            if (lookup_table_slot == 368640) {
                GhostCacheEntry *tmp_entry =
                        cache->lookup_table[lookup_table_slot];
                while (tmp_entry != NULL) {
                    tmp_entry = tmp_entry->lookup_next;
                }
            }*/
        }

    } // misses in ghost cache

    return ret;
}

void ghost_cache_destroy (GhostCache *cache)
{
    if (cache->cache_entry != NULL) {
        free (cache->cache_entry);
    }

    if (cache->lookup_table != NULL) {
        free (cache->lookup_table);
    }
}
