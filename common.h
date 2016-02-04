#ifndef COMMON_H_
#define COMMON_H_

#include <limits.h>
#include <inttypes.h>

#define FILE_LINE_SIZE              256
#define MISS_FILTER_COUNTER_MAX     SCHAR_MAX
//#define TIME_WINDOW_SIZE            288000000000  //assuming 8 hours per window
#define SUB_WINDOW_SIZE             72000000000   //assuming 2 hours sub window
//#define SUB_WINDOW_PER_WINDOW       4
#define LOG_2_BLOCK_SIZE            12            //assuming 4K block size
/*
#define STATIC_SSD_SIZE             1340628
#define STATIC_WB_SIZE              51200
#define NUM_REQS                    2913999581
*/
#define STATIC_SSD_SIZE             3541
#define STATIC_WB_SIZE              512
#define NUM_REQS                    4172539

typedef struct CSVLineData {
    uint64_t timestamp;
    uint8_t  server_num;
    uint8_t  volume_num;
    char     req_type[5];
    uint64_t start_addr;
    uint32_t req_size;
    uint64_t duration;
} CSVLineData;

typedef struct Request {
    uint64_t block_num;
    uint8_t  server_num;
    uint8_t  volume_num;
    uint8_t  req_type;  // 0 - read; 1 - write;
    uint32_t sub_window_ind;
} Request;

typedef struct ReplayReq {
    uint64_t block_num;
    uint8_t  server_num;
    uint8_t  volume_num;
    uint8_t  req_type;  // 0 - read; 1 - write;
} ReplayReq __attribute__((aligned(16)));

#endif
