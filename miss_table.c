#include <stdio.h>
#include <stdlib.h>

#include "common.h"
#include "debug.h"
#include "miss_table.h"


int miss_table_init (uint64_t size, uint32_t threshold,
        uint8_t num_sub_windows, MissTable *miss_table)
{
    miss_table->lookup_table_size = size;
    miss_table->threshold         = threshold;
    miss_table->entry_count       = 0;
    miss_table->num_prunes        = 0;
    miss_table->num_sub_windows   = num_sub_windows;
    miss_table->lookup_table = (MissTableEntry **) calloc (size,
            sizeof(MissTableEntry *));
    check(miss_table->lookup_table!=NULL,
            "failed to allocate miss_table->lookup_table.");

    return 0;

error:

    return -1;
}

/* Return hits if the entry existed in the miss_table
 * Otherwise, return miss, insert the entry into the
 * miss_table.
 *
 * When the number of entries is larger than the threshold,
 * Start pruning the miss_table.
 *
 */
int miss_table_access (MissTable *miss_table, Request *req)
{
    int hits       = 0;
    int miss_count = 0;
    int ind        = 0;
    uint32_t i     = 0;
    uint32_t lookup_table_slot = req->block_num % miss_table->lookup_table_size;
    MissTableEntry *entry = miss_table->lookup_table[lookup_table_slot];
    uint32_t last_sub_window_ind;
    uint32_t sub_window_ind_diff;
    uint8_t  num_sub_windows = miss_table->num_sub_windows;

    /* try to find a match */
    while (entry != NULL) {
        if ((entry->block_num == req->block_num) &&
            (entry->volume_num == req->volume_num) &&
            (entry->server_num == req->server_num)) {
            break;
        }
        entry = entry->next;
    }

    if (entry != NULL) {
        /* find a match */
        last_sub_window_ind = entry->last_access_sub_window_ind;
        sub_window_ind_diff = req->sub_window_ind - last_sub_window_ind;
        uint8_t last_sub_window_counter_ind = last_sub_window_ind
                % num_sub_windows;
        uint8_t curr_sub_window_counter_ind = req->sub_window_ind
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

        if (miss_count >= miss_table->threshold) {
            hits = 1;
        }

        if (entry->counter[curr_sub_window_counter_ind] != MISS_FILTER_COUNTER_MAX) {
            entry->counter[curr_sub_window_counter_ind] += 1;
        }
        entry->last_access_sub_window_ind = req->sub_window_ind;

/*        if ((req->block_num == 815716) && (req->server_num == 9)
                && (req->volume_num == 0)) {
            if (hits) {
                printf("\t hits %d\n", miss_count);
            } else {
                printf("\t misses %d\n", miss_count);
            }
        }*/

    } else {
        /* no match found */
        miss_table->num_inserts += 1;
        MissTableEntry *new_entry = (MissTableEntry *) calloc (1,
                sizeof(MissTableEntry));
        check(new_entry!=NULL,
                "failed to allocate MissTableEntry for insertion.");

        new_entry->block_num  = req->block_num;
        new_entry->server_num = req->server_num;
        new_entry->volume_num = req->volume_num;
        new_entry->last_access_sub_window_ind = req->sub_window_ind;
        uint8_t curr_sub_window_counter_ind = req->sub_window_ind
                        % num_sub_windows;
        new_entry->counter[curr_sub_window_counter_ind] = 1;

/*        if ((req->block_num == 815716) && (req->server_num == 9)
                && (req->volume_num == 0)) {
            printf ("\t inserts into miss table.\n");
        }*/

        /* insert the entry to the beginning of the chain */
        entry = miss_table->lookup_table[lookup_table_slot];
        if (entry != NULL) {
            entry->prev = new_entry;
            new_entry->next = entry;
            new_entry->prev = NULL;
        }
        miss_table->lookup_table[lookup_table_slot] = new_entry;

        miss_table->entry_count += 1;
        /* Prune the miss table */
        MissTableEntry *curr_entry = NULL;
        MissTableEntry *next_entry = NULL;
        if (miss_table->entry_count == (miss_table->lookup_table_size << 3)) {
            for (i = 0; i < miss_table->lookup_table_size; i++) {
                curr_entry = miss_table->lookup_table[i];
                while (curr_entry != NULL) {
                    next_entry = curr_entry->next;
                    last_sub_window_ind = curr_entry->last_access_sub_window_ind;
                    sub_window_ind_diff = req->sub_window_ind - last_sub_window_ind;
                    if (sub_window_ind_diff >= num_sub_windows) {
                        if (curr_entry->prev != NULL) {
                            curr_entry->prev->next = next_entry;
                        }
                        if (next_entry != NULL) {
                            next_entry->prev = curr_entry->prev;
                        }
                        if (curr_entry == miss_table->lookup_table[i]) {
                            miss_table->lookup_table[i] = next_entry;
                        }
                        free(curr_entry);
                        miss_table->entry_count -= 1;
                    }
                    curr_entry = next_entry;
                }
            } // end of loop through miss_table->lookup_table
            miss_table->num_prunes += 1;
        } // end of pruning
    } // miss in the miss_table

    return hits;

error:

    return -1;
}

void miss_table_destroy (MissTable *miss_table)
{
    uint32_t i;
    MissTableEntry *curr_entry = NULL;
    MissTableEntry *next_entry = NULL;

    if (miss_table->lookup_table != NULL) {
        for (i = 0; i < miss_table->lookup_table_size; i++) {
            curr_entry = miss_table->lookup_table[i];
            while (curr_entry != NULL) {
                next_entry = curr_entry->next;
                free (curr_entry);
                curr_entry = next_entry;
            }
        }
        free (miss_table->lookup_table);
    }

    free (miss_table);
}
