#ifndef MISS_TABLE_H_
#define MISS_TABLE_H_

typedef struct MissTableEntry {
    uint64_t        block_num;
    uint8_t         server_num;
    uint8_t         volume_num;
    uint8_t         counter[12];
    uint32_t        last_access_sub_window_ind;
    struct MissTableEntry *prev;
    struct MissTableEntry *next;
} MissTableEntry __attribute__((aligned (64)));

typedef struct MissTable {
    uint64_t         lookup_table_size;
    uint32_t         threshold;
    uint64_t         entry_count; // threshold for starting pruning
    uint32_t         num_prunes;
    uint8_t          num_sub_windows;
    uint32_t         num_inserts;
    MissTableEntry **lookup_table;
} MissTable;


int  miss_table_init    (uint64_t size, uint32_t threshold,
        uint8_t num_sub_windwos, MissTable *miss_table);
int  miss_table_access  (MissTable *miss_table, Request *req);
void miss_table_destroy (MissTable *miss_table);

#endif
