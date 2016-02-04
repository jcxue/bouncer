#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/time.h>

#include "common.h"
#include "debug.h"

#define NUM_UNIQUE_ADDRS   1048576
#define NUM_WB_SLOTS       10
#define SIEVE_THRESHOLD    7
#define MAX_BLOCKS_PER_REQ 16

int main (int argc, char *argv)
{
    uint64_t block_ind, num_lines = 0, num_reqs = 0;
    uint8_t req_size, req_type, done = NUM_WB_SLOTS;
    uint8_t access_cnt[NUM_UNIQUE_ADDRS];
    uint64_t wb_block_ind[NUM_WB_SLOTS];
    uint32_t i, j, flag1, flag2, flag3;

    FILE *fp = NULL;
    fp = fopen ("test_10.csv", "w");
    check (fp!=NULL, "failed to create output trace file.");

    srand(time(NULL));

    /* generate block inds that will have exactly 7 accesses */
    for (i = 0; i < NUM_WB_SLOTS; i++) {
        wb_block_ind[i] = (rand() % NUM_UNIQUE_ADDRS);
    }

    /* initialize access_cnt array */
    for (i = 0; i < NUM_UNIQUE_ADDRS; i++) {
        access_cnt[i] = 0;
    }

    while (done != 0) {
        uint32_t num_trials = 0;
        do {
            num_trials += 1;
            flag3 = 0;
            flag1 = 1;
            block_ind = (rand() % NUM_UNIQUE_ADDRS);
            if (num_trials > 20) {
                req_size = 1;
                block_ind = wb_block_ind[rand() % NUM_WB_SLOTS];
            } else {
                req_size = (rand() % 16) + 1;
            }
            for (i = 0; i < req_size; i++) {
                uint64_t tmp_ind = block_ind + i;
                if (tmp_ind >= NUM_UNIQUE_ADDRS) {
                    flag1 = 0;
                    //printf("ind out of range.\n");
                    break;
                }
                for (j = 0; j < NUM_WB_SLOTS; j++) {
                    if (tmp_ind == wb_block_ind[j]) {
                        flag3 = 1;
                        break;
                    }
                }
                if (access_cnt[tmp_ind] >= (SIEVE_THRESHOLD-1)) {
                    flag2 = 0;
                    for (j = 0; j < NUM_WB_SLOTS; j++) {
                        if (tmp_ind == wb_block_ind[j]) {
                            if (access_cnt[tmp_ind] == (SIEVE_THRESHOLD-1)) {
                                flag2 = 1;
                                break;
                            }
                        }
                    }
                    if (flag2 == 0) {
                        //printf("threshold overflow.\n");
                        flag1 = 0;
                        break;
                    }
                }
            }
        } while (flag1 == 0);

        if (flag3 == 1) {
            fprintf (fp, "128166372002905282      7       0       %s    %"PRIu64"      %d   67254\n",
                    "Write", (block_ind<<12), (req_size<<12));
        } else {
            if ((rand() % 3) == 0) {
                fprintf (fp, "128166372002905282      7       0       %s    %"PRIu64"      %d   67254\n",
                                    "Write", (block_ind<<12), (req_size<<12));
            } else {
                fprintf (fp, "128166372002905282      7       0       %s    %"PRIu64"      %d   67254\n",
                                    "Read", (block_ind<<12), (req_size<<12));
            }
        }

        num_lines += 1;
        //printf ("num_lines = %"PRIu64"\n", num_lines);

        for (i = 0; i < req_size; i++) {
            access_cnt[block_ind + i] += 1;
            if (access_cnt[block_ind + i] == SIEVE_THRESHOLD) {
                done -= 1;
            }
            num_reqs += 1;
        }
    }

    /* print the last line without line return */
    do {
        flag1 = 1;
        block_ind = (rand() % NUM_UNIQUE_ADDRS);
        if (access_cnt[block_ind] < (SIEVE_THRESHOLD-1)) {
            access_cnt[block_ind] += 1;
            fprintf (fp, "128166372002905282      7       0       Read    %"PRIu64"      4096   67254", (block_ind<<12));
        } else {
            flag1 = 0;
        }
    } while (flag1 == 0);

    fclose (fp);
    fp = NULL;

    /* verify generated trace file */
    printf ("verifying generated trace file ...\n");
    FILE *in = fopen ("test_10.csv", "r");
    check(in!=NULL, "failed to open trace file.")
    char line[256];
    CSVLineData csv_data;
    int ret = 0;
    uint32_t local_count[NUM_UNIQUE_ADDRS];
    for (i = 0; i < NUM_UNIQUE_ADDRS; i++) {
        local_count[i] = 0;
    }

    while (fgets(line, sizeof(line), in) != NULL) {
            ret = sscanf(line, "%"SCNu64" %"SCNu8" %"SCNu8" %s %"SCNu64" %"SCNu32" "
            "%"SCNu64"\n", &csv_data.timestamp, &csv_data.server_num,
                    &csv_data.volume_num, csv_data.req_type, &csv_data.start_addr,
                    &csv_data.req_size, &csv_data.duration);
            check(ret==7, "failed to parse csv line:\n\t%s\n", line);

            req_size = (csv_data.req_size >> 12);
            for (i = 0; i < req_size; i++) {
                block_ind = (csv_data.start_addr >> 12) + i;
                check (block_ind<NUM_UNIQUE_ADDRS, "block_ind out of range:\n\t%s.", line);
                local_count[block_ind] += 1;
                check (local_count[block_ind]<=SIEVE_THRESHOLD,
                        "access count out of range.");
            }
    }

    fclose (in);

    for (i = 0; i < NUM_UNIQUE_ADDRS; i++) {
        if (local_count[i] == SIEVE_THRESHOLD) {
            flag1 = 0;
            for (j = 0; j < NUM_WB_SLOTS; j++) {
                if (wb_block_ind[j] == i) {
                    flag1 = 1;
                    break;
                }
            }
            check (flag1==1, "access count exceeds threshold.");
        }
        if (access_cnt[i] != local_count[i]) {
            check (0, "access count does not match.");
        }
    }

/*    printf ("writing stats file.\n");

    FILE *stats_fp = fopen ("test_10_stats.txt", "w");
    check (stats_fp != NULL, "failed to open stats file.");

    for (i = 0; i < (NUM_UNIQUE_ADDRS-1); i++) {
        fprintf(stats_fp, "%"PRIu64"\t%"PRIu8"\n", (i << 12), access_cnt[i]);
    }
    fprintf(stats_fp, "%"PRIu64"\t%"PRIu8"", (i << 12), access_cnt[i]);

    fclose (stats_fp);*/

    return 0;

error:

    if (fp != NULL) {
        fclose (fp);
    }

    if (in != NULL) {
        fclose (in);
    }

/*    if (stats_fp != NULL) {
        fclose (stats_fp);
    }*/

    return -1;
}
