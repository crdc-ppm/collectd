/**
 * collectd - src/ceph.c
 * Copyright (C) 2011  New Dream Network
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Colin McCabe <cmccabe@alumni.cmu.edu>
 *   Dennis Zou <yunzou@cisco.com>
 *   Dan Ryder <daryder@cisco.com>
 **/

#define _BSD_SOURCE

#include "collectd.h"
#include "common.h"
#include "plugin.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <yajl/yajl_parse.h>
#if HAVE_YAJL_YAJL_VERSION_H
#include <yajl/yajl_version.h>
#endif

#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <math.h>
#include <inttypes.h>

#define MAX_RRD_DS_NAME_LEN 20

#define RETRY_AVGCOUNT -1

#if defined(YAJL_MAJOR) && (YAJL_MAJOR > 1)
# define HAVE_YAJL_V2 1
#endif

#define RETRY_ON_EINTR(ret, expr) \
    while(1) { \
        ret = expr; \
        if(ret >= 0) \
            break; \
        ret = -errno; \
        if(ret != -EINTR) \
            break; \
    }

/** Timeout interval in seconds */
#define CEPH_TIMEOUT_INTERVAL 1

/** Maximum path length for a UNIX domain socket on this system */
#define UNIX_DOMAIN_SOCK_PATH_MAX (sizeof(((struct sockaddr_un*)0)->sun_path))

#define CEPH_ASOK_REQ_PRE "{ \"prefix\": \""
#define CEPH_ASOK_REQ_POST "\" }\n"
#define CEPH_FSID_REQ "config get\",\"var\": \"fsid"
#define FSID_STRING_LEN 37

#define CEPH_DAEMON_TYPES_NUM 3
const char * ceph_daemon_types [CEPH_DAEMON_TYPES_NUM] =
                { "osd", "mon", "mds" };

/** Yajl callback returns */
#define CEPH_CB_CONTINUE 1
#define CEPH_CB_ABORT 0

#if HAVE_YAJL_V2
typedef size_t yajl_len_t;
#else
typedef unsigned int yajl_len_t;
#endif

/******* ceph_daemon *******/
struct ceph_daemon
{
    /** Version of the admin_socket interface */
    uint32_t version;
    /** daemon name **/
    char name[DATA_MAX_NAME_LEN];

    /** cluster fsid **/
    char fsid[FSID_STRING_LEN];

    /** cluster name **/
    char cluster[DATA_MAX_NAME_LEN];

    int dset_num;

    /** Path to the socket that we use to talk to the ceph daemon */
    char asok_path[UNIX_DOMAIN_SOCK_PATH_MAX];

    /** The set of  key/value pairs that this daemon reports
     * dset.type        The daemon name
     * dset.ds_num      Number of data sources (key/value pairs) 
     * dset.ds      Dynamically allocated array of key/value pairs
     */
    /** Dynamically allocated array **/
    struct data_set_s *dset;
    int **pc_types;
};

/******* JSON parsing *******/
typedef int (*node_handler_t)(void *, const char*, const char*);

/** Track state and handler while parsing JSON */
struct yajl_struct
{
    node_handler_t handler;
    void * handler_arg;
    struct {
      char key[DATA_MAX_NAME_LEN];
      int key_len;
    } state[YAJL_MAX_DEPTH];
    int depth;
};
typedef struct yajl_struct yajl_struct;

/**
 * Keep track of last data for latency values so we can calculate rate
 * since last poll.
 */
struct last_data **last_poll_data = NULL;
int last_idx = 0;

enum perfcounter_type_d
{
    PERFCOUNTER_LATENCY = 0x4, PERFCOUNTER_DERIVE = 0x8,
};

/** Give user option to use default (long run = since daemon started) avg */
static int long_run_latency_avg = 0;

/**
 * Give user option to use default type for special cases -
 * filestore.journal_wr_bytes is currently only metric here. Ceph reports the
 * type as a sum/count pair and will calculate it the same as a latency value.
 * All other "bytes" metrics (excluding the used/capacity bytes for the OSD)
 * use the DERIVE type. Unless user specifies to use given type, convert this
 * metric to use DERIVE.
 */
static int convert_special_metrics = 1;

/** Array of daemons to monitor */
static struct ceph_daemon **g_daemons = NULL;

/** Number of elements in g_daemons */
static int g_num_daemons = 0;

struct values_holder
{
    int values_len;
    value_t *values;
};

/**
 * A set of values_t data that we build up in memory while parsing the JSON.
 */
struct values_tmp
{
    struct ceph_daemon *d;
    int holder_num;
    struct values_holder vh[0];
    uint64_t avgcount;
};

/**
 * A set of count/sum pairs to keep track of latency types and get difference
 * between this poll data and last poll data.
 */
struct last_data
{
    char dset_name[DATA_MAX_NAME_LEN];
    char ds_name[MAX_RRD_DS_NAME_LEN];
    double last_sum;
    uint64_t last_count;
};


/******* network I/O *******/
enum cstate_t
{
    CSTATE_UNCONNECTED = 0,
    CSTATE_WRITE_REQUEST,
    CSTATE_READ_VERSION,
    CSTATE_READ_AMT,
    CSTATE_READ_JSON,
};

enum request_type_t
{
    ASOK_REQ_VERSION = 0,
    ASOK_REQ_DATA = 1,
    ASOK_REQ_SCHEMA = 2,
    ASOK_REQ_FSID = 3,
    ASOK_REQ_NONE = 1000,
};

struct cconn
{
    /** The Ceph daemon that we're talking to */
    struct ceph_daemon *d;

    /** Request type */
    uint32_t request_type;

    /** The connection state */
    enum cstate_t state;

    /** The socket we use to talk to this daemon */
    int asok;

    /** The amount of data remaining to read / write. */
    uint32_t amt;

    /** Length of the JSON to read */
    uint32_t json_len;

    /** Buffer containing JSON data */
    unsigned char *json;

    /** Keep data important to yajl processing */
    struct yajl_struct yajl;
};

static int ceph_cb_null(void *ctx)
{
    return CEPH_CB_CONTINUE;
}

static int ceph_cb_boolean(void *ctx, int bool_val)
{
    return CEPH_CB_CONTINUE;
}

static int 
ceph_cb_number(void *ctx, const char *number_val, yajl_len_t number_len)
{
    yajl_struct *yajl = (yajl_struct*)ctx;
    char buffer[number_len+1];
    int i, latency_type = 0, result;
    char key[128];

    memcpy(buffer, number_val, number_len);
    buffer[sizeof(buffer) - 1] = 0;

    ssnprintf(key, yajl->state[0].key_len, "%s", yajl->state[0].key);
    for(i = 1; i < yajl->depth; i++)
    {
        if((i == yajl->depth-1) && ((strcmp(yajl->state[i].key,"avgcount") == 0)
                || (strcmp(yajl->state[i].key,"sum") == 0)))
        {
            if(convert_special_metrics)
            {
                /**
                 * Special case for filestore:JournalWrBytes. For some reason,
                 * Ceph schema encodes this as a count/sum pair while all
                 * other "Bytes" data (excluding used/capacity bytes for OSD
                 * space) uses a single "Derive" type. To spare further
                 * confusion, keep this KPI as the same type of other "Bytes".
                 * Instead of keeping an "average" or "rate", use the "sum" in
                 * the pair and assign that to the derive value.
                 */
                if((strcmp(yajl->state[i-1].key, "journal_wr_bytes") == 0) &&
                        (strcmp(yajl->state[i-2].key,"filestore") == 0) &&
                        (strcmp(yajl->state[i].key,"avgcount") == 0))
                {
                    DEBUG("Skipping avgcount for filestore.JournalWrBytes");
                    yajl->depth = (yajl->depth - 1);
                    return CEPH_CB_CONTINUE;
                }
            }
            //probably a avgcount/sum pair. if not - we'll try full key later
            latency_type = 1;
            break;
        }
        strncat(key, ".", 1);
        strncat(key, yajl->state[i].key, yajl->state[i].key_len+1);
    }

    result = yajl->handler(yajl->handler_arg, buffer, key);

    if((result == RETRY_AVGCOUNT) && latency_type)
    {
        strncat(key, ".", 1);
        strncat(key, yajl->state[yajl->depth-1].key,
                yajl->state[yajl->depth-1].key_len+1);
        result = yajl->handler(yajl->handler_arg, buffer, key);
    }

    if(result == -ENOMEM)
    {
        ERROR("ceph plugin: memory allocation failed");
        return CEPH_CB_ABORT;
    }

    yajl->depth = (yajl->depth - 1);
    return CEPH_CB_CONTINUE;
}

static int ceph_cb_string(void *ctx, const unsigned char *string_val, 
        yajl_len_t string_len)
{
    yajl_struct *yajl = (yajl_struct*)ctx;
    if((yajl->depth != 1) || (strcmp(yajl->state[0].key,"fsid") != 0) ||
                                            (string_len != (FSID_STRING_LEN-1)))
    {
        /** this is not FSID - ignore it */
        DEBUG("yajl ceph_cb_string, ignoring %s", string_val);
        if(yajl->depth > 0)
        {
            yajl->depth = (yajl->depth -1);
        }
        return CEPH_CB_CONTINUE;
    }
    char buffer[string_len+1];
    memcpy(buffer, string_val, string_len);
    buffer[sizeof(buffer) - 1] = 0;
    char key[128];
    ssnprintf(key, yajl->state[0].key_len, "%s", yajl->state[0].key);

    yajl->handler(yajl->handler_arg, buffer, key);
    yajl->depth = (yajl->depth - 1);
    return CEPH_CB_CONTINUE;
}

static int ceph_cb_start_map(void *ctx)
{
    return CEPH_CB_CONTINUE;
}

static int
ceph_cb_map_key(void *ctx, const unsigned char *key, yajl_len_t string_len)
{
    yajl_struct *yajl = (yajl_struct*)ctx;

    if((yajl->depth+1)  >= YAJL_MAX_DEPTH)
    {
        ERROR("ceph plugin: depth exceeds max, aborting.");
        return CEPH_CB_ABORT;
    }

    char buffer[string_len+1];

    memcpy(buffer, key, string_len);
    buffer[sizeof(buffer) - 1] = 0;

    snprintf(yajl->state[yajl->depth].key, sizeof(buffer), "%s", buffer);
    yajl->state[yajl->depth].key_len = sizeof(buffer);
    yajl->depth = (yajl->depth + 1);

    return CEPH_CB_CONTINUE;
}

static int ceph_cb_end_map(void *ctx)
{
    yajl_struct *yajl = (yajl_struct*)ctx;

    yajl->depth = (yajl->depth - 1);
    return CEPH_CB_CONTINUE;
}

static int ceph_cb_start_array(void *ctx)
{
    return CEPH_CB_CONTINUE;
}

static int ceph_cb_end_array(void *ctx)
{
    return CEPH_CB_CONTINUE;
}

static yajl_callbacks callbacks = {
        ceph_cb_null,
        ceph_cb_boolean,
        NULL,
        NULL,
        ceph_cb_number,
        ceph_cb_string,
        ceph_cb_start_map,
        ceph_cb_map_key,
        ceph_cb_end_map,
        ceph_cb_start_array,
        ceph_cb_end_array
};

static void ceph_daemon_print(const struct ceph_daemon *d)
{
    DEBUG("name=%s, asok_path=%s", d->name, d->asok_path);
}

static void ceph_daemons_print(void)
{
    int i;
    for(i = 0; i < g_num_daemons; ++i)
    {
        ceph_daemon_print(g_daemons[i]);
    }
}

static void ceph_daemon_free(struct ceph_daemon *d)
{
    int i = 0;
    for(; i < d->dset_num; i++)
    {
        plugin_unregister_data_set((d->dset + i)->type);
        sfree(d->dset->ds);
        sfree(d->pc_types[i]);
    }
    sfree(d->dset);
    sfree(d->pc_types);
    sfree(d);
}

static void compact_ds_name(char *source, char *dest)
{
    int keys_num = 0, i;
    char *save_ptr = NULL, *tmp_ptr = source;
    char *keys[16];
    char len_str[3];
    char tmp[DATA_MAX_NAME_LEN];
    int reserved = 0;
    int offset = 0;
    memset(tmp, 0, sizeof(tmp));
    if(source == NULL || dest == NULL || source[0] == '\0' || dest[0] != '\0')
    {
        return;
    }
    size_t src_len = strlen(source);
    snprintf(len_str, sizeof(len_str), "%zu", src_len);
    unsigned char append_status = 0x0;
    append_status |= (source[src_len - 1] == '-') ? 0x1 : 0x0;
    append_status |= (source[src_len - 1] == '+') ? 0x2 : 0x0;
    while ((keys[keys_num] = strtok_r(tmp_ptr, ":_-+", &save_ptr)) != NULL)
    {
        tmp_ptr = NULL;
        /** capitalize 1st char **/
        keys[keys_num][0] = toupper(keys[keys_num][0]);
        keys_num++;
        if(keys_num >= 16)
        {
            break;
        }
    }
    /** concatenate each part of source string **/
    for(i = 0; i < keys_num; i++)
    {
        strcat(tmp, keys[i]);
    }
    tmp[DATA_MAX_NAME_LEN - 1] = '\0';
    /** to coordinate limitation of length of ds name from RRD
     *  we will truncate ds_name
     *  when the its length is more than
     *  MAX_RRD_DS_NAME_LEN
     */
    if(strlen(tmp) > MAX_RRD_DS_NAME_LEN - 1)
    {
        append_status |= 0x4;
        /** we should reserve space for
         * len_str
         */
        reserved += 2;
    }
    if(append_status & 0x1)
    {
        /** we should reserve space for
         * "Minus"
         */
        reserved += 5;
    }
    if(append_status & 0x2)
    {
        /** we should reserve space for
         * "Plus"
         */
        reserved += 4;
    }
    snprintf(dest, MAX_RRD_DS_NAME_LEN - reserved, "%s", tmp);
    offset = strlen(dest);
    switch (append_status)
    {
        case 0x1:
            memcpy(dest + offset, "Minus", 5);
            break;
        case 0x2:
            memcpy(dest + offset, "Plus", 5);
            break;
        case 0x4:
            memcpy(dest + offset, len_str, 2);
            break;
        case 0x5:
            memcpy(dest + offset, "Minus", 5);
            memcpy(dest + offset + 5, len_str, 2);
            break;
        case 0x6:
            memcpy(dest + offset, "Plus", 4);
            memcpy(dest + offset + 4, len_str, 2);
            break;
        default:
            break;
    }
}
static int parse_keys(const char *key_str, char *dset_name, char *ds_name)
{
    char *ptr, *rptr;
    size_t dset_name_len = 0;
    size_t ds_name_len = 0;
    char tmp_ds_name[DATA_MAX_NAME_LEN];
    memset(tmp_ds_name, 0, sizeof(tmp_ds_name));
    if(dset_name == NULL || ds_name == NULL || key_str == NULL ||
            key_str[0] == '\0' || dset_name[0] != '\0' || ds_name[0] != '\0')
    {
        return -1;
    }
    if((ptr = strchr(key_str, '.')) == NULL
            || (rptr = strrchr(key_str, '.')) == NULL)
    {
        strncpy(dset_name, key_str, DATA_MAX_NAME_LEN - 1);
        strncpy(tmp_ds_name, key_str, DATA_MAX_NAME_LEN - 1);
        goto compact;
    }
    dset_name_len =
            (ptr - key_str) > (DATA_MAX_NAME_LEN - 1) ?
                    (DATA_MAX_NAME_LEN - 1) : (ptr - key_str);
    memcpy(dset_name, key_str, dset_name_len);
    ds_name_len =
           (rptr - ptr) > DATA_MAX_NAME_LEN ? DATA_MAX_NAME_LEN : (rptr - ptr);
    if(ds_name_len == 0)
    { /** only have two keys **/
        if(!strncmp(rptr + 1, "type", 4))
        {/** if last key is "type",ignore **/
            strncpy(tmp_ds_name, dset_name, DATA_MAX_NAME_LEN - 1);
        }
        else
        {/** if last key isn't "type", copy last key **/
            strncpy(tmp_ds_name, rptr + 1, DATA_MAX_NAME_LEN - 1);
        }
    }
    else if(!strncmp(rptr + 1, "type", 4))
    {/** more than two keys **/
        memcpy(tmp_ds_name, ptr + 1, ds_name_len - 1);
    }
    else
    {/** copy whole keys **/
        strncpy(tmp_ds_name, ptr + 1, DATA_MAX_NAME_LEN - 1);
    }
    compact: compact_ds_name(tmp_ds_name, ds_name);
    return 0;
}

static int get_matching_dset(const struct ceph_daemon *d, const char *name)
{
    int idx;
    for(idx = 0; idx < d->dset_num; ++idx)
    {
        if(strcmp(d->dset[idx].type, name) == 0)
        {
            return idx;
        }
    }
    return -1;
}

static int get_matching_value(const struct data_set_s *dset, const char *name,
        int num_values)
{
    int idx;
    for(idx = 0; idx < num_values; ++idx)
    {
        if(strcmp(dset->ds[idx].name, name) == 0)
        {
            return idx;
        }
    }
    return -1;
}

static int ceph_daemon_add_ds_entry(struct ceph_daemon *d, const char *name,
        int pc_type)
{
    struct data_source_s *ds;
    struct data_set_s *dset;
    struct data_set_s *dset_array;
    int **pc_types_array = NULL;
    int *pc_types;
    int *pc_types_new;
    int idx = 0;
    if(strlen(name) + 1 > DATA_MAX_NAME_LEN)
    {
        return -ENAMETOOLONG;
    }
    char dset_name[DATA_MAX_NAME_LEN];
    char ds_name[MAX_RRD_DS_NAME_LEN];
    memset(dset_name, 0, sizeof(dset_name));
    memset(ds_name, 0, sizeof(ds_name));
    if(parse_keys(name, dset_name, ds_name))
    {
        return 1;
    }
    idx = get_matching_dset(d, dset_name);
    if(idx == -1)
    {/* need to add a dset **/
        dset_array = realloc(d->dset,
                sizeof(struct data_set_s) * (d->dset_num + 1));
        if(!dset_array)
        {
            return -ENOMEM;
        }
        pc_types_array = realloc(d->pc_types,
                sizeof(int *) * (d->dset_num + 1));
        if(!pc_types_array)
        {
            return -ENOMEM;
        }
        dset = &dset_array[d->dset_num];
        /** this step is very important, otherwise,
         *  realloc for dset->ds will tricky because of
         *  a random addr in dset->ds
         */
        memset(dset, 0, sizeof(struct data_set_s));
        dset->ds_num = 0;
        snprintf(dset->type, DATA_MAX_NAME_LEN, "%s", dset_name);
        pc_types = pc_types_array[d->dset_num] = NULL;
        d->dset = dset_array;
    }
    else
    {
        dset = &d->dset[idx];
        pc_types = d->pc_types[idx];
    }
    struct data_source_s *ds_array = realloc(dset->ds,
            sizeof(struct data_source_s) * (dset->ds_num + 1));
    if(!ds_array)
    {
        return -ENOMEM;
    }
    pc_types_new = realloc(pc_types, sizeof(int) * (dset->ds_num + 1));
    if(!pc_types_new)
    {
        return -ENOMEM;
    }
    dset->ds = ds_array;

    if(convert_special_metrics)
    {
        /**
         * Special case for filestore:JournalWrBytes. For some reason, Ceph
         * schema encodes this as a count/sum pair while all other "Bytes" data
         * (excluding used/capacity bytes for OSD space) uses a single "Derive"
         * type. To spare further confusion, keep this KPI as the same type of
         * other "Bytes". Instead of keeping an "average" or "rate", use the
         * "sum" in the pair and assign that to the derive value.
         */
        if((strcmp(dset_name,"filestore") == 0) &&
                                        strcmp(ds_name, "JournalWrBytes") == 0)
        {
            pc_type = 10;
        }
    }

    if(idx == -1)
    {
        pc_types_array[d->dset_num] = pc_types_new;
        d->pc_types = pc_types_array;
        d->pc_types[d->dset_num][dset->ds_num] = pc_type;
        d->dset_num++;
    }
    else
    {
        d->pc_types[idx] = pc_types_new;
        d->pc_types[idx][dset->ds_num] = pc_type;
    }
    ds = &ds_array[dset->ds_num++];
    snprintf(ds->name, MAX_RRD_DS_NAME_LEN, "%s", ds_name);
    ds->type = (pc_type & PERFCOUNTER_DERIVE) ? DS_TYPE_DERIVE : DS_TYPE_GAUGE;
            
    /**
     * Use min of 0 for DERIVE types so we don't get negative values on Ceph
     * service restart
     */
    ds->min = (ds->type == DS_TYPE_DERIVE) ? 0 : NAN;
    ds->max = NAN;
    return 0;
}

/******* ceph_config *******/
static int cc_handle_str(struct oconfig_item_s *item, char *dest, int dest_len)
{
    const char *val;
    if(item->values_num != 1)
    {
        return -ENOTSUP;
    }
    if(item->values[0].type != OCONFIG_TYPE_STRING)
    {
        return -ENOTSUP;
    }
    val = item->values[0].value.string;
    if(snprintf(dest, dest_len, "%s", val) > (dest_len - 1))
    {
        ERROR("ceph plugin: configuration parameter '%s' is too long.\n",
                item->key);
        return -ENAMETOOLONG;
    }
    return 0;
}

static int cc_handle_bool(struct oconfig_item_s *item, int *dest)
{
    if(item->values_num != 1)
    {
        return -ENOTSUP;
    }

    if(item->values[0].type != OCONFIG_TYPE_BOOLEAN)
    {
        return -ENOTSUP;
    }

    *dest = (item->values[0].value.boolean) ? 1 : 0;
    return 0;
}

static int cc_parse_cluster_name(struct ceph_daemon *cd)
{
    char search_char = '/', *tmp, asok_name[UNIX_DOMAIN_SOCK_PATH_MAX];
    int last_index_slash = -1, daemon_type_index = -1;
    size_t i;

    tmp = strchr(cd->asok_path, search_char);
    while(tmp)
    {
        last_index_slash = (tmp-(cd->asok_path+1));
        tmp = strchr(tmp+1,search_char);
    }

    if(last_index_slash == -1)
    {
        ERROR("Bad ceph socket path. Please specify the absolute path.");
        return -1;
    }

    size_t asok_name_length = (size_t)(strlen(cd->asok_path)-last_index_slash-2);

    memcpy(asok_name, &cd->asok_path[last_index_slash+2], asok_name_length);
    asok_name[asok_name_length] = '\0';

    for(i = 0; i < CEPH_DAEMON_TYPES_NUM; i++)
    {
        char * tmp_str = strstr(asok_name, ceph_daemon_types[i]);
        if(tmp_str)
        {
            daemon_type_index = (tmp_str-(asok_name+1));
            break;
        }
    }

    char * asok_str = ".asok";
    char *tmp_str = strstr(asok_name, asok_str);
    if(daemon_type_index == -1 || !tmp_str)
    {
        ERROR("Bad ceph socket path (%s). Was not an admin socket.", cd->asok_path);
        return -1;
    }
    memcpy(cd->cluster, asok_name, (size_t)daemon_type_index);
    return 0;
}

static int cc_add_daemon_config(oconfig_item_t *ci)
{
    int ret, i;
    struct ceph_daemon *array, *nd, cd;
    memset(&cd, 0, sizeof(struct ceph_daemon));

    if((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING))
    {
        WARNING("ceph plugin: `Daemon' blocks need exactly one string "
                "argument.");
        return (-1);
    }

    ret = cc_handle_str(ci, cd.name, DATA_MAX_NAME_LEN);
    if(ret)
    {
        return ret;
    }

    for(i=0; i < ci->children_num; i++)
    {
        oconfig_item_t *child = ci->children + i;

        if(strcasecmp("SocketPath", child->key) == 0)
        {
            ret = cc_handle_str(child, cd.asok_path, sizeof(cd.asok_path));
            if(ret)
            {
                return ret;
            }
            
            ret = cc_parse_cluster_name(&cd);
            if(ret)
            {
                return ret;
            }
        }
        else
        {
            WARNING("ceph plugin: ignoring unknown option %s", child->key);
        }
    }
    if(cd.name[0] == '\0')
    {
        ERROR("ceph plugin: you must configure a daemon name.\n");
        return -EINVAL;
    }
    else if(cd.asok_path[0] == '\0')
    {
        ERROR("ceph plugin(name=%s): you must configure an administrative "
        "socket path.\n", cd.name);
        return -EINVAL;
    }
    else if(!((cd.asok_path[0] == '/') ||
            (cd.asok_path[0] == '.' && cd.asok_path[1] == '/')))
    {
        ERROR("ceph plugin(name=%s): administrative socket paths must begin "
                "with '/' or './' Can't parse: '%s'\n", cd.name, cd.asok_path);
        return -EINVAL;
    }
    array = realloc(g_daemons,
                    sizeof(struct ceph_daemon *) * (g_num_daemons + 1));
    if(array == NULL)
    {
        /* The positive return value here indicates that this is a
         * runtime error, not a configuration error.  */
        return ENOMEM;
    }
    g_daemons = (struct ceph_daemon**) array;
    nd = malloc(sizeof(struct ceph_daemon));
    if(!nd)
    {
        return ENOMEM;
    }
    memcpy(nd, &cd, sizeof(struct ceph_daemon));
    g_daemons[g_num_daemons++] = nd;
    return 0;
}

static int ceph_config(oconfig_item_t *ci)
{
    int ret, i;

    for(i = 0; i < ci->children_num; ++i)
    {
        oconfig_item_t *child = ci->children + i;
        if(strcasecmp("Daemon", child->key) == 0)
        {
            ret = cc_add_daemon_config(child);
            if(ret)
            {
                return ret;
            }
        }
        else if(strcasecmp("LongRunAvgLatency", child->key) == 0)
        {
            ret = cc_handle_bool(child, &long_run_latency_avg);
            if(ret)
            {
                return ret;
            }
        }
        else if(strcasecmp("ConvertSpecialMetricTypes", child->key) == 0)
        {
            ret = cc_handle_bool(child, &convert_special_metrics);
            if(ret)
            {
                return ret;
            }
        }
        else
        {
            WARNING("ceph plugin: ignoring unknown option %s", child->key);
        }
    }
    return 0;
}

static int
traverse_json(const unsigned char *json, uint32_t json_len, yajl_handle hand)
{
    yajl_status status = yajl_parse(hand, json, json_len);
    unsigned char *msg;

    switch(status)
    {
        case yajl_status_error:
            msg = yajl_get_error(hand, /* verbose = */ 1,
                                       /* jsonText = */ (unsigned char *) json,
                                                      (unsigned int) json_len);
            ERROR ("ceph plugin: yajl_parse failed: %s", msg);
            yajl_free_error(hand, msg);
            return 1;
        case yajl_status_client_canceled:
            return 1;
        default:
            return 0;
    }
}

static int
node_handler_define_schema(void *arg, const char *val, const char *key)
{
    struct ceph_daemon *d = (struct ceph_daemon *) arg;
    int pc_type;
    pc_type = atoi(val);
    DEBUG("\nceph_daemon_add_ds_entry(d=%s,key=%s,pc_type=%04x)",
            d->name, key, pc_type);
    return ceph_daemon_add_ds_entry(d, key, pc_type);
}

static int
node_handler_parse_fsid(void *arg, const char *val, const char *key)
{
    struct ceph_daemon *d = (struct ceph_daemon *) arg;
    int res = snprintf(d->fsid, FSID_STRING_LEN, "%s", val);
    if(res != (FSID_STRING_LEN-1))
    {
        WARNING("Could not set d->fsid, wrote %d bytes", res);
    }
    DEBUG("Set daemon->fsid to %s", d->fsid);
    return 0;
}

static int add_last(const char *dset_n, const char *ds_n, double cur_sum,
        uint64_t cur_count)
{
    last_poll_data[last_idx] = malloc(1 * sizeof(struct last_data));
    if(!last_poll_data[last_idx])
    {
        return -ENOMEM;
    }
    sstrncpy(last_poll_data[last_idx]->dset_name,dset_n,
            sizeof(last_poll_data[last_idx]->dset_name));
    sstrncpy(last_poll_data[last_idx]->ds_name,ds_n,
            sizeof(last_poll_data[last_idx]->ds_name));
    last_poll_data[last_idx]->last_sum = cur_sum;
    last_poll_data[last_idx]->last_count = cur_count;
    last_idx++;
    return 0;
}

static int update_last(const char *dset_n, const char *ds_n, double cur_sum,
        uint64_t cur_count)
{
    int i;
    for(i = 0; i < last_idx; i++)
    {
        if(strcmp(last_poll_data[i]->dset_name,dset_n) == 0 &&
                (strcmp(last_poll_data[i]->ds_name,ds_n) == 0))
        {
            last_poll_data[i]->last_sum = cur_sum;
            last_poll_data[i]->last_count = cur_count;
            return 0;
        }
    }

    if(!last_poll_data)
    {
        last_poll_data = malloc(1 * sizeof(struct last_data *));
        if(!last_poll_data)
        {
            return -ENOMEM;
        }
    }
    else
    {
        struct last_data **tmp_last = realloc(last_poll_data,
                ((last_idx+1) * sizeof(struct last_data *)));
        if(!tmp_last)
        {
            return -ENOMEM;
        }
        last_poll_data = tmp_last;
    }
    return add_last(dset_n,ds_n,cur_sum,cur_count);
}

static double get_last_avg(const char *dset_n, const char *ds_n,
        double cur_sum, uint64_t cur_count)
{
    int i;
    double result = -1.1, sum_delt = 0.0;
    uint64_t count_delt = 0;
    for(i = 0; i < last_idx; i++)
    {
        if((strcmp(last_poll_data[i]->dset_name,dset_n) == 0) &&
                (strcmp(last_poll_data[i]->ds_name,ds_n) == 0))
        {
            if(cur_count < last_poll_data[i]->last_count)
            {
                break;
            }
            sum_delt = (cur_sum - last_poll_data[i]->last_sum);
            count_delt = (cur_count - last_poll_data[i]->last_count);
            result = (sum_delt / count_delt);
            break;
        }
    }

    if(result == -1.1)
    {
        result = NAN;
    }
    if(update_last(dset_n,ds_n,cur_sum,cur_count) == -ENOMEM)
    {
        return -ENOMEM;
    }
    return result;
}

static int node_handler_fetch_data(void *arg, const char *val, const char *key)
{
    int dset_idx, ds_idx;
    value_t *uv;
    char dset_name[DATA_MAX_NAME_LEN];
    char ds_name[MAX_RRD_DS_NAME_LEN];
    struct values_tmp *vtmp = (struct values_tmp*) arg;
    memset(dset_name, 0, sizeof(dset_name));
    memset(ds_name, 0, sizeof(ds_name));
    if(parse_keys(key, dset_name, ds_name))
    {
        DEBUG("enter node_handler_fetch_data");
        return 1;
    }
    dset_idx = get_matching_dset(vtmp->d, dset_name);
    if(dset_idx == -1)
    {
        return 1;
    }
    ds_idx = get_matching_value(&vtmp->d->dset[dset_idx], ds_name,
            vtmp->d->dset[dset_idx].ds_num);
    if(ds_idx == -1)
    {
        DEBUG("DSet:%s, DS:%s, DSet idx:%d, DS idx:%d",
            dset_name,ds_name,dset_idx,ds_idx);
        return RETRY_AVGCOUNT;
    }
    uv = &(vtmp->vh[dset_idx].values[ds_idx]);

    if(vtmp->d->pc_types[dset_idx][ds_idx] & PERFCOUNTER_LATENCY)
    {
        if(vtmp->avgcount == -1)
        {
            sscanf(val, "%" PRIu64, &vtmp->avgcount);
        }
        else
        {
            double sum, result;
            sscanf(val, "%lf", &sum);
            DEBUG("avgcount:%ld",vtmp->avgcount);
            DEBUG("sum:%lf",sum);

            if(vtmp->avgcount == 0)
            {
                vtmp->avgcount = 1;
            }

            /** User wants latency values as long run avg */
            if(long_run_latency_avg)
            {
                result = (sum / vtmp->avgcount);
                DEBUG("uv->gauge = sumd / avgcounti = :%lf", result);
            }
            else
            {
                result = get_last_avg(dset_name, ds_name, sum, vtmp->avgcount);
                if(result == -ENOMEM)
                {
                    return -ENOMEM;
                }
                DEBUG("uv->gauge = (sumd_now - sumd_last) / "
                        "(avgcounti_now - avgcounti_last) = :%lf", result);
            }

            uv->gauge = result;
            vtmp->avgcount = -1;
        }
    }
    else if(vtmp->d->pc_types[dset_idx][ds_idx] & PERFCOUNTER_DERIVE)
    {
        uint64_t derive_val;
        sscanf(val, "%" PRIu64, &derive_val);
        uv->derive = derive_val;
        DEBUG("uv->derive = %" PRIu64 "",(uint64_t)uv->derive);
    }
    else
    {
        double other_val;
        sscanf(val, "%lf", &other_val);
        uv->gauge = other_val;
        DEBUG("uv->gauge = %lf",uv->gauge);
    }
    return 0;
}

static int cconn_connect(struct cconn *io)
{
    struct sockaddr_un address;
    int flags, fd, err;
    if(io->state != CSTATE_UNCONNECTED)
    {
        ERROR("cconn_connect: io->state != CSTATE_UNCONNECTED");
        return -EDOM;
    }
    fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if(fd < 0)
    {
        int err = -errno;
        ERROR("cconn_connect: socket(PF_UNIX, SOCK_STREAM, 0) failed: "
        "error %d", err);
        return err;
    }
    memset(&address, 0, sizeof(struct sockaddr_un));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s",
            io->d->asok_path);
    RETRY_ON_EINTR(err,
        connect(fd, (struct sockaddr *) &address, sizeof(struct sockaddr_un)));
    if(err < 0)
    {
        ERROR("cconn_connect: connect(%d) failed: error %d", fd, err);
        return err;
    }

    flags = fcntl(fd, F_GETFL, 0);
    if(fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
    {
        err = -errno;
        ERROR("cconn_connect: fcntl(%d, O_NONBLOCK) error %d", fd, err);
        return err;
    }
    io->asok = fd;
    io->state = CSTATE_WRITE_REQUEST;
    io->amt = 0;
    io->json_len = 0;
    io->json = NULL;
    return 0;
}

static void cconn_close(struct cconn *io)
{
    io->state = CSTATE_UNCONNECTED;
    if(io->asok != -1)
    {
        int res;
        RETRY_ON_EINTR(res, close(io->asok));
    }
    io->asok = -1;
    io->amt = 0;
    io->json_len = 0;
    sfree(io->json);
    io->json = NULL;
}

/* Process incoming JSON counter data */
static int
cconn_process_data(struct cconn *io, yajl_struct *yajl, yajl_handle hand)
{
    int i, ret = 0;
    struct values_tmp *vtmp = calloc(1, sizeof(struct values_tmp)
                    + (sizeof(struct values_holder)) * io->d->dset_num);
    if(!vtmp)
    {
        return -ENOMEM;
    }

    for(i = 0; i < io->d->dset_num; i++)
    {
        value_t *val = calloc(1, (sizeof(value_t) * io->d->dset[i].ds_num));
        vtmp->vh[i].values = val;
        vtmp->vh[i].values_len = io->d->dset[i].ds_num;
    }
    vtmp->d = io->d;
    vtmp->holder_num = io->d->dset_num;
    vtmp->avgcount = -1;
    yajl->handler_arg = vtmp;
    ret = traverse_json(io->json, io->json_len, hand);
    if(ret)
    {
        goto done;
    }
    for(i = 0; i < vtmp->holder_num; i++)
    {
        value_list_t vl = VALUE_LIST_INIT;
        sstrncpy(vl.host, hostname_g, sizeof(vl.host));
        sstrncpy(vl.plugin, "ceph", sizeof(vl.plugin));
        char tmp_plugin_instance[DATA_MAX_NAME_LEN];
        size_t chars_remaining = (size_t)DATA_MAX_NAME_LEN;
        sstrncpy(tmp_plugin_instance, io->d->name, chars_remaining);
        chars_remaining -= strlen(io->d->name);
        if(chars_remaining >= (strlen(io->d->cluster)+1))
        {
            strncat(tmp_plugin_instance, "-", chars_remaining);
            chars_remaining -= 1;
            strncat(tmp_plugin_instance, io->d->cluster, chars_remaining);
            chars_remaining -= strlen(io->d->cluster);
            if(chars_remaining >= (FSID_STRING_LEN+1))
            {
                strncat(tmp_plugin_instance, ".", chars_remaining);
                chars_remaining -= 1;
                strncat(tmp_plugin_instance, io->d->fsid, chars_remaining);
            }
        }
        sstrncpy(vl.plugin_instance, tmp_plugin_instance, sizeof(vl.plugin_instance));
        sstrncpy(vl.type, io->d->dset[i].type, sizeof(vl.type));
        vl.values = vtmp->vh[i].values;
        vl.values_len = io->d->dset[i].ds_num;
        DEBUG("cconn_process_data(io=%s): vl.values_len=%d, json=\"%s\"",
                io->d->name, vl.values_len, io->json);
        ret = plugin_dispatch_values(&vl);
        if(ret)
        {
            goto done;
        }
    }

    done: for(i = 0; i < vtmp->holder_num; i++)
    {
        sfree(vtmp->vh[i].values);
    }
    sfree(vtmp);
    return ret;
}

static int cconn_process_json(struct cconn *io)
{
    if((io->request_type != ASOK_REQ_DATA) &&
            (io->request_type != ASOK_REQ_SCHEMA)) &&
            (io->request_type != ASOK_REQ_FSID))
    {
        return -EDOM;
    }

    int result = 1;
    yajl_handle hand;
    yajl_status status;

    hand = yajl_alloc(&callbacks,
#if HAVE_YAJL_V2
      /* alloc funcs = */ NULL,
#else
      /* alloc funcs = */ NULL, NULL,
#endif
      /* context = */ (void *)(&io->yajl));

    if(!hand)
    {
        ERROR ("ceph plugin: yajl_alloc failed.");
        return ENOMEM;
    }

    io->yajl.depth = 0;

    switch(io->request_type)
    {
        case ASOK_REQ_DATA:
            io->yajl.handler = node_handler_fetch_data;
            result = cconn_process_data(io, &io->yajl, hand);
            break;
        case ASOK_REQ_SCHEMA:
            io->yajl.handler = node_handler_define_schema;
            io->yajl.handler_arg = io->d;
            result = traverse_json(io->json, io->json_len, hand);
            break;
        case ASOK_REQ_FSID:
            io->yajl.handler = node_handler_parse_fsid;
            io->yajl.handler_arg = io->d;
            result = traverse_json(io->json, io->json_len, hand);
            break;
    }

    if(result)
    {
        goto done;
    }

#if HAVE_YAJL_V2
    status = yajl_complete_parse(hand);
#else
    status = yajl_parse_complete(hand);
#endif

    if (status != yajl_status_ok)
    {
      unsigned char *errmsg = yajl_get_error (hand, /* verbose = */ 0,
          /* jsonText = */ NULL, /* jsonTextLen = */ 0);
      ERROR ("ceph plugin: yajl_parse_complete failed: %s",
          (char *) errmsg);
      yajl_free_error (hand, errmsg);
      yajl_free (hand);
      return 1;
    }

    done:
    yajl_free (hand);
    return result;
}

static int cconn_validate_revents(struct cconn *io, int revents)
{
    if(revents & POLLERR)
    {
        ERROR("cconn_validate_revents(name=%s): got POLLERR", io->d->name);
        return -EIO;
    }
    switch (io->state)
    {
        case CSTATE_WRITE_REQUEST:
            return (revents & POLLOUT) ? 0 : -EINVAL;
        case CSTATE_READ_VERSION:
        case CSTATE_READ_AMT:
        case CSTATE_READ_JSON:
            return (revents & POLLIN) ? 0 : -EINVAL;
        default:
            ERROR("cconn_validate_revents(name=%s) got to illegal state on "
                    "line %d", io->d->name, __LINE__);
            return -EDOM;
    }
}

/** Handle a network event for a connection */
static int cconn_handle_event(struct cconn *io)
{
    int ret;
    switch (io->state)
    {
        case CSTATE_UNCONNECTED:
            ERROR("cconn_handle_event(name=%s) got to illegal state on line "
                    "%d", io->d->name, __LINE__);

            return -EDOM;
        case CSTATE_WRITE_REQUEST:
        {
            char cmd[64];
            if(io->request_type == ASOK_REQ_FSID)
            {
                snprintf(cmd, sizeof(cmd), "%s%s%s", CEPH_ASOK_REQ_PRE,
                        CEPH_FSID_REQ ,CEPH_ASOK_REQ_POST);
            }
            else
            {
                snprintf(cmd, sizeof(cmd), "%s%d%s", CEPH_ASOK_REQ_PRE,
                        io->request_type, CEPH_ASOK_REQ_POST);
            }
            size_t cmd_len = strlen(cmd);
            RETRY_ON_EINTR(ret,
                  write(io->asok, ((char*)&cmd) + io->amt, cmd_len - io->amt));
            DEBUG("cconn_handle_event(name=%s,state=%d,amt=%d,ret=%d)",
                    io->d->name, io->state, io->amt, ret);
            if(ret < 0)
            {
                return ret;
            }
            io->amt += ret;
            if(io->amt >= cmd_len)
            {
                io->amt = 0;
                switch (io->request_type)
                {
                    case ASOK_REQ_VERSION:
                        io->state = CSTATE_READ_VERSION;
                        break;
                    default:
                        io->state = CSTATE_READ_AMT;
                        break;
                }
            }
            return 0;
        }
        case CSTATE_READ_VERSION:
        {
            RETRY_ON_EINTR(ret,
                    read(io->asok, ((char*)(&io->d->version)) + io->amt,
                            sizeof(io->d->version) - io->amt));
            DEBUG("cconn_handle_event(name=%s,state=%d,ret=%d)",
                    io->d->name, io->state, ret);
            if(ret < 0)
            {
                return ret;
            }
            io->amt += ret;
            if(io->amt >= sizeof(io->d->version))
            {
                io->d->version = ntohl(io->d->version);
                if(io->d->version != 1)
                {
                    ERROR("cconn_handle_event(name=%s) not "
                    "expecting version %d!", io->d->name, io->d->version);
                    return -ENOTSUP;
                }
                DEBUG("cconn_handle_event(name=%s): identified as "
                        "version %d", io->d->name, io->d->version);
                io->amt = 0;
                cconn_close(io);
                io->request_type = ASOK_REQ_FSID;
            }
            return 0;
        }
        case CSTATE_READ_AMT:
        {
            RETRY_ON_EINTR(ret,
                    read(io->asok, ((char*)(&io->json_len)) + io->amt,
                            sizeof(io->json_len) - io->amt));
            DEBUG("cconn_handle_event(name=%s,state=%d,ret=%d)",
                    io->d->name, io->state, ret);
            if(ret < 0)
            {
                return ret;
            }
            io->amt += ret;
            if(io->amt >= sizeof(io->json_len))
            {
                io->json_len = ntohl(io->json_len);
                io->amt = 0;
                io->state = CSTATE_READ_JSON;
                io->json = calloc(1, io->json_len + 1);
                if(!io->json)
                {
                    ERROR("ERR CALLOCING IO->JSON");
                    return -ENOMEM;
                }
            }
            return 0;
        }
        case CSTATE_READ_JSON:
        {
            RETRY_ON_EINTR(ret,
                   read(io->asok, io->json + io->amt, io->json_len - io->amt));
            DEBUG("cconn_handle_event(name=%s,state=%d,ret=%d)",
                    io->d->name, io->state, ret);
            if(ret < 0)
            {
                return ret;
            }
            io->amt += ret;
            if(io->amt >= io->json_len)
            {
                ret = cconn_process_json(io);
                if(ret)
                {
                    return ret;
                }
                cconn_close(io);
                if(io->request_type == ASOK_REQ_FSID)
                {
                    io->amt = 0;
                    io->request_type = ASOK_REQ_SCHEMA;
                }
                else
                {
                    io->request_type = ASOK_REQ_NONE;
                }
            }
            return 0;
        }
        default:
            ERROR("cconn_handle_event(name=%s) got to illegal state on "
            "line %d", io->d->name, __LINE__);
            return -EDOM;
    }
}

static int cconn_prepare(struct cconn *io, struct pollfd* fds)
{
    int ret;
    if(io->request_type == ASOK_REQ_NONE)
    {
        /* The request has already been serviced. */
        return 0;
    }
    else if((io->request_type == ASOK_REQ_DATA) && (io->d->dset_num == 0))
    {
        /* If there are no counters to report on, don't bother
         * connecting */
        return 0;
    }

    switch (io->state)
    {
        case CSTATE_UNCONNECTED:
            ret = cconn_connect(io);
            if(ret > 0)
            {
                return -ret;
            }
            else if(ret < 0)
            {
                return ret;
            }
            fds->fd = io->asok;
            fds->events = POLLOUT;
            return 1;
        case CSTATE_WRITE_REQUEST:
            fds->fd = io->asok;
            fds->events = POLLOUT;
            return 1;
        case CSTATE_READ_VERSION:
        case CSTATE_READ_AMT:
        case CSTATE_READ_JSON:
            fds->fd = io->asok;
            fds->events = POLLIN;
            return 1;
        default:
            ERROR("cconn_prepare(name=%s) got to illegal state on line %d",
                    io->d->name, __LINE__);
            return -EDOM;
    }
}

/** Returns the difference between two struct timevals in milliseconds.
 * On overflow, we return max/min int.
 */
static int milli_diff(const struct timeval *t1, const struct timeval *t2)
{
    int64_t ret;
    int sec_diff = t1->tv_sec - t2->tv_sec;
    int usec_diff = t1->tv_usec - t2->tv_usec;
    ret = usec_diff / 1000;
    ret += (sec_diff * 1000);
    return (ret > INT_MAX) ? INT_MAX : ((ret < INT_MIN) ? INT_MIN : (int)ret);
}

/** This handles the actual network I/O to talk to the Ceph daemons.
 */
static int cconn_main_loop(uint32_t request_type)
{
    int i, ret, some_unreachable = 0;
    struct timeval end_tv;
    struct cconn io_array[g_num_daemons];

    DEBUG("entering cconn_main_loop(request_type = %d)", request_type);

    /* create cconn array */
    memset(io_array, 0, sizeof(io_array));
    for(i = 0; i < g_num_daemons; ++i)
    {
        io_array[i].d = g_daemons[i];
        io_array[i].request_type = request_type;
        io_array[i].state = CSTATE_UNCONNECTED;
    }

    /** Calculate the time at which we should give up */
    gettimeofday(&end_tv, NULL);
    end_tv.tv_sec += CEPH_TIMEOUT_INTERVAL;

    while (1)
    {
        int nfds, diff;
        struct timeval tv;
        struct cconn *polled_io_array[g_num_daemons];
        struct pollfd fds[g_num_daemons];
        memset(fds, 0, sizeof(fds));
        nfds = 0;
        for(i = 0; i < g_num_daemons; ++i)
        {
            struct cconn *io = io_array + i;
            ret = cconn_prepare(io, fds + nfds);
            if(ret < 0)
            {
                WARNING("ERROR: cconn_prepare(name=%s,i=%d,st=%d)=%d",
                        io->d->name, i, io->state, ret);
                cconn_close(io);
                io->request_type = ASOK_REQ_NONE;
                some_unreachable = 1;
            }
            else if(ret == 1)
            {
                DEBUG("did cconn_prepare(name=%s,i=%d,st=%d)",
                        io->d->name, i, io->state);
                polled_io_array[nfds++] = io_array + i;
            }
        }
        if(nfds == 0)
        {
            /* finished */
            ret = 0;
            DEBUG("cconn_main_loop: no more cconn to manage.");
            goto done;
        }
        gettimeofday(&tv, NULL);
        diff = milli_diff(&end_tv, &tv);
        if(diff <= 0)
        {
            /* Timed out */
            ret = -ETIMEDOUT;
            WARNING("ERROR: cconn_main_loop: timed out.\n");
            goto done;
        }
        RETRY_ON_EINTR(ret, poll(fds, nfds, diff));
        if(ret < 0)
        {
            ERROR("poll(2) error: %d", ret);
            goto done;
        }
        for(i = 0; i < nfds; ++i)
        {
            struct cconn *io = polled_io_array[i];
            int revents = fds[i].revents;
            if(revents == 0)
            {
                /* do nothing */
            }
            else if(cconn_validate_revents(io, revents))
            {
                WARNING("ERROR: cconn(name=%s,i=%d,st=%d): "
                "revents validation error: "
                "revents=0x%08x", io->d->name, i, io->state, revents);
                cconn_close(io);
                io->request_type = ASOK_REQ_NONE;
                some_unreachable = 1;
            }
            else
            {
                int ret = cconn_handle_event(io);
                if(ret)
                {
                    WARNING("ERROR: cconn_handle_event(name=%s,"
                    "i=%d,st=%d): error %d", io->d->name, i, io->state, ret);
                    cconn_close(io);
                    io->request_type = ASOK_REQ_NONE;
                    some_unreachable = 1;
                }
            }
        }
    }
    done: for(i = 0; i < g_num_daemons; ++i)
    {
        cconn_close(io_array + i);
    }
    if(some_unreachable)
    {
        DEBUG("cconn_main_loop: some Ceph daemons were unreachable.");
    }
    else
    {
        DEBUG("cconn_main_loop: reached all Ceph daemons :)");
    }
    return ret;
}

static int ceph_read(void)
{
    return cconn_main_loop(ASOK_REQ_DATA);
}

/******* lifecycle *******/
static int ceph_init(void)
{
    int i, ret, j;
    DEBUG("ceph_init");
    ceph_daemons_print();

    ret = cconn_main_loop(ASOK_REQ_VERSION);
    if(ret)
    {
        return ret;
    }
    for(i = 0; i < g_num_daemons; ++i)
    {
        struct ceph_daemon *d = g_daemons[i];
        for(j = 0; j < d->dset_num; j++)
        {
            ret = plugin_register_data_set(d->dset + j);
            if(ret)
            {
                ERROR("plugin_register_data_set(%s) failed!", d->name);
            }
            else
            {
                DEBUG("plugin_register_data_set(%s): "
                        "(d->dset)[%d]->ds_num=%d",
                        d->name, j, d->dset[j].ds_num);
            }
        }
    }
    return 0;
}

static int ceph_shutdown(void)
{
    int i;
    for(i = 0; i < g_num_daemons; ++i)
    {
        ceph_daemon_free(g_daemons[i]);
    }
    sfree(g_daemons);
    g_daemons = NULL;
    g_num_daemons = 0;
    for(i = 0; i < last_idx; i++)
    {
        sfree(last_poll_data[i]);
    }
    sfree(last_poll_data);
    last_poll_data = NULL;
    last_idx = 0;
    DEBUG("finished ceph_shutdown");
    return 0;
}

void module_register(void)
{
    plugin_register_complex_config("ceph", ceph_config);
    plugin_register_init("ceph", ceph_init);
    plugin_register_read("ceph", ceph_read);
    plugin_register_shutdown("ceph", ceph_shutdown);
}
