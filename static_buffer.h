#ifndef StaticBuffer_H_
#define StaticBuffer_H_

#include <inttypes.h>
#include "common.h"

typedef struct StaticBufferEntry {
    uint64_t block_num;
    uint8_t  server_num;
    uint8_t  volume_num;
    struct StaticBufferEntry *lookup_next;
} StaticBufferEntry __attribute__((aligned (64)));

typedef struct StaticBuffer {
    uint64_t               size;
    uint64_t               entry_count;
    uint64_t               read_hits;
    uint64_t               write_hits;
    StaticBufferEntry   *entry;
    StaticBufferEntry  **lookup_table;
} StaticBuffer;

int  static_buffer_init     (StaticBuffer *sb, uint64_t size);
int  static_buffer_lookup   (StaticBuffer *sb, ReplayReq *req);
int  static_buffer_insert   (StaticBuffer *sb, uint64_t block_num,
                                uint8_t server_num, uint8_t volume_num);
void static_buffer_destroy  (StaticBuffer *sb);

#endif
