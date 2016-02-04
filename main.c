#define _GNU_SOURCE 1

#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <inttypes.h>
#include <sys/time.h>

#include "debug.h"
#include "common.h"
#include "config_parser.h"
#include "lru_cache.h"
#include "miss_filter.h"
#include "miss_table.h"
#include "static_buffer.h"
#include "ghost_cache.h"

#define LCHILD(x) ((x<<1) + 1)
#define RCHILD(x) ((x<<1) + 2)

/* Global variables */
uint64_t starting_time_stamp; /* the first request timestamp in 100ns */
uint64_t time_window_size; /* time window size in 100ns */

static void usage(const char *argv0) {
    printf("Usage: \n");
    printf("\t base line sieve store   : %s 0\n", argv0);
    printf("\t traditional write buffer: %s 1 tr_wb_size\n", argv0);
    printf("\t sieved write buffer     : %s 2 s_wb_threshold s_wb_size\n", argv0);
    printf("\t sieved + traditional write buffer: %s 3 "
            "s_wb_threshold s_wb_size tr_wb_size\n",
            argv0);
}

int run_sieved_write_buffer(ConfigInfo *config_info) {
    int ret;
    uint64_t i;
    FILE *trace_fp = NULL;
    FILE *out_fp = NULL;
    char line[FILE_LINE_SIZE];
    CSVLineData csv_data;

    Request req, replaced_req;
    int num_blocks;
    int progress = 10;

    LRUCache   *write_buffer = NULL;
    MissFilter *miss_filter = NULL;
    MissTable  *miss_table = NULL;
    GhostCache *wb_ghost_cache = NULL;
    LRUCache   *ssd_cache = NULL;

    struct timeval start, end;
    uint64_t tot_reads = 0, tot_writes = 0, tot_reqs = 0, tot_lines = 1;

    /* initializations */

    out_fp = fopen (config_info->result_file, "w");
    check (out_fp!=NULL, "failed to open result file: %s.",
                config_info->result_file);

    write_buffer = (LRUCache *) calloc (1, sizeof(LRUCache));
    check (write_buffer!=NULL, "failed to allocate write_buffer.");
    ret = lru_cache_init (config_info->sieved_wb_size, write_buffer);
    check (ret != -1, "failed to initialize write_buffer.");

    miss_filter = (MissFilter *) calloc(1, sizeof(MissFilter));
    check(miss_filter!=NULL, "failed to allocate miss_filter.");
    ret = miss_filter_init(config_info->miss_filter_size,
            config_info->miss_filter_threshold, config_info->num_sub_windows,
            miss_filter);
    check(ret!=-1, "failed to initialize miss_filter.");

    miss_table = (MissTable *) calloc(1, sizeof(MissTable));
    check(miss_table!=NULL, "failed to allocate miss_table.");
    ret = miss_table_init(config_info->miss_table_lookup_size,
            config_info->miss_table_threshold, config_info->num_sub_windows,
            miss_table);
    check(ret!=-1, "failed to initialize miss_table.");

    wb_ghost_cache = (GhostCache *) calloc(1, sizeof(GhostCache));
    check(wb_ghost_cache!=NULL, "failed to allocate wb_ghost_cache.");
    ret = ghost_cache_init(config_info->sieved_ghost_cache_size,
            config_info->sieved_wb_threshold, config_info->num_sub_windows,
            wb_ghost_cache);
    check(ret!=-1, "failed to initialize wb_ghost_cache.");

    ssd_cache = (LRUCache *) calloc(1, sizeof(LRUCache));
    check(ssd_cache!=NULL, "failed to allocate ssd_cache.");
    ret = lru_cache_init(config_info->ssd_size, ssd_cache);
    check(ret!=-1, "failed to initialize ssd_cache.");

    /* start simulation */
    gettimeofday(&start, NULL);

    /* go over trace file */
    trace_fp = fopen(config_info->trace_file, "r");
    check(trace_fp!=NULL, "failed to open trace file:%s\n",
            config_info->trace_file);

    while (fgets(line, sizeof(line), trace_fp) != NULL) {
        ret = sscanf(line, "%"SCNu64" %"SCNu8" %"SCNu8" %s %"SCNu64" %"SCNu32" "
        "%"SCNu64"\n", &csv_data.timestamp, &csv_data.server_num,
                &csv_data.volume_num, csv_data.req_type, &csv_data.start_addr,
                &csv_data.req_size, &csv_data.duration);
        check(ret==7, "failed to parse csv line:\n\t%s\n", line);
        tot_lines += 1;

        /* first line of trace file, extract starting timestamp */
        if (tot_reqs == 0) {
            starting_time_stamp = csv_data.timestamp;
        }

        req.block_num = (csv_data.start_addr >> LOG_2_BLOCK_SIZE);
        num_blocks = (csv_data.req_size >> LOG_2_BLOCK_SIZE);
        if (num_blocks == 0) {
            num_blocks = 1;
        }

        if (strcmp(csv_data.req_type, "Write") == 0) {
            req.req_type = 1;
        } else {
            req.req_type = 0;
        }

        req.server_num = csv_data.server_num;
        req.volume_num = csv_data.volume_num;
        req.sub_window_ind = (csv_data.timestamp - starting_time_stamp)
                            / SUB_WINDOW_SIZE;

        for (i = 0; i < num_blocks; i++) {

            tot_reqs += 1;

            if (tot_reqs % 292000000 == 0) {
                printf("%d%% is done.\n", progress);
                progress += 10;
            }

            if (req.req_type == 0) {
                // read request
                tot_reads += 1;
                ret = lru_cache_read_lookup(write_buffer, &req);
                if (ret == 0) {
                    ret = lru_cache_lookup(ssd_cache, &req);
                }
                if (ret == 0) {
                    ret = miss_filter_lookup(miss_filter, &req);
                    if (ret == 1) {
                        /*hits in miss_filter, check miss_table*/
                        ret = miss_table_access(miss_table, &req);
                        /*hits in miss_table, insert it to ssd_cache*/
                        if (ret == 1) {
                            lru_cache_insert(ssd_cache, req.block_num,
                                    req.server_num, req.volume_num,
                                    &replaced_req);
                        } // hits in miss_table
                    } // hits in miss_filter
                }
            } else {
                // write request
                tot_writes += 1;
                ret = lru_cache_lookup(write_buffer, &req);
                if (ret == 0) {
                    ret = lru_cache_peek(ssd_cache, &req);
                    if (ret == 0) {
                        // miss in ssd_cache
                        ret = miss_filter_lookup(miss_filter, &req);
                        if (ret == 1) {
                            /*hits in miss_filter, check miss_table*/
                            ret = miss_table_access(miss_table, &req);
                            /*hits in miss_table, insert it to ssd_cache*/
                            if (ret == 1) {
                                ret = ghost_cache_access (wb_ghost_cache, &req);
                                if (ret == 0) {
                                    // allocate to ssd
                                    lru_cache_insert(ssd_cache, req.block_num,
                                            req.server_num, req.volume_num,
                                            &replaced_req);
                                } else {
                                    // allocate to wb
                                    ret = lru_cache_insert(write_buffer,
                                            req.block_num, req.server_num,
                                            req.volume_num, &replaced_req);
                                    if (ret == 1) {
                                        ret = lru_cache_peek(ssd_cache,
                                                &replaced_req);
                                        if (ret == 1) {
                                            ret = lru_cache_update(ssd_cache,
                                                    &replaced_req);
                                            check(ret==0,
                                                    "failed to update entry in ssd_cache.");
                                        } else {
                                            lru_cache_insert(ssd_cache,
                                                    replaced_req.block_num,
                                                    replaced_req.server_num,
                                                    replaced_req.volume_num,
                                                    &req);
                                        }
                                    } // write buffer has a replaced entry
                                } // allocate to wb
                            } // hits in miss_table
                        } // hits in miss_filter
                    } else {
                        // hits in ssd_cache
                        ret = ghost_cache_access (wb_ghost_cache, &req);
                        if (ret == 1) {
                            ret = lru_cache_remove(ssd_cache, &req);
                            check(ret==0,
                                    "failed to remove entry from ssd_cache.");
                            ret = lru_cache_insert(write_buffer, req.block_num,
                                    req.server_num, req.volume_num,
                                    &replaced_req);
                            if (ret == 1) {
                                ret = lru_cache_peek(ssd_cache, &replaced_req);
                                if (ret == 1) {
                                    ret = lru_cache_update(ssd_cache,
                                            &replaced_req);
                                    check(ret==0,
                                            "failed to update entry in ssd_cache.");
                                } else {
                                    lru_cache_insert(ssd_cache,
                                            replaced_req.block_num,
                                            replaced_req.server_num,
                                            replaced_req.volume_num, &req);
                                }
                            } // write buffer has a replaced entry
                        } else {
                            // cannot make into the write buffer
                            // write to ssd instead.
                            ret = lru_cache_lookup(ssd_cache, &req);
                        }
                    }
                }
            } // write request
            req.block_num += 1;
        } // loop through each request
    } // loop through each line from the trace file

    gettimeofday(&end, NULL);

    fprintf(out_fp, "total number of requests: %"PRIu64"\n", tot_reqs);
    fprintf(out_fp, "total number of reads:    %"PRIu64"\n", tot_reads);
    fprintf(out_fp, "total number of writes:   %"PRIu64"\n", tot_writes);
    long duration = (end.tv_sec - start.tv_sec) * 1000000
                + (end.tv_usec - start.tv_usec);
    fprintf(out_fp, "time consumed = %.4f (min)\n",
                (double) duration / (1000000.0 * 60.0));

    fprintf(out_fp, "\n");
    fprintf(out_fp, "sieved write buffer: \n");
    fprintf(out_fp, "sieved_wb_size  :  %"PRIu64"\n", write_buffer->cache_size);
    fprintf(out_fp, "read hits       :  %"PRIu64"\n", write_buffer->read_hits);
    fprintf(out_fp, "read lookups    :  %"PRIu64"\n", write_buffer->read_lookups);
    fprintf(out_fp, "read hits ratio :  %.4f\n",
          (double) write_buffer->read_hits/(double) write_buffer->read_lookups);
    fprintf(out_fp, "write hits      :  %"PRIu64"\n", write_buffer->write_hits);
    fprintf(out_fp, "write lookups   :  %"PRIu64"\n", write_buffer->write_lookups);
    fprintf(out_fp, "write hits ratio:  %.4f\n",
          (double) write_buffer->write_hits/(double) write_buffer->write_lookups);
    fprintf(out_fp, "num inserts     :  %"PRIu64"\n", write_buffer->num_writes);
    fprintf(out_fp, "num updates     :  %"PRIu64"\n", write_buffer->num_updates);
    fprintf(out_fp, "num replaces    :  %"PRIu64"\n", write_buffer->num_replaces);

    fprintf(out_fp, "\n");
    fprintf(out_fp, "ssd cache: \n");

    fprintf(out_fp, "ssd cache size: %"PRIu64":\n", ssd_cache->cache_size);
    fprintf(out_fp, "ssd cache miss filter threshold: %"PRIu32"\n",
            miss_filter->threshold);
    fprintf(out_fp, "ssd cache miss table threshold: %"PRIu32"\n",
            miss_table->threshold);
    fprintf(out_fp, "num sub windows: %"PRIu8"\n", miss_filter->num_sub_windows);
    fprintf(out_fp, "read hits       :  %"PRIu64"\n", ssd_cache->read_hits);
    fprintf(out_fp, "read lookups    :  %"PRIu64"\n", ssd_cache->read_lookups);
    fprintf(out_fp, "read hits ratio :  %.4f\n",
          (double) ssd_cache->read_hits/(double) ssd_cache->read_lookups);
    fprintf(out_fp, "write hits      :  %"PRIu64"\n", ssd_cache->write_hits);
    fprintf(out_fp, "write lookups   :  %"PRIu64"\n", ssd_cache->write_lookups);
    fprintf(out_fp, "write hits ratio:  %.4f\n",
          (double) ssd_cache->write_hits/(double) ssd_cache->write_lookups);
    fprintf(out_fp, "num inserts     :  %"PRIu64"\n", ssd_cache->num_writes);
    fprintf(out_fp, "num removes     :  %"PRIu64"\n", ssd_cache->num_removes);
    fprintf(out_fp, "num updates     :  %"PRIu64"\n", ssd_cache->num_updates);
    fprintf(out_fp, "num replaces    :  %"PRIu64"\n", ssd_cache->num_replaces);

    fprintf(out_fp, "\n");
    fprintf(out_fp, "ghost cache: \n");

    fprintf (out_fp, "ghost cache size: %"PRIu64"\n", wb_ghost_cache->cache_size);
    fprintf (out_fp, "ghost cache threshold: %"PRIu32"\n", wb_ghost_cache->threshold);
    fprintf (out_fp, "ghost cache num_sub_windows: %"PRIu8"\n",
            wb_ghost_cache->num_sub_windows);
    fprintf (out_fp, "num_inserts    :  %"PRIu32"\n", wb_ghost_cache->num_inserts);
    fprintf (out_fp, "num_replaces    : %"PRIu32"\n", wb_ghost_cache->num_replaces);

    /* destroy resource */
    fclose(trace_fp);
    fclose(out_fp);
    lru_cache_destroy (write_buffer);
    miss_filter_destroy(miss_filter);
    miss_table_destroy(miss_table);
    ghost_cache_destroy(wb_ghost_cache);
    lru_cache_destroy(ssd_cache);

    return 0;

error:
    if (trace_fp != NULL) {
        fclose(trace_fp);
    }

    if (out_fp != NULL) {
        fclose(out_fp);
    }

    if (write_buffer != NULL) {
        lru_cache_destroy (write_buffer);
    }

    if (miss_filter != NULL) {
        miss_filter_destroy(miss_filter);
    }

    if (miss_table != NULL) {
        miss_table_destroy(miss_table);
    }

    if (wb_ghost_cache != NULL) {
        ghost_cache_destroy(wb_ghost_cache);
    }

    if (ssd_cache != NULL) {
        lru_cache_destroy(ssd_cache);
    }

    return -1;
}

int run_traditional_write_buffer(ConfigInfo *config_info) {
    int ret;
    uint64_t i;
    FILE *trace_fp = NULL;
    FILE *out_fp = NULL;
    char line[FILE_LINE_SIZE];
    CSVLineData csv_data;

    Request req, replaced_req;
    int num_blocks;
    int progress = 10;

    LRUCache   *write_buffer = NULL;
    MissFilter *miss_filter = NULL;
    MissTable  *miss_table = NULL;
    LRUCache   *ssd_cache = NULL;

    struct timeval start, end;
    uint64_t tot_reads = 0, tot_writes = 0, tot_reqs = 0, tot_lines = 1;

    /* initializations */

    out_fp = fopen (config_info->result_file, "w");
    check (out_fp!=NULL, "failed to open result file: %s.",
                config_info->result_file);

    write_buffer = (LRUCache *) calloc (1, sizeof(LRUCache));
    check (write_buffer!=NULL, "failed to allocate write_buffer.");
    ret = lru_cache_init (config_info->traditional_wb_size, write_buffer);
    check (ret != -1, "failed to initialize write_buffer.");

    miss_filter = (MissFilter *) calloc(1, sizeof(MissFilter));
    check(miss_filter!=NULL, "failed to allocate miss_filter.");
    ret = miss_filter_init(config_info->miss_filter_size,
            config_info->miss_filter_threshold, config_info->num_sub_windows,
            miss_filter);
    check(ret!=-1, "failed to initialize miss_filter.");

    miss_table = (MissTable *) calloc(1, sizeof(MissTable));
    check(miss_table!=NULL, "failed to allocate miss_table.");
    ret = miss_table_init(config_info->miss_table_lookup_size,
            config_info->miss_table_threshold, config_info->num_sub_windows,
            miss_table);
    check(ret!=-1, "failed to initialize miss_table.");

    ssd_cache = (LRUCache *) calloc(1, sizeof(LRUCache));
    check(ssd_cache!=NULL, "failed to allocate ssd_cache.");
    ret = lru_cache_init(config_info->ssd_size, ssd_cache);
    check(ret!=-1, "failed to initialize ssd_cache.");

    /* start simulation */
    gettimeofday(&start, NULL);

    /* go over trace file */
    trace_fp = fopen(config_info->trace_file, "r");
    check(trace_fp!=NULL, "failed to open trace file:%s\n",
            config_info->trace_file);

    while (fgets(line, sizeof(line), trace_fp) != NULL) {
        ret = sscanf(line, "%"SCNu64" %"SCNu8" %"SCNu8" %s %"SCNu64" %"SCNu32" "
        "%"SCNu64"\n", &csv_data.timestamp, &csv_data.server_num,
                &csv_data.volume_num, csv_data.req_type, &csv_data.start_addr,
                &csv_data.req_size, &csv_data.duration);
        check(ret==7, "failed to parse csv line:\n\t%s\n", line);
        tot_lines += 1;

        /* first line of trace file, extract starting timestamp */
        if (tot_reqs == 0) {
            starting_time_stamp = csv_data.timestamp;
        }

        req.block_num = (csv_data.start_addr >> LOG_2_BLOCK_SIZE);
        num_blocks = (csv_data.req_size >> LOG_2_BLOCK_SIZE);
        if (num_blocks == 0) {
            num_blocks = 1;
        }

        if (strcmp(csv_data.req_type, "Write") == 0) {
            req.req_type = 1;
        } else {
            req.req_type = 0;
        }

        req.server_num = csv_data.server_num;
        req.volume_num = csv_data.volume_num;
        req.sub_window_ind = (csv_data.timestamp - starting_time_stamp)
                            / SUB_WINDOW_SIZE;

        for (i = 0; i < num_blocks; i++) {
            tot_reqs += 1;
            if (tot_reqs % 292000000 == 0) {
                printf("%d%% is done.\n", progress);
                progress += 10;
            }

            if (req.req_type == 0) {
                // read request
                tot_reads += 1;
                ret = lru_cache_read_lookup(write_buffer, &req);
                if (ret == 1) {
                    continue;
                } else {
                    ret = lru_cache_lookup(ssd_cache, &req);
                }
                if (ret == 0) {
                    ret = miss_filter_lookup(miss_filter, &req);
                    if (ret == 1) {
                        /*hits in miss_filter, check miss_table*/
                        ret = miss_table_access(miss_table, &req);
                        /*hits in miss_table, insert it to ssd_cache*/
                        if (ret == 1) {
                            lru_cache_insert(ssd_cache, req.block_num,
                                    req.server_num, req.volume_num,
                                    &replaced_req);
                        } // hits in miss_table
                    } // hits in miss_filter
                }
            } else {
                // write request
                tot_writes += 1;
                ret = lru_cache_lookup(write_buffer, &req);
                if (ret == 0) {
                    ret = lru_cache_peek(ssd_cache, &req);
                    if (ret == 0) {
                        // miss in ssd_cache
                        ret = miss_filter_lookup(miss_filter, &req);
                        if (ret == 1) {
                            /*hits in miss_filter, check miss_table*/
                            ret = miss_table_access(miss_table, &req);
                            /*hits in miss_table, insert it to ssd_cache*/
                            if (ret == 1) {

                                ret = lru_cache_insert(write_buffer,
                                        req.block_num, req.server_num,
                                        req.volume_num, &replaced_req);
                                if (ret == 1) {
                                    ret = lru_cache_peek (ssd_cache, &replaced_req);
                                    if (ret == 1) {
                                        ret = lru_cache_update(ssd_cache,
                                                &replaced_req);
                                        check(ret==0,
                                                "failed to update entry in ssd_cache.");
                                    } else {
                                        lru_cache_insert(ssd_cache,
                                            replaced_req.block_num,
                                            replaced_req.server_num,
                                            replaced_req.volume_num, &req);
                                    }
                                } // write buffer has a replaced entry
                            } // hits in miss_table
                        } // hits in miss_filter
                    } else {
                        // hits in ssd_cache
                        ret = lru_cache_remove (ssd_cache, &req);
                        check (ret==0, "failed to remove entry from ssd_cache.");
                        ret = lru_cache_insert(write_buffer, req.block_num,
                                req.server_num, req.volume_num, &replaced_req);
                        if (ret == 1) {
                            ret = lru_cache_peek(ssd_cache, &replaced_req);
                            if (ret == 1) {
                                ret = lru_cache_update(ssd_cache,
                                        &replaced_req);
                                check(ret==0,
                                        "failed to update entry in ssd_cache.");
                            } else {
                                lru_cache_insert(ssd_cache,
                                        replaced_req.block_num,
                                        replaced_req.server_num,
                                        replaced_req.volume_num, &req);
                            }
                        } // write buffer has a replaced entry
                    } // hits in ssd cache
                } // miss in write buffer
            } // write request
            req.block_num += 1;
        } // loop through each request
    } // loop through each line from the trace file

    gettimeofday(&end, NULL);

    fprintf(out_fp, "total number of requests: %"PRIu64"\n", tot_reqs);
    fprintf(out_fp, "total number of reads:    %"PRIu64"\n", tot_reads);
    fprintf(out_fp, "total number of writes:   %"PRIu64"\n", tot_writes);
    long duration = (end.tv_sec - start.tv_sec) * 1000000
                + (end.tv_usec - start.tv_usec);
    fprintf(out_fp, "time consumed = %.4f (min)\n",
                (double) duration / (1000000.0 * 60.0));

    fprintf(out_fp, "\n");
    fprintf(out_fp, "write buffer: \n");

    fprintf(out_fp, "wb size         :  %"PRIu64"\n", write_buffer->cache_size);
    fprintf(out_fp, "read hits       :  %"PRIu64"\n", write_buffer->read_hits);
    fprintf(out_fp, "read lookups    :  %"PRIu64"\n", write_buffer->read_lookups);
    fprintf(out_fp, "read hits ratio :  %.4f\n",
          (double) write_buffer->read_hits/(double) write_buffer->read_lookups);
    fprintf(out_fp, "write hits      :  %"PRIu64"\n", write_buffer->write_hits);
    fprintf(out_fp, "write lookups   :  %"PRIu64"\n", write_buffer->write_lookups);
    fprintf(out_fp, "write hits ratio:  %.4f\n",
          (double) write_buffer->write_hits/(double) write_buffer->write_lookups);
    fprintf(out_fp, "num inserts     :  %"PRIu64"\n", write_buffer->num_writes);
    fprintf(out_fp, "num updates     :  %"PRIu64"\n", write_buffer->num_updates);
    fprintf(out_fp, "num replaces    :  %"PRIu64"\n", write_buffer->num_replaces);

    fprintf(out_fp, "\n");
    fprintf(out_fp, "ssd cache: \n");

    fprintf(out_fp, "ssd cache size  :  %"PRIu64"\n", ssd_cache->cache_size);
    fprintf(out_fp, "read hits       :  %"PRIu64"\n", ssd_cache->read_hits);
    fprintf(out_fp, "read lookups    :  %"PRIu64"\n", ssd_cache->read_lookups);
    fprintf(out_fp, "read hits ratio :  %.4f\n",
          (double) ssd_cache->read_hits/(double) ssd_cache->read_lookups);
    fprintf(out_fp, "write hits      :  %"PRIu64"\n", ssd_cache->write_hits);
    fprintf(out_fp, "write lookups   :  %"PRIu64"\n", ssd_cache->write_lookups);
    fprintf(out_fp, "write hits ratio:  %.4f\n",
          (double) ssd_cache->write_hits/(double) ssd_cache->write_lookups);
    fprintf(out_fp, "num inserts     :  %"PRIu64"\n", ssd_cache->num_writes);
    fprintf(out_fp, "num removes     :  %"PRIu64"\n", ssd_cache->num_removes);
    fprintf(out_fp, "num updates     :  %"PRIu64"\n", ssd_cache->num_updates);
    fprintf(out_fp, "num replaces    :  %"PRIu64"\n", ssd_cache->num_replaces);

    /* destroy resource */
    fclose(trace_fp);
    fclose(out_fp);
    lru_cache_destroy (write_buffer);
    miss_filter_destroy(miss_filter);
    miss_table_destroy(miss_table);
    lru_cache_destroy(ssd_cache);

    return 0;

error:
    if (trace_fp != NULL) {
        fclose(trace_fp);
    }

    if (out_fp != NULL) {
        fclose(out_fp);
    }

    if (write_buffer != NULL) {
        lru_cache_destroy (write_buffer);
    }

    if (miss_filter != NULL) {
        miss_filter_destroy(miss_filter);
    }

    if (miss_table != NULL) {
        miss_table_destroy(miss_table);
    }

    if (ssd_cache != NULL) {
        lru_cache_destroy(ssd_cache);
    }

    return -1;
}

int run_sieve_store_base (ConfigInfo *config_info) {
    int ret;
    int64_t i;
    FILE *trace_fp = NULL;
    FILE *out_fp   = NULL;
    char line[FILE_LINE_SIZE];
    CSVLineData csv_data;

    Request req, replaced_req;
    int num_blocks;
    int progress = 10;

    MissFilter *miss_filter  = NULL;
    MissTable  *miss_table   = NULL;
    LRUCache   *ssd_cache    = NULL;

    struct timeval start, end;
    uint64_t  tot_reqs = 0;

    /* initializations */

    out_fp = fopen (config_info->result_file, "w");
    check (out_fp!=NULL, "failed to open result file: %s.",
            config_info->result_file);

    miss_filter = (MissFilter *) calloc(1, sizeof(MissFilter));
    check(miss_filter!=NULL, "failed to allocate miss_filter.");
    ret = miss_filter_init(config_info->miss_filter_size,
            config_info->miss_filter_threshold, config_info->num_sub_windows,
            miss_filter);
    check(ret!=-1, "failed to initialize miss_filter.");

    miss_table = (MissTable *) calloc(1, sizeof(MissTable));
    check(miss_table!=NULL, "failed to allocate miss_table.");
    ret = miss_table_init(config_info->miss_table_lookup_size,
            config_info->miss_table_threshold, config_info->num_sub_windows,
            miss_table);
    check(ret!=-1, "failed to initialize miss_table.");

    ssd_cache = (LRUCache *) calloc(1, sizeof(LRUCache));
    check(ssd_cache!=NULL, "failed to allocate ssd_cache.");
    ret = lru_cache_init(config_info->ssd_size, ssd_cache);
    check(ret!=-1, "failed to initialize ssd_cache.");

    /* start simulation */
    gettimeofday(&start, NULL);

    /* go over trace file */
    trace_fp = fopen(config_info->trace_file, "r");
    check(trace_fp!=NULL, "failed to open trace file:%s\n",
            config_info->trace_file);

/*    out_fp = fopen ("allocations.txt", "w");
    check (out_fp!=NULL, "failed to open output file.\n");*/

    while (fgets(line, sizeof(line), trace_fp) != NULL) {
        ret = sscanf(line, "%"SCNu64" %"SCNu8" %"SCNu8" %s %"SCNu64" %"SCNu32" "
        "%"SCNu64"\n", &csv_data.timestamp, &csv_data.server_num,
                &csv_data.volume_num, csv_data.req_type, &csv_data.start_addr,
                &csv_data.req_size, &csv_data.duration);
        check(ret==7, "failed to parse csv line:\n\t%s\n", line);

        /* first line of trace file, extract starting timestamp */
        if (tot_reqs == 0) {
            starting_time_stamp = csv_data.timestamp;
        }

        req.server_num = csv_data.server_num;
        req.volume_num = csv_data.volume_num;
        req.sub_window_ind = (csv_data.timestamp - starting_time_stamp)
                / SUB_WINDOW_SIZE;

        req.block_num = (csv_data.start_addr >> LOG_2_BLOCK_SIZE);
        num_blocks = (csv_data.req_size >> LOG_2_BLOCK_SIZE);
        if (num_blocks == 0) {
            num_blocks = 1;
        }

        if (strcmp(csv_data.req_type, "Write") == 0) {
            req.req_type = 1;
        } else {
            req.req_type = 0;
        }

        for (i = 0; i < num_blocks; i++) {
            tot_reqs += 1;

            if (tot_reqs % 292000000 == 0) {
                printf("%d%% is done.\n", progress);
                progress += 10;
            }

            ret = lru_cache_lookup(ssd_cache, &req);
            if (ret == 0) {
                /*miss in ssd_cache, check miss_filter*/
                ret = miss_filter_lookup(miss_filter, &req);
                if (ret == 1) {
                    /*hits in miss_filter, check miss_table*/
                    ret = miss_table_access(miss_table, &req);
                    /*hits in miss_table, insert it to ssd_cache*/
                    if (ret == 1) {
                        lru_cache_insert(ssd_cache, req.block_num,
                                req.server_num, req.volume_num, &replaced_req);
                    } // hits in miss_table
                } // hits in miss_filter
            } // miss in ssd_cache

            req.block_num += 1;
        } // loop through each request
    } // loop through each line from the trace file

    gettimeofday(&end, NULL);

    fprintf(out_fp, "total number of reads:    %"PRIu64"\n", ssd_cache->read_lookups);
    fprintf(out_fp, "total number of writes:   %"PRIu64"\n", ssd_cache->write_lookups);
    fprintf(out_fp, "total number of requests: %"PRIu64"\n", tot_reqs);
    long duration = (end.tv_sec - start.tv_sec) * 1000000
               + (end.tv_usec - start.tv_usec);
    fprintf(out_fp, "time consumed = %.4f (min)\n",
               (double) duration / (1000000.0 * 60.0));

    fprintf(out_fp, "\n\n");
    fprintf(out_fp, "read hits       :  %"PRIu64"\n", ssd_cache->read_hits);
    fprintf(out_fp, "read lookups    :  %"PRIu64"\n", ssd_cache->read_lookups);
    fprintf(out_fp, "read hits ratio :  %.4f\n",
            (double) ssd_cache->read_hits / (double) ssd_cache->read_lookups);
    fprintf(out_fp, "write hits      :  %"PRIu64"\n", ssd_cache->write_hits);
    fprintf(out_fp, "write lookups   :  %"PRIu64"\n", ssd_cache->write_lookups);
    fprintf(out_fp, "write hits ratio:  %.4f\n",
            (double) ssd_cache->write_hits / (double) ssd_cache->write_lookups);
    fprintf(out_fp, "tot_hits_ratio  :  %.4f\n",
            (double) (ssd_cache->write_hits + ssd_cache->read_hits)
                    / (double) (ssd_cache->write_lookups
                            + ssd_cache->read_lookups));

    fprintf(out_fp, "ssd cache writes :  %"PRIu64"\n", ssd_cache->num_writes);
    fprintf(out_fp, "ssd cache replacements: %"PRIu64"\n", ssd_cache->num_replaces);

    /* destroy resource */
    fclose(trace_fp);
    fclose(out_fp);
    miss_filter_destroy(miss_filter);
    miss_table_destroy(miss_table);
    lru_cache_destroy(ssd_cache);

    return 0;

    error: if (trace_fp != NULL) {
        fclose(trace_fp);
    }

    if (out_fp != NULL) {
        fclose(out_fp);
    }

    if (miss_filter != NULL) {
        miss_filter_destroy(miss_filter);
    }

    if (miss_table != NULL) {
        miss_table_destroy(miss_table);
    }

    if (ssd_cache != NULL) {
        lru_cache_destroy(ssd_cache);
    }

    return -1;
}

int sort_ssd_cache (LRUCache *ssd_cache, LRUCacheEntry ** entry_ptr)
{
    uint64_t num_entries = ssd_cache->entry_count;
    int64_t i, j;
    LRUCacheEntry *entry = NULL;
    int pass = 1;

    for (i = 0; i < num_entries; i++) {
        entry_ptr[i] = &(ssd_cache->cache_entry[i]);
    }

    for (i = 1; i < num_entries; i++) {
        entry = entry_ptr[i];
        j = i - 1;
        while (j >= 0) {
            if (entry->write_access_count > entry_ptr[j]->write_access_count) {
                entry_ptr[j+1] = entry_ptr[j];
                j -= 1;
            } else {
                break;
            }
        }
        entry_ptr[j+1] = entry;
    }

    for (i = 0; i < num_entries-1; i++) {
        if (entry_ptr[i]->write_access_count < entry_ptr[i+1]->write_access_count) {
            pass = 0;
            break;
        }
    }
    check (pass==1, "failed to sort based on write_access_count.");

    return 0;

error:

   return -1;
}

void max_heapify (LRUCacheEntry **entry_ptr, int64_t root, int64_t len)
{
    int64_t l = LCHILD (root);
    int64_t r = RCHILD (root);
    int64_t largest;
    LRUCacheEntry *entry;

    while ((l<len) || (r<len)) {
        if ((l < len)
                && (entry_ptr[l]->write_access_count
                        > entry_ptr[root]->write_access_count)) {
            largest = l;
        } else {
            largest = root;
        }

        if ((r < len)
                && (entry_ptr[r]->write_access_count
                        > entry_ptr[root]->write_access_count)) {
            largest = r;
        }

        if (largest != root) {
            entry = entry_ptr[largest];
            entry_ptr[largest] = entry_ptr[root];
            entry_ptr[root] = entry;
        } else {
            break;
        }

        root = largest;
        l = LCHILD (root);
        r = RCHILD (root);
    }
}

int run_static_buffer (ConfigInfo *config_info) {
    int ret;
    int64_t i;
    FILE *trace_fp = NULL;
    char line[FILE_LINE_SIZE];
    CSVLineData csv_data;

    Request req, replaced_req;
    ReplayReq replay_req;
    int num_blocks;

    MissFilter   *miss_filter  = NULL;
    MissTable    *miss_table   = NULL;
    LRUCache     *ssd_cache    = NULL;
    StaticBuffer *static_ssd   = NULL;
    StaticBuffer *static_wb    = NULL;
/*    ReplayReq    *req_array    = NULL;*/

    struct timeval start, end;
    uint64_t  tot_reqs = 0;
    /* initializations */

    miss_filter = (MissFilter *) calloc(1, sizeof(MissFilter));
    check(miss_filter!=NULL, "failed to allocate miss_filter.");
    ret = miss_filter_init(config_info->miss_filter_size,
            config_info->miss_filter_threshold, config_info->num_sub_windows,
            miss_filter);
    check(ret!=-1, "failed to initialize miss_filter.");

    miss_table = (MissTable *) calloc(1, sizeof(MissTable));
    check(miss_table!=NULL, "failed to allocate miss_table.");
    ret = miss_table_init(config_info->miss_table_lookup_size,
            config_info->miss_table_threshold, config_info->num_sub_windows,
            miss_table);
    check(ret!=-1, "failed to initialize miss_table.");

    ssd_cache = (LRUCache *) calloc(1, sizeof(LRUCache));
    check(ssd_cache!=NULL, "failed to allocate ssd_cache.");
    ret = lru_cache_init(config_info->ssd_size, ssd_cache);
    check(ret!=-1, "failed to initialize ssd_cache.");

    static_ssd = (StaticBuffer *) calloc (1, sizeof(StaticBuffer));
    check (static_ssd!=NULL, "failed to allocate static_ssd.");
    ret = static_buffer_init (static_ssd, STATIC_SSD_SIZE);
    check (ret==0, "failed to initialize static_ssd.");

    static_wb = (StaticBuffer *) calloc (1, sizeof(StaticBuffer));
    check (static_wb!=NULL, "failed to allocate static_wb.");
    ret = static_buffer_init (static_wb, STATIC_WB_SIZE);
    check (ret==0, "failed to initialize static_wb.");

/*    req_array = (ReplayReq *) calloc (NUM_REQS, sizeof(ReplayReq));
    check (req_array!=NULL, "failed to allocate req_array");*/

    /* start simulation */
    gettimeofday(&start, NULL);

    /* go over trace file */
    trace_fp = fopen(config_info->trace_file, "r");
    check(trace_fp!=NULL, "failed to open trace file:%s\n",
            config_info->trace_file);

    while (fgets(line, sizeof(line), trace_fp) != NULL) {
        ret = sscanf(line, "%"SCNu64" %"SCNu8" %"SCNu8" %s %"SCNu64" %"SCNu32" "
        "%"SCNu64"\n", &csv_data.timestamp, &csv_data.server_num,
                &csv_data.volume_num, csv_data.req_type, &csv_data.start_addr,
                &csv_data.req_size, &csv_data.duration);
        check(ret==7, "failed to parse csv line:\n\t%s\n", line);

        /* first line of trace file, extract starting timestamp */
        if (tot_reqs == 0) {
            starting_time_stamp = csv_data.timestamp;
        }

/*        if (csv_data.start_addr == 412225536) {
            printf("debug\n");
        }*/

        req.block_num = (csv_data.start_addr >> LOG_2_BLOCK_SIZE);
        num_blocks = (csv_data.req_size >> LOG_2_BLOCK_SIZE);
        if (num_blocks == 0) {
            num_blocks = 1;
        }

        if (strcmp(csv_data.req_type, "Write") == 0) {
            req.req_type = 1;
        } else {
            req.req_type = 0;
        }

        req.server_num = csv_data.server_num;
        req.volume_num = csv_data.volume_num;
        req.sub_window_ind = (csv_data.timestamp - starting_time_stamp)
                / SUB_WINDOW_SIZE;

        for (i = 0; i < num_blocks; i++) {

/*            req_array[tot_reqs].block_num = req.block_num;
            req_array[tot_reqs].server_num = req.server_num;
            req_array[tot_reqs].volume_num = req.volume_num;
            req_array[tot_reqs].req_type = req.req_type;*/

            /*printf ("%"PRIu8" %"PRIu8" %"PRIu64" %"PRIu32"\n", req.server_num,
             req.volume_num, req.block_num, req.sub_window_ind);*/

            tot_reqs += 1;
/*            if (req.block_num == 100746) {
                debug_counter += 1;
                printf("%s\n", line);
            }*/

            ret = lru_cache_lookup(ssd_cache, &req);
            if (ret == 0) {
                /*miss in ssd_cache, check miss_filter*/
                ret = miss_filter_lookup(miss_filter, &req);
                if (ret == 1) {
                    /*hits in miss_filter, check miss_table*/
                    ret = miss_table_access(miss_table, &req);
                    /*hits in miss_table, insert it to ssd_cache*/
                    if (ret == 1) {
                        lru_cache_insert(ssd_cache, req.block_num,
                                req.server_num, req.volume_num, &replaced_req);
                    } // hits in miss_table
                } // hits in miss_filter
            } // miss in ssd_cache

            req.block_num += 1;
        } // loop through each request
    } // loop through each line from the trace file

    fclose(trace_fp);
    trace_fp = NULL;

    gettimeofday(&end, NULL);
    long duration = (end.tv_sec - start.tv_sec) * 1000000
            + (end.tv_usec - start.tv_usec);
    printf("base line sieved store time consumed = %.4f (min)\n",
            (double) duration / (1000000.0 * 60.0));

    gettimeofday(&start, NULL);
    /* build static ssd buffer */
    printf ("start building static buffer ...\n");
    check (ssd_cache->entry_count==STATIC_SSD_SIZE,
            "ssd_cache->entry_count = %"PRIu64"", ssd_cache->entry_count);
    uint64_t tot_entries = ssd_cache->entry_count;
    uint64_t block_num, server_num, volume_num;
    for (i = 0; i < tot_entries; i++) {
        block_num  = ssd_cache->cache_entry[i].block_num;
        server_num = ssd_cache->cache_entry[i].server_num;
        volume_num = ssd_cache->cache_entry[i].volume_num;
        ret = static_buffer_insert (static_ssd, block_num, server_num, volume_num);
        check (ret==0, "failed to insert to static_ssd.");
    }

    /* build static write buffer */
    LRUCacheEntry **entry_ptr = (LRUCacheEntry **) calloc (STATIC_SSD_SIZE,
            sizeof(LRUCacheEntry *));
    check (entry_ptr!=NULL, "failed to allocate entry_ptr.");
    for (i = 0; i < STATIC_SSD_SIZE; i++) {
        entry_ptr[i] = &(ssd_cache->cache_entry[i]);
    }
/*    ret = sort_ssd_cache (ssd_cache, entry_ptr);
    check (ret==0, "failed to sort ssd cache.");*/
    /* building max heap based on write_access_count */
    uint64_t half_array_len = (STATIC_SSD_SIZE >> 1);
    for (i = half_array_len; i >= 0; i--) {
        max_heapify (entry_ptr, i, STATIC_SSD_SIZE);
    }

    tot_entries = STATIC_WB_SIZE;
    int64_t len = STATIC_WB_SIZE;

    for (i = 0; i < tot_entries; i++) {
        block_num = entry_ptr[0]->block_num;
        server_num = entry_ptr[0]->server_num;
        volume_num = entry_ptr[0]->volume_num;
        ret = static_buffer_insert (static_wb, block_num, server_num, volume_num);
        check (ret==0, "failed to insert to static_wb.");

        entry_ptr[0] = entry_ptr[len-1];
        len -= 1;
        max_heapify (entry_ptr, 0, len);
    }
    free (entry_ptr);
    printf ("finish building static wb.\n");
    gettimeofday(&end, NULL);
    duration = (end.tv_sec - start.tv_sec) * 1000000
            + (end.tv_usec - start.tv_usec);
    printf("construct write buffer time consumed = %.4f (min)\n",
            (double) duration / (1000000.0 * 60.0));

    gettimeofday(&start, NULL);

    /* replay request trace */
/*    uint64_t step_size = tot_reqs / 10;
    for (i = 0; i < tot_reqs; i++) {
        ret = static_buffer_lookup (static_wb, &(req_array[i]));
        if (ret == 0) {
            ret = static_buffer_lookup (static_ssd, &(req_array[i]));
        }
        if (((i+1) % step_size) == 0) {
            printf ("%"PRIu64" out of %"PRIu64" are done.\n", i, tot_reqs);
        }
    }*/
    trace_fp = fopen(config_info->trace_file, "r");
    check(trace_fp!=NULL, "failed to open trace file:%s\n",
            config_info->trace_file);

    while (fgets(line, sizeof(line), trace_fp) != NULL) {
        ret = sscanf(line, "%"SCNu64" %"SCNu8" %"SCNu8" %s %"SCNu64" %"SCNu32" "
        "%"SCNu64"\n", &csv_data.timestamp, &csv_data.server_num,
                &csv_data.volume_num, csv_data.req_type, &csv_data.start_addr,
                &csv_data.req_size, &csv_data.duration);
        check(ret==7, "failed to parse csv line:\n\t%s\n", line);

        replay_req.block_num = (csv_data.start_addr >> 12);
        num_blocks = (csv_data.req_size >> 12);
        if (num_blocks == 0) {
            num_blocks = 1;
        }

        if (strcmp(csv_data.req_type, "Write") == 0) {
            replay_req.req_type = 1;
        } else {
            replay_req.req_type = 0;
        }

        replay_req.server_num = csv_data.server_num;
        replay_req.volume_num = csv_data.volume_num;

        for (i = 0; i < num_blocks; i++) {
/*            ret = static_buffer_lookup(static_wb, &replay_req);
            if (ret == 0) {
                ret = static_buffer_lookup(static_ssd, &replay_req);
            }*/
            static_buffer_lookup(static_ssd, &replay_req);
            replay_req.block_num += 1;
        }
    }
    fclose (trace_fp);

    gettimeofday(&end, NULL);

    duration = (end.tv_sec - start.tv_sec) * 1000000
            + (end.tv_usec - start.tv_usec);
    printf("trace replay time consumed = %.4f (min)\n",
            (double) duration / (1000000.0 * 60.0));

    printf("============ sieve_store_base ================\n");

    printf("total number of reads:    %"PRIu64"\n", ssd_cache->read_lookups);
    printf("total number of writes:   %"PRIu64"\n", ssd_cache->write_lookups);
    printf("total number of requests: %"PRIu64"\n", tot_reqs);

    printf("==============================================\n");
    printf("read hits       :  %"PRIu64"\n", ssd_cache->read_hits);
    printf("read lookups    :  %"PRIu64"\n", ssd_cache->read_lookups);
    printf("read hits ratio :  %.4f\n",
            (double) ssd_cache->read_hits / (double) ssd_cache->read_lookups);
    printf("write hits      :  %"PRIu64"\n", ssd_cache->write_hits);
    printf("write lookups   :  %"PRIu64"\n", ssd_cache->write_lookups);
    printf("write hits ratio:  %.4f\n",
            (double) ssd_cache->write_hits / (double) ssd_cache->write_lookups);

    printf("==============================================\n");

    printf("ssd cache inserts:    %"PRIu64"\n", ssd_cache->num_writes);
    printf("ssd cache updates:    %"PRIu64"\n", ssd_cache->num_updates);
    printf("ssd cache replaces:   %"PRIu64"\n", ssd_cache->num_replaces);

    printf("==============================================\n");

    printf ("static_wb write hits  = %"PRIu64"\n", static_wb->write_hits);
    printf ("static_ssd write hits = %"PRIu64"\n", static_ssd->write_hits);

    printf("==============================================\n");

    /* destroy resource */

    miss_filter_destroy(miss_filter);
    miss_table_destroy(miss_table);
    lru_cache_destroy(ssd_cache);
    static_buffer_destroy(static_ssd);
    static_buffer_destroy(static_wb);
    /*free (req_array);*/
    return 0;

error:

    if (trace_fp != NULL) {
        fclose(trace_fp);
    }

    if (miss_filter != NULL) {
        miss_filter_destroy(miss_filter);
    }

    if (miss_table != NULL) {
        miss_table_destroy(miss_table);
    }

    if (ssd_cache != NULL) {
        lru_cache_destroy(ssd_cache);
    }

    if (static_ssd != NULL) {
        static_buffer_destroy(static_ssd);
    }

    if (static_wb != NULL) {
        static_buffer_destroy(static_wb);
    }

/*    if (req_array != NULL) {
        free (req_array);
    }*/

    return -1;
}

int run_sieved_plus_traditional_wb (ConfigInfo *config_info) {
    int ret;
    uint64_t i;
    FILE *trace_fp = NULL;
    FILE *out_fp = NULL;
    FILE *debug_fp = NULL;
    char line[FILE_LINE_SIZE];
    CSVLineData csv_data;

    Request req, replaced_req;
    int num_blocks;
    int progress = 10;

    LRUCache   *s_wb = NULL;
    LRUCache   *tr_wb = NULL;
    MissFilter *miss_filter = NULL;
    MissTable  *miss_table = NULL;
    GhostCache *wb_ghost_cache = NULL;
    LRUCache   *ssd_cache = NULL;

    struct timeval start, end;
    uint64_t tot_reads = 0, tot_writes = 0, tot_reqs = 0, tot_lines = 1;

    /* initializations */

    out_fp = fopen (config_info->result_file, "w");
    check (out_fp!=NULL, "failed to open result file: %s.",
                config_info->result_file);

    debug_fp = fopen (config_info->debug_file, "w");
    check (debug_fp!=NULL, "failed to open debug file: %s.",
            config_info->debug_file);

    s_wb = (LRUCache *) calloc (1, sizeof(LRUCache));
    check (s_wb!=NULL, "failed to allocate s_wb.");
    ret = lru_cache_init (config_info->sieved_wb_size, s_wb);
    check (ret != -1, "failed to initialize s_wb.");

    miss_filter = (MissFilter *) calloc(1, sizeof(MissFilter));
    check(miss_filter!=NULL, "failed to allocate miss_filter.");
    ret = miss_filter_init(config_info->miss_filter_size,
            config_info->miss_filter_threshold, config_info->num_sub_windows,
            miss_filter);
    check(ret!=-1, "failed to initialize miss_filter.");

    miss_table = (MissTable *) calloc(1, sizeof(MissTable));
    check(miss_table!=NULL, "failed to allocate miss_table.");
    ret = miss_table_init(config_info->miss_table_lookup_size,
            config_info->miss_table_threshold, config_info->num_sub_windows,
            miss_table);
    check(ret!=-1, "failed to initialize miss_table.");

    wb_ghost_cache = (GhostCache *) calloc(1, sizeof(GhostCache));
    check(wb_ghost_cache!=NULL, "failed to allocate wb_ghost_cache.");
    ret = ghost_cache_init(config_info->sieved_ghost_cache_size,
            config_info->sieved_wb_threshold, config_info->num_sub_windows,
            wb_ghost_cache);
    check(ret!=-1, "failed to initialize wb_ghost_cache.");

    tr_wb = (LRUCache *) calloc(1, sizeof(LRUCache));
    check(tr_wb!=NULL, "failed to allocate tr_wb.");
    ret = lru_cache_init(config_info->traditional_wb_size, tr_wb);
    check(ret!=-1, "failed to initialize tr_wb.");

    ssd_cache = (LRUCache *) calloc(1, sizeof(LRUCache));
    check(ssd_cache!=NULL, "failed to allocate ssd_cache.");
    ret = lru_cache_init(config_info->ssd_size, ssd_cache);
    check(ret!=-1, "failed to initialize ssd_cache.");

    /* start simulation */
    gettimeofday(&start, NULL);

    /* go over trace file */
    trace_fp = fopen(config_info->trace_file, "r");
    check(trace_fp!=NULL, "failed to open trace file:%s\n",
            config_info->trace_file);

    while (fgets(line, sizeof(line), trace_fp) != NULL) {
        ret = sscanf(line, "%"SCNu64" %"SCNu8" %"SCNu8" %s %"SCNu64" %"SCNu32" "
        "%"SCNu64"\n", &csv_data.timestamp, &csv_data.server_num,
                &csv_data.volume_num, csv_data.req_type, &csv_data.start_addr,
                &csv_data.req_size, &csv_data.duration);
        check(ret==7, "failed to parse csv line:\n\t%s\n", line);
        tot_lines += 1;

        /* first line of trace file, extract starting timestamp */
        if (tot_reqs == 0) {
            starting_time_stamp = csv_data.timestamp;
        }

        req.block_num = (csv_data.start_addr >> LOG_2_BLOCK_SIZE);
        num_blocks = (csv_data.req_size >> LOG_2_BLOCK_SIZE);
        if (num_blocks == 0) {
            num_blocks = 1;
        }

        if (strcmp(csv_data.req_type, "Write") == 0) {
            req.req_type = 1;
        } else {
            req.req_type = 0;
        }

        req.server_num = csv_data.server_num;
        req.volume_num = csv_data.volume_num;
        req.sub_window_ind = (csv_data.timestamp - starting_time_stamp)
                            / SUB_WINDOW_SIZE;

        for (i = 0; i < num_blocks; i++) {

            tot_reqs += 1;

            if (tot_reqs % 292000000 == 0) {
                printf("%d%% is done.\n", progress);
                progress += 10;
            }

            if (req.req_type == 0) {
                // read request
                tot_reads += 1;
                ret = lru_cache_read_lookup(s_wb, &req);
                if (ret == 0) {
                    ret = lru_cache_read_lookup(tr_wb, &req);
                }
                if (ret == 0) {
                    ret = lru_cache_lookup(ssd_cache, &req);
                }
                if (ret == 0) {
                    ret = miss_filter_lookup(miss_filter, &req);
                    if (ret == 1) {
                        /*hits in miss_filter, check miss_table*/
                        ret = miss_table_access(miss_table, &req);
                        /*hits in miss_table, insert it to ssd_cache*/
                        if (ret == 1) {
                            lru_cache_insert(ssd_cache, req.block_num,
                                    req.server_num, req.volume_num,
                                    &replaced_req);
                        } // hits in miss_table
                    } // hits in miss_filter
                }
            } else {
                // write request
                tot_writes += 1;
                ret = lru_cache_lookup(s_wb, &req);
                if (ret == 0) {
                    ret = lru_cache_peek(tr_wb, &req);
                    if (ret == 1) {
                        // hits in tr_wb
                        ret = ghost_cache_access(wb_ghost_cache, &req);
                        if (ret == 1) {
                            fprintf(debug_fp, "%"PRIu64" %"PRIu64" %"PRIu8" %"PRIu8"\n",
                                csv_data.timestamp, req.block_num, req.server_num, req.volume_num);
                            ret = lru_cache_remove(tr_wb, &req);
                            check(ret==0,
                                    "failed to remove entry from tr_wb.");
                            ret = lru_cache_insert(s_wb, req.block_num,
                                    req.server_num, req.volume_num,
                                    &replaced_req);
                            if (ret == 1) {
                                ret = lru_cache_peek(tr_wb, &replaced_req);
                                if (ret == 1) {
                                    ret = lru_cache_update(tr_wb,
                                            &replaced_req);
                                    check(ret==0,
                                            "failed to update entry in tr_wb.");
                                } else {
                                    ret = lru_cache_insert(tr_wb,
                                            replaced_req.block_num,
                                            replaced_req.server_num,
                                            replaced_req.volume_num, &req);
                                    if (ret == 1) {
                                        ret = lru_cache_peek(ssd_cache,
                                                &req);
                                        if (ret == 1) {
                                            ret = lru_cache_update(ssd_cache,
                                                    &req);
                                            check(ret==0,
                                                    "failed to update entry in ssd_cache.");
                                        } else {
                                            lru_cache_insert(ssd_cache,
                                                    req.block_num,
                                                    req.server_num,
                                                    req.volume_num,
                                                    &replaced_req);
                                        }
                                    }
                                }
                            } // write buffer has a replaced entry
                        } else {
                            // cannot make into the write buffer
                            // write to ssd instead.
                            ret = lru_cache_lookup(tr_wb, &req);
                        }

                    } // hits in tr_wb
                    else {
                        ret = lru_cache_peek(ssd_cache, &req);
                        if (ret == 0) {
                            // miss in ssd_cache
                            ret = miss_filter_lookup(miss_filter, &req);
                            if (ret == 1) {
                                /*hits in miss_filter, check miss_table*/
                                ret = miss_table_access(miss_table, &req);
                                /*hits in miss_table, insert it to ssd_cache*/
                                if (ret == 1) {
                                    ret = ghost_cache_access(wb_ghost_cache,
                                            &req);
                                    if (ret == 0) {
                                        // allocate to tr_wb
                                        ret = lru_cache_insert(tr_wb,
                                                req.block_num, req.server_num,
                                                req.volume_num, &replaced_req);
                                        if (ret == 1) {
                                            ret = lru_cache_peek(ssd_cache,
                                                    &replaced_req);
                                            if (ret == 1) {
                                                ret = lru_cache_update(
                                                        ssd_cache,
                                                        &replaced_req);
                                                check(ret==0,
                                                        "failed to update entry in ssd_cache.");
                                            } else {
                                                lru_cache_insert(ssd_cache,
                                                        replaced_req.block_num,
                                                        replaced_req.server_num,
                                                        replaced_req.volume_num,
                                                        &req);
                                            }
                                        }
                                    } else {
                                        // allocate to wb
                                        fprintf(debug_fp, "%"PRIu64" %"PRIu64" %"PRIu8" %"PRIu8"\n",
                                            csv_data.timestamp, req.block_num, req.server_num, req.volume_num);
                                        ret = lru_cache_insert(s_wb,
                                                req.block_num, req.server_num,
                                                req.volume_num, &replaced_req);
                                        if (ret == 1) {
                                            ret = lru_cache_peek(tr_wb,
                                                    &replaced_req);
                                            if (ret == 1) {
                                                ret = lru_cache_update(
                                                        ssd_cache,
                                                        &replaced_req);
                                                check(ret==0,
                                                        "failed to update entry in wr_wb.");
                                            } else {
                                                ret = lru_cache_insert(tr_wb,
                                                        replaced_req.block_num,
                                                        replaced_req.server_num,
                                                        replaced_req.volume_num,
                                                        &req);
                                                if (ret == 1) {
                                                    ret = lru_cache_peek(
                                                            ssd_cache, &req);
                                                    if (ret == 1) {
                                                        ret = lru_cache_update(
                                                                ssd_cache,
                                                                &req);
                                                        check(ret==0,
                                                                "failed to update entry in ssd_cache.");
                                                    } else {
                                                        ret = lru_cache_insert(
                                                                ssd_cache,
                                                                req.block_num,
                                                                req.server_num,
                                                                req.volume_num,
                                                                &replaced_req);
                                                    }
                                                }
                                            }
                                        } // write buffer has a replaced entry
                                    } // allocate to wb
                                } // hits in miss_table
                            } // hits in miss_filter
                        } else {
                            // hits in ssd_cache
                            ret = ghost_cache_access(wb_ghost_cache, &req);
                            if (ret == 1) {
                                fprintf(debug_fp, "%"PRIu64" %"PRIu64" %"PRIu8" %"PRIu8"\n",
                                    csv_data.timestamp, req.block_num, req.server_num, req.volume_num);
                                ret = lru_cache_remove(ssd_cache, &req);
                                check(ret==0,
                                        "failed to remove entry from ssd_cache.");
                                ret = lru_cache_insert(s_wb,
                                        req.block_num, req.server_num,
                                        req.volume_num, &replaced_req);
                                if (ret == 1) {
                                    ret = lru_cache_peek(ssd_cache,
                                            &replaced_req);
                                    if (ret == 1) {
                                        ret = lru_cache_update(ssd_cache,
                                                &replaced_req);
                                        check(ret==0,
                                                "failed to update entry in ssd_cache.");
                                    } else {
                                        lru_cache_insert(ssd_cache,
                                                replaced_req.block_num,
                                                replaced_req.server_num,
                                                replaced_req.volume_num, &req);
                                    }
                                } // write buffer has a replaced entry
                            } else {
                                // cannot make into the write buffer
                                // write to ssd instead.
                                ret = lru_cache_lookup(ssd_cache, &req);
                            }
                        }
                    } // misses in tr_wb
                } // misses in sieved_wb
            } // write request
            req.block_num += 1;
        } // loop through each request
    } // loop through each line from the trace file

    gettimeofday(&end, NULL);

    fprintf(out_fp, "total number of requests: %"PRIu64"\n", tot_reqs);
    fprintf(out_fp, "total number of reads:    %"PRIu64"\n", tot_reads);
    fprintf(out_fp, "total number of writes:   %"PRIu64"\n", tot_writes);
    long duration = (end.tv_sec - start.tv_sec) * 1000000
                + (end.tv_usec - start.tv_usec);
    fprintf(out_fp, "time consumed = %.4f (min)\n",
                (double) duration / (1000000.0 * 60.0));

    fprintf(out_fp, "\n");
    fprintf(out_fp, "sieved write buffer: \n");
    fprintf(out_fp, "sieved_wb_size  :  %"PRIu64"\n", s_wb->cache_size);
    fprintf(out_fp, "read hits       :  %"PRIu64"\n", s_wb->read_hits);
    fprintf(out_fp, "read lookups    :  %"PRIu64"\n", s_wb->read_lookups);
    fprintf(out_fp, "read hits ratio :  %.4f\n",
          (double) s_wb->read_hits/(double) s_wb->read_lookups);
    fprintf(out_fp, "write hits      :  %"PRIu64"\n", s_wb->write_hits);
    fprintf(out_fp, "write lookups   :  %"PRIu64"\n", s_wb->write_lookups);
    fprintf(out_fp, "write hits ratio:  %.4f\n",
          (double) s_wb->write_hits/(double) s_wb->write_lookups);
    fprintf(out_fp, "num inserts     :  %"PRIu64"\n", s_wb->num_writes);
    fprintf(out_fp, "num updates     :  %"PRIu64"\n", s_wb->num_updates);
    fprintf(out_fp, "num replaces    :  %"PRIu64"\n", s_wb->num_replaces);

    fprintf(out_fp, "\n");
    fprintf(out_fp, "traditional write buffer: \n");
    fprintf(out_fp, "sieved_wb_size  :  %"PRIu64"\n", tr_wb->cache_size);
    fprintf(out_fp, "read hits       :  %"PRIu64"\n", tr_wb->read_hits);
    fprintf(out_fp, "read lookups    :  %"PRIu64"\n", tr_wb->read_lookups);
    fprintf(out_fp, "read hits ratio :  %.4f\n",
            (double) tr_wb->read_hits / (double) tr_wb->read_lookups);
    fprintf(out_fp, "write hits      :  %"PRIu64"\n", tr_wb->write_hits);
    fprintf(out_fp, "write lookups   :  %"PRIu64"\n", tr_wb->write_lookups);
    fprintf(out_fp, "write hits ratio:  %.4f\n",
            (double) tr_wb->write_hits / (double) tr_wb->write_lookups);
    fprintf(out_fp, "num inserts     :  %"PRIu64"\n", tr_wb->num_writes);
    fprintf(out_fp, "num removes     :  %"PRIu64"\n", tr_wb->num_removes);
    fprintf(out_fp, "num updates     :  %"PRIu64"\n", tr_wb->num_updates);
    fprintf(out_fp, "num replaces    :  %"PRIu64"\n", tr_wb->num_replaces);

    fprintf(out_fp, "\n");
    fprintf(out_fp, "ssd cache: \n");

    fprintf(out_fp, "ssd cache size: %"PRIu64":\n", ssd_cache->cache_size);
    fprintf(out_fp, "ssd cache miss filter threshold: %"PRIu32"\n",
            miss_filter->threshold);
    fprintf(out_fp, "ssd cache miss table threshold: %"PRIu32"\n",
            miss_table->threshold);
    fprintf(out_fp, "num sub windows: %"PRIu8"\n", miss_filter->num_sub_windows);
    fprintf(out_fp, "read hits       :  %"PRIu64"\n", ssd_cache->read_hits);
    fprintf(out_fp, "read lookups    :  %"PRIu64"\n", ssd_cache->read_lookups);
    fprintf(out_fp, "read hits ratio :  %.4f\n",
          (double) ssd_cache->read_hits/(double) ssd_cache->read_lookups);
    fprintf(out_fp, "write hits      :  %"PRIu64"\n", ssd_cache->write_hits);
    fprintf(out_fp, "write lookups   :  %"PRIu64"\n", ssd_cache->write_lookups);
    fprintf(out_fp, "write hits ratio:  %.4f\n",
          (double) ssd_cache->write_hits/(double) ssd_cache->write_lookups);
    fprintf(out_fp, "num inserts     :  %"PRIu64"\n", ssd_cache->num_writes);
    fprintf(out_fp, "num removes     :  %"PRIu64"\n", ssd_cache->num_removes);
    fprintf(out_fp, "num updates     :  %"PRIu64"\n", ssd_cache->num_updates);
    fprintf(out_fp, "num replaces    :  %"PRIu64"\n", ssd_cache->num_replaces);

    fprintf(out_fp, "\n");
    fprintf(out_fp, "ghost cache: \n");

    fprintf (out_fp, "ghost cache size: %"PRIu64"\n", wb_ghost_cache->cache_size);
    fprintf (out_fp, "ghost cache threshold: %"PRIu32"\n", wb_ghost_cache->threshold);
    fprintf (out_fp, "ghost cache num_sub_windows: %"PRIu8"\n",
            wb_ghost_cache->num_sub_windows);
    fprintf (out_fp, "num_inserts    :  %"PRIu32"\n", wb_ghost_cache->num_inserts);
    fprintf (out_fp, "num_replaces    : %"PRIu32"\n", wb_ghost_cache->num_replaces);

    /* destroy resource */
    fclose(trace_fp);
    fclose(out_fp);
    fclose(debug_fp);
    lru_cache_destroy (s_wb);
    lru_cache_destroy (tr_wb);
    miss_filter_destroy(miss_filter);
    miss_table_destroy(miss_table);
    ghost_cache_destroy(wb_ghost_cache);
    lru_cache_destroy(ssd_cache);

    return 0;

error:
    if (trace_fp != NULL) {
        fclose(trace_fp);
    }

    if (out_fp != NULL) {
        fclose(out_fp);
    }

    if (debug_fp != NULL) {
        fclose(debug_fp);
    }

    if (s_wb != NULL) {
        lru_cache_destroy (s_wb);
    }

    if (tr_wb != NULL) {
        lru_cache_destroy (tr_wb);
    }

    if (miss_filter != NULL) {
        miss_filter_destroy(miss_filter);
    }

    if (miss_table != NULL) {
        miss_table_destroy(miss_table);
    }

    if (wb_ghost_cache != NULL) {
        ghost_cache_destroy(wb_ghost_cache);
    }

    if (ssd_cache != NULL) {
        lru_cache_destroy(ssd_cache);
    }

    return -1;
}

int main(int argc, char *argv[]) {
    int ret = 0;
    ConfigInfo *config_info;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    /* memory allocation */
    config_info = (ConfigInfo *) calloc(1, sizeof(ConfigInfo));
    check(config_info!=NULL, "failed to allocate memory for config_info.");
    strncpy (config_info->trace_file, "./ensemble_trace.csv", FILE_LINE_SIZE);
    config_info->miss_filter_size = 500000000;
    config_info->miss_filter_threshold = 9;
    config_info->miss_table_lookup_size = 5000000;
    config_info->miss_table_threshold = 4;
    config_info->ssd_size = (uint64_t)12 * 1024 * 1024 *1024;
    config_info->ssd_size = (config_info->ssd_size >> LOG_2_BLOCK_SIZE);
    config_info->num_sub_windows = 4;

    /* parse configuration file */
/*    ret = parse_config_file(argv[1], config_info);
    check(ret==0, "failed to parse configuration file: %s", argv[1]);*/

    if (atoi(argv[1]) == 0) {
        if (argc != 5) {
            printf("base line sieve store: ./%s 0 num_sub_window miss_filter_threshold miss_table_threshold\n", argv[0]);
            return 1;
        }
        config_info->num_sub_windows = atoi(argv[2]);
        config_info->miss_filter_threshold = atoi(argv[3]);
        config_info->miss_table_threshold = atoi(argv[4]);
        sprintf(config_info->result_file, "./results/ss_%"PRIu8"_%"PRIu32"_%"PRIu32".out",
                config_info->num_sub_windows, config_info->miss_filter_threshold,
                config_info->miss_table_threshold);

        ret = run_sieve_store_base(config_info);
        check(ret==0, "failed to run sieve store base.");
    } else if (atoi(argv[1]) == 1) {
        if (argc != 6) {
            printf("traditional write buffer: ./%s 1 tr_wb_size num_sub_window miss_filter_threshold miss_table_threshold\n", argv[0]);
            return 1;
        }
        config_info->traditional_wb_size = atoi(argv[2]);
        config_info->num_sub_windows = atoi(argv[3]);
        config_info->miss_filter_threshold = atoi(argv[4]);
        config_info->miss_table_threshold = atoi(argv[5]);
        sprintf(config_info->result_file,
                "./results/tr_wb_%"PRIu64"_%"PRIu8"_%"PRIu32"_%"PRIu32".out",
                config_info->traditional_wb_size,
                config_info->num_sub_windows,
                config_info->miss_filter_threshold,
                config_info->miss_table_threshold);

        ret = run_traditional_write_buffer(config_info);
        check(ret==0, "failed to run traditional write buffer.");
    } else if (atoi(argv[1]) == 2) {
        if (argc != 4) {
            printf("sieved write buffer: ./%s 2 s_wb_threshold s_wb_size\n", argv[0]);
        }
        config_info->sieved_ghost_cache_size = 409600;
        config_info->sieved_wb_threshold = atoi(argv[2]);
        config_info->sieved_wb_size = atoi(argv[3]);
        sprintf(config_info->result_file, "./results/s_wb_%"PRIu32"_%"PRIu64".out",
                config_info->sieved_wb_threshold, config_info->sieved_wb_size);

        ret = run_sieved_write_buffer (config_info);
        check (ret==0, "failed to run sieved write buffer.");
    } else if (atoi(argv[1]) == 3) {
        if (argc != 6) {
            printf("\t sieved + traditional write buffer: %s 3 "
                    "s_wb_threshold num_time_windows miss_filter_threshold "
                    "miss_table_threshold\n", argv[0]);
        }
        config_info->sieved_ghost_cache_size = 409600;
        config_info->sieved_wb_size = 51200;
        config_info->traditional_wb_size = 51200;
        config_info->sieved_wb_threshold = atoi(argv[2]);
		config_info->num_sub_windows = atoi(argv[3]);
        config_info->miss_filter_threshold = atoi(argv[4]);
        config_info->miss_table_threshold = atoi(argv[5]);
		sprintf(config_info->result_file,
		        "./results/s_tr_wb_%"PRIu32"_%"PRIu8"_%"PRIu32"_%"PRIu32".out",
                config_info->sieved_wb_threshold, config_info->num_sub_windows,
                config_info->miss_filter_threshold,
                config_info->miss_table_threshold);
        sprintf(config_info->debug_file,
                "./debug/s_tr_wb_%"PRIu32"_%"PRIu8"_%"PRIu32"_%"PRIu32".out",
                config_info->sieved_wb_threshold, config_info->num_sub_windows,
                config_info->miss_filter_threshold,
                config_info->miss_table_threshold);
        //config_info->sieved_wb_threshold = atoi(argv[2]);
        //config_info->sieved_wb_size = atoi(argv[3]);
        //config_info->traditional_wb_size = atoi(argv[4]);
        /*sprintf(config_info->result_file, "./results/s_tr_wb_%"PRIu32"_%"PRIu64"_%"PRIu64".out",
                config_info->sieved_wb_threshold, config_info->sieved_wb_size,
                config_info->traditional_wb_size);
				*/

        ret = run_sieved_plus_traditional_wb (config_info);
        check(ret == 0, "failed to run sieved plus traditional write buffer.");
    }

    /* memory deallocation*/
    free(config_info);

    return 0;

error:

    if (config_info != NULL) {
        free(config_info);
    }
    return -1;
}
