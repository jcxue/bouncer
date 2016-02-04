#ifndef BOUNCER_BUFFER_H_
#define BOUNCER_BUFFER_H_

#include "common.h"

typedef struct BouncerBufferEntry {
    uint64_t                  block_num;
    uint8_t                   server_num;
    uint8_t                   volume_num;
    struct BouncerBufferEntry     *lookup_next;
    struct BouncerBufferEntry     *lookup_prev;
    struct BouncerBufferEntry     *lru_next;
    struct BouncerBufferEntry     *lru_prev;
} BouncerBufferEntry __attribute__((aligned (64)));


typedef struct BouncerBuffer {
    uint64_t        buffer_size;         // The total number of entries
    uint64_t        entry_count;        // The number of entries currently being occupied
    uint64_t        lookup_table_size;  // The size of hash lookup table
    BouncerBufferEntry  *buffer_entry;
    BouncerBufferEntry  **lookup_table;
    BouncerBufferEntry  *lru_head;
    BouncerBufferEntry  *lru_tail;
}BouncerBuffer __attribute__((aligned (64)));

int  bouncer_buffer_init    (uint32_t buffer_size, BouncerBuffer *buffer);
int  bouncer_buffer_lookup  (BouncerBuffer *buffer, Request *req);
int  bouncer_buffer_insert  (BouncerBuffer *buffer, uint64_t block_num,
                      uint8_t server_num, uint8_t volume_num, Request *ret_req);
void bouncer_buffer_destroy (BouncerBuffer *buffer);
void bouncer_buffer_test    ();

#endif
