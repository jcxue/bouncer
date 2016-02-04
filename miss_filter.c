#include <stdlib.h>

#include "debug.h"
#include "common.h"
#include "miss_filter.h"

int miss_filter_init(uint64_t size, uint32_t threshold,
        uint8_t num_sub_windows, MissFilter *miss_filter) {
    miss_filter->size             = size;
    miss_filter->threshold        = threshold;
    miss_filter->num_sub_windows  = num_sub_windows;

    miss_filter->entry = (MissFilterEntry *)calloc(size, sizeof(MissFilterEntry));
    check(miss_filter->entry!=NULL, "failed to allocate miss filter entry.");

    return 0;

    error:

    return -1;
}

int miss_filter_lookup(MissFilter *miss_filter, Request *req) {
    int i;
    int hits = 0;
    int ind = 0;
    int miss_count = 0;
    int num_sub_windows = miss_filter->num_sub_windows;
    uint32_t miss_filter_slot = req->block_num % miss_filter->size;
    MissFilterEntry *entry = &(miss_filter->entry[miss_filter_slot]);
    uint32_t last_sub_window_ind = entry->last_access_sub_window_ind;
    uint32_t sub_window_ind_diff = req->sub_window_ind - last_sub_window_ind;
    uint8_t last_sub_window_counter_ind = last_sub_window_ind
            % num_sub_windows;
    uint8_t curr_sub_window_counter_ind = req->sub_window_ind
            % num_sub_windows;

    /* update stale miss count information */
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

    if (entry->counter[curr_sub_window_counter_ind] != MISS_FILTER_COUNTER_MAX) {
        entry->counter[curr_sub_window_counter_ind] += 1;
    }
    entry->last_access_sub_window_ind = req->sub_window_ind;

    if (miss_count > miss_filter->threshold) {
        hits = 1;
    }

/*    if ((req->block_num == 815716) && (req->server_num == 9)
            && (req->volume_num == 0)) {
        if (hits) {
            printf("\t filter hits %d\n", miss_count);
        } else {
            printf("\t filter misses %d\n", miss_count);
        }
    }*/

    return hits;
}

void miss_filter_destroy(MissFilter *miss_filter) {
    if (miss_filter->entry != NULL) {
        free(miss_filter->entry);
    }

    free(miss_filter);
}
