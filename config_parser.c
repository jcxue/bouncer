#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "debug.h"
#include "common.h"
#include "config_parser.h"

uint32_t *parse_buffer_size_str(char *buffer_size_str, uint32_t *num_sizes)
{
    int       num_digits = 0;
    int       ind        = 0;
    char      *i         = buffer_size_str;
    uint32_t  *buffer_sizes;

    (*num_sizes) = 1;
    while (*i != 0) {
        if (*i == ':') {
            (*num_sizes) += 1;
        }
        i += 1;
    }

    buffer_sizes = (uint32_t *) calloc((*num_sizes), sizeof(uint32_t));
    check(buffer_sizes!=NULL, "failed to allocate memory for buffer_sizes.");

    i = buffer_size_str;
    while (*i != 0) {
        if (*i == ':') {
            check(num_digits!=0, "empty buffer size string.");
            check(buffer_sizes[ind]>0, "non valid buffer size: %"PRIu32".",
                    buffer_sizes[ind]);
            ind += 1;
            num_digits = 0;
            i += 1;
            continue;
        }

        if ((*i >= '0') && (*i <= '9')) {
            buffer_sizes[ind] = (buffer_sizes[ind] * 10) + (*i - '0');
            num_digits += 1;
        } else {
            check(0, "non digit character:%c.", *i);
        }

        i += 1;
    }

    return buffer_sizes;
error:
    return 0;
}

void clean_up_line (char *line)
{
    char *i = line;
    char *j = line;

    while (*j != 0) {
        *i = *j;
        j += 1;
        if (*i != ' ' && *i != '\t' && *i != '\r' && *i != '\n') {
            i += 1;
        }
    }
    *i = 0;
}

int parse_int (char *line)
{
    char *i = line;
    int  val = 0;
    int  start = 0;
    int  num_digits = 0;

    while (*i != 0) {
        if (*i == ':') {
            val = 0;
            start = 1;
            i += 1;
            continue;
        }

        if (start == 1) {
            if ((*i >= '0') && (*i <= '9')) {
                val = (val * 10) + (*i - '0');
                num_digits += 1;
            } else {
                check(0, "non digit character:%c.", *i);
            }
        }

        i += 1;
    }

    check(num_digits!=0, "empty string.");
    check(val>=0, "non valid val: %d.", val);

    return val;

error:

    return -1;
}

int get_trace_file_path (char *line, char *trace_file)
{
    char *i = line;
    int   ind = 0;

    while (*i != ':') {
        ind += 1;
        i += 1;
    }

    strncpy (trace_file, line+ind+1, FILE_LINE_SIZE);

    return 0;
}

int parse_config_file (char *config_file, ConfigInfo *config_info)
{
    FILE *fp = NULL;
    char  line[FILE_LINE_SIZE];
    int   ret = 0;

    fp = fopen (config_file, "r");
    check (fp!=NULL, "failed to open configuration file: %s", config_file);

    while (fgets(line, sizeof(line), fp) != NULL) {
        /* skip through comments */
        if (strstr(line, "#") != NULL) {
            continue;
        }

        /* remove tab, space, '\n', '\r' from the line */
        clean_up_line(line);

        if (strstr(line, "trace_file:") != NULL) {
            ret = get_trace_file_path (line, config_info->trace_file);
            check (ret!=-1, "failed to get trace file path.");
        } else if (strstr(line, "miss_filter_size:") != NULL) {
            config_info->miss_filter_size = parse_int (line);
            check (config_info->miss_filter_size!=-1,
                                "failed to parse miss_filter_size");
        } else if (strstr(line, "miss_filter_threshold:") != NULL) {
            config_info->miss_filter_threshold = parse_int(line);
            check(config_info->miss_filter_threshold!=-1,
                    "failed to parse miss_filter_threshold");
        } else if (strstr(line, "miss_table_lookup_size:") != NULL) {
            config_info->miss_table_lookup_size = parse_int(line);
            check(config_info->miss_table_lookup_size!=-1,
                    "failed to parse miss_table_lookup_size");
        } else if (strstr(line, "miss_table_threshold:") != NULL) {
            config_info->miss_table_threshold = parse_int(line);
            check(config_info->miss_table_threshold!=-1,
                    "failed to parse miss_table_threshold");
        } else if (strstr(line, "ssd_size_in_gig_bytes:") != NULL) {
            config_info->ssd_size = parse_int(line);
            check(config_info->ssd_size!=-1,
                    "failed to parse ssd_size_in_gig_bytes.");
            config_info->ssd_size = (config_info->ssd_size << 30);
            config_info->ssd_size = (config_info->ssd_size >> LOG_2_BLOCK_SIZE);
        } else if (strstr(line, "num_sub_windows:") != NULL)  {
            config_info->num_sub_windows = parse_int(line);
            check(config_info->num_sub_windows!=-1,
                    "failed to parse num_sub_windows");
            check(config_info->num_sub_windows<=12,
                    "cannot have more than 12 sub windows.");
        } else if (strstr(line, "test_type:") != NULL) {
            config_info->test_type = parse_int (line);
            check (config_info->test_type>=0, "failed to parse test_type.");
        }
    }

/*    printf("trace_file: %s\n", config_info->trace_file);
    printf("bouncer_cache_size: %"PRIu32"\n", config_info->bouncer_cache_size);
    printf("miss_filter_size: %"PRIu32"\n", config_info->miss_filter_size);
    printf("bloom_filter_threshold: %"PRIu8"\n",
            config_info->miss_filter_threshold);
    printf("miss_table_size: %"PRIu32"\n", config_info->miss_table_size);
    printf("miss_table_threshold: %"PRIu8"\n",
            config_info->miss_table_threshold);
    printf("ssd_size: %"PRIu64"\n",
            config_info->ssd_size);*/

    fclose (fp);
    fp = NULL;
    return 0;

error:

    if (fp != NULL) {
        fclose (fp);
        fp = NULL;
    }
    return -1;
}
