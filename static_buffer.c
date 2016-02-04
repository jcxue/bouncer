#include <stdlib.h>

#include "debug.h"
#include "static_buffer.h"


int static_buffer_init (StaticBuffer *sb, uint64_t size)
{
    sb->size = size;
    sb->entry_count = 0;

    sb->entry = (StaticBufferEntry *) calloc (sb->size,
            sizeof(StaticBufferEntry));
    check (sb->entry!=NULL, "failed to allocate sb->entry.");

    sb->lookup_table = (StaticBufferEntry **) calloc (sb->size,
            sizeof (StaticBufferEntry *));
    check (sb->lookup_table!=NULL, "failed to allocate sb->lookup_table.");

    return 0;

error:

    return -1;
}

int static_buffer_lookup   (StaticBuffer *sb, ReplayReq *req)
{
    int exists = 0;
    uint64_t table_ind = req->block_num % sb->size;
    StaticBufferEntry *entry = sb->lookup_table[table_ind];

    while (entry != NULL) {
        if ((entry->block_num == req->block_num) &&
            (entry->server_num == req->server_num) &&
            (entry->volume_num == req->volume_num)) {
            exists = 1;
            break;
        } else {
            entry = entry->lookup_next;
        }
    }

    if (exists) {
        if (req->req_type == 0) {
            sb->read_hits += 1;
        } else {
            sb->write_hits += 1;
        }
    }

    return exists;
}

int static_buffer_insert (StaticBuffer *sb, uint64_t block_num,
        uint8_t server_num, uint8_t volume_num)
{
    uint64_t table_ind = block_num % sb->size;
    StaticBufferEntry *entry = NULL;

    // insert into sb
    entry = &(sb->entry[sb->entry_count]);
    entry->block_num = block_num;
    entry->server_num = server_num;
    entry->volume_num = volume_num;

    // insert into the front of the lookup table
    entry->lookup_next = sb->lookup_table[table_ind];
    sb->lookup_table[table_ind] = entry;

    sb->entry_count += 1;
    check(sb->entry_count <= sb->size, "sb overflows.");

    return 0;

 error:

     return -1;
}

void static_buffer_destroy (StaticBuffer *sb)
{
    if (sb->entry != NULL) {
        free (sb->entry);
    }

    if (sb->lookup_table != NULL) {
        free (sb->lookup_table);
    }

    if (sb != NULL) {
        free (sb);
    }
}
