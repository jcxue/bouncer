#ifndef MISS_FILTER_H_
#define MISS_FILTER_H_

#include <inttypes.h>

typedef struct MissFilterEntry {
    uint8_t   counter[12];
    uint32_t  last_access_sub_window_ind;
} MissFilterEntry __attribute__((aligned(16)));

typedef struct MissFilter {
    uint64_t         size;
    uint32_t         threshold;
    uint8_t          num_sub_windows;
    MissFilterEntry *entry;
} MissFilter;

int  miss_filter_init    (uint64_t size, uint32_t threshold,
        uint8_t num_sub_windows, MissFilter *miss_filter);
int  miss_filter_lookup  (MissFilter *miss_filter, Request *req);
void miss_filter_destroy (MissFilter *miss_filter);

#endif
