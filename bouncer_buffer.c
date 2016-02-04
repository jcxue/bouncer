#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "debug.h"
#include "common.h"
#include "bouncer_buffer.h"

/*
 * Assuming the lookup_table_size is multiples of 512
 * so that the lookup_table would occupy multiples of
 * 4K pages.
 *
 * lookup_table_size is 8 times smaller than the
 * buffer_size so that the open chaining would not
 * be longer than 8.
 */
uint32_t get_bouncer_lookup_table_size (uint32_t buffer_size)
{
    uint32_t lookup_table_size = 512;
    uint32_t multiple          = (buffer_size >> 12) + 1;

    return (lookup_table_size * multiple);
}

int bouncer_buffer_init (uint32_t buffer_size, BouncerBuffer *buffer)
{
    buffer->buffer_size        = buffer_size;
    check (buffer_size>=2, "the Bouncer buffer size should at least be 2.");

    buffer->entry_count       = 0;
    if (buffer_size < 64) {
        buffer->lookup_table_size = buffer_size;
    } else {
        buffer->lookup_table_size = get_bouncer_lookup_table_size (buffer_size);
    }

    /* allocate space for buffer_entry */
    buffer->buffer_entry = (BouncerBufferEntry *) calloc (buffer->buffer_size,
            sizeof(BouncerBufferEntry));
    check(buffer->buffer_entry!=NULL,
            "failed to allocate space for bouncer_buffer->buffer_entry.");

    /* allocate space for lookup_table */
    buffer->lookup_table = (BouncerBufferEntry **) calloc (
            buffer->lookup_table_size, sizeof(BouncerBufferEntry *));
    check(buffer->lookup_table!=NULL,
            "failed to allocate bouncer_buffer->lookup table.");

    buffer->lru_head = NULL;
    buffer->lru_tail = NULL;

    return 0;

error:

    return -1;
}

/* check if the request could hit in the buffer.
 *
 * returns 0: buffer miss
 *         1: buffer hit plus updating LRU info
 *
 */
int bouncer_buffer_lookup (BouncerBuffer *buffer, Request *req)
{
    int exists = 0;
    int table_slot = req->block_num % buffer->lookup_table_size;
    BouncerBufferEntry  *buffer_entry  = buffer->lookup_table[table_slot];

    while (buffer_entry != NULL) {
        if ((buffer_entry->block_num == req->block_num)   &&
            (buffer_entry->server_num == req->server_num) &&
            (buffer_entry->volume_num == req->volume_num)) {
            exists = 1;
            break;
        } else {
            buffer_entry = buffer_entry->lookup_next;
        }
    }

    if (exists)
    {
        /* update LRU information */
        if ((buffer_entry->lru_prev == NULL) && (buffer_entry->lru_next == NULL)) {
            /* the only entry in the buffer, do nothing. */
        } else {
            if (buffer_entry == buffer->lru_tail) {
                buffer->lru_tail = buffer_entry->lru_prev;
                buffer_entry->lru_prev->lru_next = NULL;
                buffer->lru_head->lru_prev = buffer_entry;
                buffer_entry->lru_next = buffer->lru_head;
                buffer_entry->lru_prev = NULL;
                buffer->lru_head = buffer_entry;
            } else {
                if (buffer_entry == buffer->lru_head) {
                    /* do nothing */
                } else {
                    buffer_entry->lru_prev->lru_next = buffer_entry->lru_next;
                    buffer_entry->lru_next->lru_prev = buffer_entry->lru_prev;
                    buffer_entry->lru_prev = NULL;
                    buffer_entry->lru_next = buffer->lru_head;
                    buffer->lru_head->lru_prev = buffer_entry;
                    buffer->lru_head = buffer_entry;
                }
            }
        }

        /* move the entry to the front of lookup chain */
        if ((buffer_entry->lookup_prev == NULL) && (buffer_entry->lookup_next == NULL)) {
            /* the only entry in the chain, do nothing */
        } else {
            if (buffer->lookup_table[table_slot] == buffer_entry) {
                /* it is the first entry already. do nothing. */
            } else {
                buffer_entry->lookup_prev->lookup_next = buffer_entry->lookup_next;
                if (buffer_entry->lookup_next != NULL) {
                    buffer_entry->lookup_next->lookup_prev =
                            buffer_entry->lookup_prev;
                }

                buffer_entry->lookup_prev = NULL;
                buffer_entry->lookup_next = buffer->lookup_table[table_slot];
                buffer->lookup_table[table_slot]->lookup_prev = buffer_entry;
                buffer->lookup_table[table_slot] = buffer_entry;
            } // else (the first entry in the chain)
        } // if (the only entry in the chain )

    } // if (exists)

    return exists;
}

/*
 * return 0 if none entry is replaced
 * return 1 if entry is replaced
 */

int bouncer_buffer_insert  (BouncerBuffer *buffer, uint64_t block_num,
                uint8_t server_num, uint8_t volume_num, Request *ret_req)
{
    BouncerBufferEntry *buffer_entry = NULL;
    int            table_slot  = -1;
    int            ret         = 0;
    if (buffer->entry_count == buffer->buffer_size) {
        /* replacing an existing entry */
        ret          = 1;
        buffer_entry = buffer->lru_tail;
        ret_req->block_num  = buffer_entry->block_num;
        ret_req->server_num = buffer_entry->server_num;
        ret_req->volume_num = buffer_entry->volume_num;

        /* updating LRU info */
        buffer->lru_tail = buffer_entry->lru_prev;
        buffer_entry->lru_prev->lru_next = NULL;

        buffer_entry->lru_prev = NULL;
        buffer_entry->lru_next = buffer->lru_head;
        buffer->lru_head->lru_prev = buffer_entry;
        buffer->lru_head = buffer_entry;

        /* update lookup info */
        /* remove it from the original lookup chain */

        table_slot = buffer_entry->block_num % buffer->lookup_table_size;

        if (buffer->lookup_table[table_slot] == buffer_entry) {
            /*if it is the first one in the chain */
            if (buffer_entry->lookup_next == NULL) {
                /* if it is the only one in the chain */
                buffer->lookup_table[table_slot] = NULL;
            } else {
                /* if it is first one but not the only one */
                buffer->lookup_table[table_slot] = buffer_entry->lookup_next;
                buffer_entry->lookup_next->lookup_prev = NULL;
            }
        } else {
            /* if it is not the first one in the chain */
            if (buffer_entry->lookup_next == NULL) {
                /* if it is the last one in the chain */
                buffer_entry->lookup_prev->lookup_next = NULL;
            } else {
                /* if it is neither the first one nor the last one */
                buffer_entry->lookup_prev->lookup_next = buffer_entry->lookup_next;
                buffer_entry->lookup_next->lookup_prev = buffer_entry->lookup_prev;
            }
        }

        /* insert it to the head of the new lookup chain */
        table_slot = block_num % buffer->lookup_table_size;
        buffer_entry->lookup_prev = NULL;
        buffer_entry->lookup_next = buffer->lookup_table[table_slot];
        if (buffer->lookup_table[table_slot] != NULL) {
            buffer->lookup_table[table_slot]->lookup_prev = buffer_entry;
        }
        buffer->lookup_table[table_slot] = buffer_entry;
        buffer_entry->block_num  = block_num;
        buffer_entry->server_num = server_num;
        buffer_entry->volume_num = volume_num;
    } else {
        /* inserting a new entry */
        buffer_entry = &(buffer->buffer_entry[buffer->entry_count]);
        buffer_entry->block_num  = block_num;
        buffer_entry->server_num = server_num;
        buffer_entry->volume_num = volume_num;

        /* update LRU info */
        if (buffer->lru_head == NULL) {
            /* very first entry */
            buffer->lru_head = buffer_entry;
            buffer->lru_tail = buffer_entry;
            buffer_entry->lru_prev = NULL;
            buffer_entry->lru_next = NULL;
        } else {
            buffer_entry->lru_prev = NULL;
            buffer_entry->lru_next = buffer->lru_head;
            buffer->lru_head->lru_prev = buffer_entry;
            buffer->lru_head = buffer_entry;
        }

        /* update lookup info */
        table_slot = block_num % buffer->lookup_table_size;

        buffer_entry->lookup_prev = NULL;
        buffer_entry->lookup_next = buffer->lookup_table[table_slot];

        if (buffer->lookup_table[table_slot] != NULL) {
            buffer->lookup_table[table_slot]->lookup_prev = buffer_entry;
        }
        buffer->lookup_table[table_slot] = buffer_entry;

        buffer->entry_count += 1;
    }

    return ret;
}

void bouncer_buffer_destroy (BouncerBuffer *buffer)
{
    if (buffer->buffer_entry != NULL) {
        free (buffer->buffer_entry);
    }

    if (buffer->lookup_table != NULL) {
        free (buffer->lookup_table);
    }

    free (buffer);
}

void print_bouncer_buffer (BouncerBuffer *buffer)
{
    int i;
    BouncerBufferEntry *buffer_entry = NULL;

    printf("++++++++++++++++++++++++++++++++++++\n");
    for (i = 0; i < buffer->lookup_table_size; i++) {
        printf("SLOT %d: ", i);
        buffer_entry = buffer->lookup_table[i];
        while (buffer_entry != NULL) {
            printf("%"PRIu64"\t", buffer_entry->block_num);
            buffer_entry = buffer_entry->lookup_next;
        }
        printf("\n");
    }

    printf("LRU:\t");
    buffer_entry = buffer->lru_head;
    while (buffer_entry != NULL) {
        printf("%"PRIu64"\t", buffer_entry->block_num);
        buffer_entry = buffer_entry->lru_next;
    }
    printf("\n++++++++++++++++++++++++++++++++++++\n\n");

}

void bouncer_buffer_test ()
{
    int ret = 0;
    int i;
    int block_array[20] = { 1, 1, 2, 1, 2, 3, 1, 2, 3, 4, 1, 2, 3, 4, 5, 1, 2,
            3, 4, 5 };
    Request req, replaced_req;

    BouncerBuffer *bouncer_buffer = (BouncerBuffer *) calloc(1, sizeof(BouncerBuffer));

    ret = bouncer_buffer_init(16, bouncer_buffer);

    for (i = 0; i < 20; i++) {
        req.block_num = block_array[i];
        req.server_num = 0;
        req.volume_num = 0;

        ret = bouncer_buffer_lookup(bouncer_buffer, &req);
        if (ret == 0) {
            bouncer_buffer_insert(bouncer_buffer, req.block_num, req.server_num,
                    req.volume_num, &replaced_req);
        }

        print_bouncer_buffer(bouncer_buffer);
    }

    bouncer_buffer_destroy(bouncer_buffer);
}

