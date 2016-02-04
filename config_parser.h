#ifndef CONFIG_PARSER_H_
#define CONFIG_PARSER_H_

#include "common.h"

typedef struct ConfigInfo {
    char      trace_file[FILE_LINE_SIZE];
    char      result_file[FILE_LINE_SIZE];
    char      debug_file[FILE_LINE_SIZE];
    uint64_t  traditional_wb_size;
    uint64_t  sieved_wb_size;
    uint32_t  sieved_ghost_cache_size;
    uint32_t  sieved_wb_threshold;
    uint64_t  miss_filter_size;
    uint32_t  miss_filter_threshold;
    uint64_t  miss_table_lookup_size;
    uint32_t  miss_table_threshold;
    uint64_t  ssd_size;
    uint8_t   num_sub_windows;
    uint8_t   test_type;
} ConfigInfo;

int parse_config_file (char *config_file, ConfigInfo *config_info);

#endif
