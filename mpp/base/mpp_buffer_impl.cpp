/*
 * Copyright 2015 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define MODULE_TAG "mpp_buffer"

#include <string.h>

#include "mpp_log.h"
#include "mpp_mem.h"
#include "mpp_env.h"
#include "mpp_hash.h"

#include "mpp_buffer_impl.h"

#define MAX_GROUP_BIT                   8
#define MAX_MISC_GROUP_BIT              3
#define BUFFER_OPS_MAX_COUNT            1024

#define SEARCH_GROUP_BY_ID(id)  ((MppBufferService::get_instance())->get_group_by_id(id))

typedef MPP_RET (*BufferOp)(MppAllocator allocator, MppBufferInfo *data);

// use this class only need it to init legacy group before main
class MppBufferService
{
private:

    // avoid any unwanted function
    MppBufferService();
    ~MppBufferService();
    MppBufferService(const MppBufferService &);
    MppBufferService &operator=(const MppBufferService &);

    // buffer group final release function
    void                destroy_group(MppBufferGroupImpl *group);

    RK_U32              get_group_id();
    RK_U32              group_id;
    RK_U32              group_count;
    RK_U32              finalizing;
    RK_U32              finished;

    RK_U32              total_size;
    RK_U32              total_max;

    // misc group for internal / externl buffer with different type
    RK_U32              misc[MPP_BUFFER_MODE_BUTT][MPP_BUFFER_TYPE_BUTT];
    RK_U32              misc_count;
    /* preset allocator apis */
    MppAllocator        mAllocator[MPP_BUFFER_TYPE_BUTT];
    MppAllocatorApi     *mAllocatorApi[MPP_BUFFER_TYPE_BUTT];

    struct list_head    mListGroup;
    DECLARE_HASHTABLE(mHashGroup, MAX_GROUP_BIT);

    // list for used buffer which do not have group
    struct list_head    mListOrphan;

public:
    static MppBufferService *get_instance() {
        static MppBufferService instance;
        return &instance;
    }
    static Mutex *get_lock() {
        static Mutex lock;
        return &lock;
    }

    MppBufferGroupImpl  *get_group(const char *tag, const char *caller,
                                   MppBufferMode mode, MppBufferType type,
                                   RK_U32 is_misc);
    RK_U32              get_misc(MppBufferMode mode, MppBufferType type);
    void                put_group(const char *caller, MppBufferGroupImpl *group);
    MppBufferGroupImpl  *get_group_by_id(RK_U32 id);
    void                dump(const char *info);
    RK_U32              is_finalizing();
    void                inc_total(RK_U32 size);
    void                dec_total(RK_U32 size);
    RK_U32              get_total_now() { return total_size; };
    RK_U32              get_total_max() { return total_max; };
};

static const char *mode2str[MPP_BUFFER_MODE_BUTT] = {
    "internal",
    "external",
};

static const char *type2str[MPP_BUFFER_TYPE_BUTT] = {
    "normal",
    "ion",
    "dma-buf",
    "drm",
};
static const char *ops2str[BUF_OPS_BUTT] = {
    "grp create ",
    "grp release",
    "grp reset",
    "grp orphan",
    "grp destroy",

    "buf commit ",
    "buf create ",
    "buf mmap   ",
    "buf ref inc",
    "buf ref dec",
    "buf discard",
    "buf destroy",
};

RK_U32 mpp_buffer_debug = 0;

static MppBufLogs *buf_logs_init(RK_U32 max_count)
{
    MppBufLogs *logs = NULL;
    pthread_mutexattr_t attr;

    if (!max_count)
        return NULL;

    logs = mpp_malloc_size(MppBufLogs, sizeof(MppBufLogs) + max_count * sizeof(MppBufLog));
    if (!logs) {
        mpp_err_f("failed to create %d buf logs\n", max_count);
        return NULL;
    }

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&logs->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    logs->max_count = max_count;
    logs->log_count = 0;
    logs->log_write = 0;
    logs->log_read = 0;
    logs->logs = (MppBufLog *)(logs + 1);

    return logs;
}

static void buf_logs_deinit(MppBufLogs *logs)
{
    pthread_mutex_destroy(&logs->lock);
    MPP_FREE(logs);
}

static void buf_logs_write(MppBufLogs *logs, RK_U32 group_id, RK_S32 buffer_id,
                           MppBufOps ops, RK_S32 ref_count, const char *caller)
{
    MppBufLog *log = NULL;

    pthread_mutex_lock(&logs->lock);

    log = &logs->logs[logs->log_write];
    log->group_id   = group_id;
    log->buffer_id  = buffer_id;
    log->ops        = ops;
    log->ref_count  = ref_count;
    log->caller     = caller;

    logs->log_write++;
    if (logs->log_write >= logs->max_count)
        logs->log_write = 0;

    if (logs->log_count < logs->max_count)
        logs->log_count++;
    else {
        logs->log_read++;
        if (logs->log_read >= logs->max_count)
            logs->log_read = 0;
    }

    pthread_mutex_unlock(&logs->lock);
}

static void buf_logs_dump(MppBufLogs *logs)
{
    while (logs->log_count) {
        MppBufLog *log = &logs->logs[logs->log_read];

        if (log->buffer_id >= 0)
            mpp_log("group %3d buffer %4d ops %s ref_count %d caller %s\n",
                    log->group_id, log->buffer_id,
                    ops2str[log->ops], log->ref_count, log->caller);
        else
            mpp_log("group %3d ops %s\n", log->group_id, ops2str[log->ops]);

        logs->log_read++;
        if (logs->log_read >= logs->max_count)
            logs->log_read = 0;
        logs->log_count--;
    }
    mpp_assert(logs->log_read == logs->log_write);
}

static void buf_add_log(MppBufferImpl *buffer, MppBufOps ops, const char* caller)
{
    if (buffer->log_runtime_en) {
        mpp_log("group %3d buffer %4d fd %3d ops %s ref_count %d caller %s\n",
                buffer->group_id, buffer->buffer_id, buffer->info.fd,
                ops2str[ops], buffer->ref_count, caller);
    }
    if (buffer->logs)
        buf_logs_write(buffer->logs, buffer->group_id, buffer->buffer_id,
                       ops, buffer->ref_count, caller);
}

static void buf_grp_add_log(MppBufferGroupImpl *group, MppBufOps ops, const char* caller)
{
    if (group->log_runtime_en) {
        mpp_log("group %3d mode %d type %d ops %s\n", group->group_id,
                group->mode, group->type, ops2str[ops]);
    }
    if (group->logs)
        buf_logs_write(group->logs, group->group_id, -1, ops, 0, caller);
}

static MPP_RET deinit_buffer_no_lock(MppBufferImpl *buffer, const char *caller)
{
    MppBufferGroupImpl *group = NULL;

    if (!MppBufferService::get_instance()->is_finalizing()) {
        mpp_assert(buffer->ref_count == 0);
        mpp_assert(buffer->used == 0);
    }

    list_del_init(&buffer->list_status);

    BufferOp func = (buffer->mode == MPP_BUFFER_INTERNAL) ?
                    (buffer->alloc_api->free) :
                    (buffer->alloc_api->release);

    func(buffer->allocator, &buffer->info);

    {
        AutoMutex auto_lock(MppBufferService::get_lock());
        group = SEARCH_GROUP_BY_ID(buffer->group_id);
    }
    if (group) {
        RK_U32 destroy = 0;

        group->usage -= buffer->info.size;
        group->buffer_count--;

        if (group->mode == MPP_BUFFER_INTERNAL)
            MppBufferService::get_instance()->dec_total(buffer->info.size);

        buf_add_log(buffer, BUF_DESTROY, caller);

        if (group->is_orphan && !group->usage && !group->is_finalizing)
            destroy = 1;

        if (destroy)
            MppBufferService::get_instance()->put_group(caller, group);
    } else {
        mpp_assert(MppBufferService::get_instance()->is_finalizing());
    }

    mpp_free(buffer);

    return MPP_OK;
}

static MPP_RET inc_buffer_ref(MppBufferImpl *buffer, const char *caller)
{
    MPP_RET ret = MPP_OK;

    pthread_mutex_lock(&buffer->lock);
    buffer->ref_count++;
    buf_add_log(buffer, BUF_REF_INC, caller);
    if (!buffer->used) {
        MppBufferGroupImpl *group = NULL;

        {
            AutoMutex auto_lock(MppBufferService::get_lock());
            group = SEARCH_GROUP_BY_ID(buffer->group_id);
        }
        // NOTE: when increasing ref_count the unused buffer must be under certain group
        mpp_assert(group);
        buffer->used = 1;
        if (group) {
            pthread_mutex_lock(&group->buf_lock);
            list_del_init(&buffer->list_status);
            list_add_tail(&buffer->list_status, &group->list_used);
            group->count_used++;
            group->count_unused--;
            pthread_mutex_unlock(&group->buf_lock);
        } else {
            mpp_err_f("unused buffer without group\n");
            ret = MPP_NOK;
        }
    }
    pthread_mutex_unlock(&buffer->lock);
    return ret;
}

static void dump_buffer_info(MppBufferImpl *buffer)
{
    mpp_log("buffer %p fd %4d size %10d ref_count %3d discard %d caller %s\n",
            buffer, buffer->info.fd, buffer->info.size,
            buffer->ref_count, buffer->discard, buffer->caller);
}

MPP_RET mpp_buffer_create(const char *tag, const char *caller,
                          MppBufferGroupImpl *group, MppBufferInfo *info,
                          MppBufferImpl **buffer)
{
    MPP_BUF_FUNCTION_ENTER();

    MPP_RET ret = MPP_OK;
    BufferOp func = NULL;
    MppBufferImpl *p = NULL;

    if (NULL == group) {
        mpp_err_f("can not create buffer without group\n");
        ret = MPP_NOK;
        goto RET;
    }

    if (group->limit_count && group->buffer_count >= group->limit_count) {
        if (group->log_runtime_en)
            mpp_log_f("group %d reach count limit %d\n", group->group_id, group->limit_count);
        ret = MPP_NOK;
        goto RET;
    }

    if (group->limit_size && info->size > group->limit_size) {
        mpp_err_f("required size %d reach group size limit %d\n", info->size, group->limit_size);
        ret = MPP_NOK;
        goto RET;
    }

    p = mpp_calloc(MppBufferImpl, 1);
    if (NULL == p) {
        mpp_err_f("failed to allocate context\n");
        ret = MPP_ERR_MALLOC;
        goto RET;
    }

    func = (group->mode == MPP_BUFFER_INTERNAL) ?
           (group->alloc_api->alloc) : (group->alloc_api->import);
    ret = func(group->allocator, info);
    if (ret) {
        mpp_err_f("failed to create buffer with size %d\n", info->size);
        mpp_free(p);
        ret = MPP_ERR_MALLOC;
        goto RET;
    }

    if (NULL == tag)
        tag = group->tag;

    strncpy(p->tag, tag, sizeof(p->tag));
    p->caller = caller;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&p->lock, &attr);
    pthread_mutexattr_destroy(&attr);
    p->allocator = group->allocator;
    p->alloc_api = group->alloc_api;
    p->log_runtime_en = group->log_runtime_en;
    p->log_history_en = group->log_history_en;
    p->group_id = group->group_id;
    p->mode = group->mode;
    p->type = group->type;
    p->logs = group->logs;
    p->info = *info;

    pthread_mutex_lock(&group->buf_lock);
    p->buffer_id = group->buffer_id++;
    INIT_LIST_HEAD(&p->list_status);
    list_add_tail(&p->list_status, &group->list_unused);

    group->usage += info->size;
    group->buffer_count++;
    group->count_unused++;
    pthread_mutex_unlock(&group->buf_lock);

    buf_add_log(p, (group->mode == MPP_BUFFER_INTERNAL) ? (BUF_CREATE) : (BUF_COMMIT), caller);

    if (group->mode == MPP_BUFFER_INTERNAL)
        MppBufferService::get_instance()->inc_total(info->size);

    if (buffer) {
        inc_buffer_ref(p, caller);
        *buffer = p;
    }

    if (group->callback)
        group->callback(group->arg, group);
RET:
    MPP_BUF_FUNCTION_LEAVE();
    return ret;
}

MPP_RET mpp_buffer_mmap(MppBufferImpl *buffer, const char* caller)
{
    MPP_BUF_FUNCTION_ENTER();

    MPP_RET ret = buffer->alloc_api->mmap(buffer->allocator, &buffer->info);
    if (ret)
        mpp_err_f("buffer %d group %d fd %d map failed caller %s\n",
                  buffer->buffer_id, buffer->group_id, buffer->info.fd, caller);

    buf_add_log(buffer, BUF_MMAP, caller);

    MPP_BUF_FUNCTION_LEAVE();
    return ret;
}

MPP_RET mpp_buffer_ref_inc(MppBufferImpl *buffer, const char* caller)
{
    MPP_BUF_FUNCTION_ENTER();

    MPP_RET ret = inc_buffer_ref(buffer, caller);

    MPP_BUF_FUNCTION_LEAVE();
    return ret;
}


MPP_RET mpp_buffer_ref_dec(MppBufferImpl *buffer, const char* caller)
{
    MPP_RET ret = MPP_OK;

    MPP_BUF_FUNCTION_ENTER();

    pthread_mutex_lock(&buffer->lock);

    buf_add_log(buffer, BUF_REF_DEC, caller);

    if (buffer->ref_count <= 0) {
        mpp_err_f("found non-positive ref_count %d caller %s\n",
                  buffer->ref_count, buffer->caller);
        mpp_abort();
        ret = MPP_NOK;
        goto done;
    }

    buffer->ref_count--;
    if (0 == buffer->ref_count) {
        MppBufferGroupImpl *group = SEARCH_GROUP_BY_ID(buffer->group_id);

        pthread_mutex_lock(&group->buf_lock);
        buffer->used = 0;
        list_del_init(&buffer->list_status);
        if (group->is_misc) {
            deinit_buffer_no_lock(buffer, caller);
        } else {
            if (buffer->discard) {
                deinit_buffer_no_lock(buffer, caller);
            } else {
                list_add_tail(&buffer->list_status, &group->list_unused);
                group->count_unused++;
            }
        }
        group->count_used--;
        if (group->callback)
            group->callback(group->arg, group);
        pthread_mutex_unlock(&group->buf_lock);
    }
done:
    pthread_mutex_unlock(&buffer->lock);
    MPP_BUF_FUNCTION_LEAVE();
    return ret;
}

MppBufferImpl *mpp_buffer_get_unused(MppBufferGroupImpl *p, size_t size)
{
    MPP_BUF_FUNCTION_ENTER();

    MppBufferImpl *buffer = NULL;

    pthread_mutex_lock(&p->buf_lock);
    if (!list_empty(&p->list_unused)) {
        MppBufferImpl *pos, *n;
        RK_S32 found = 0;
        RK_S32 search_count = 0;

        list_for_each_entry_safe(pos, n, &p->list_unused, MppBufferImpl, list_status) {
            mpp_buf_dbg(MPP_BUF_DBG_CHECK_SIZE, "request size %d on buf idx %d size %d\n",
                        size, pos->buffer_id, pos->info.size);
            if (pos->info.size >= size) {
                buffer = pos;
                inc_buffer_ref(buffer, __FUNCTION__);
                found = 1;
                break;
            } else {
                if (MPP_BUFFER_INTERNAL == p->mode) {
                    deinit_buffer_no_lock(pos, __FUNCTION__);
                    p->count_unused--;
                } else
                    search_count++;
            }
        }

        if (!found && search_count)
            mpp_err_f("can not found match buffer with size larger than %d\n", size);
    }
    pthread_mutex_unlock(&p->buf_lock);

    MPP_BUF_FUNCTION_LEAVE();
    return buffer;
}

RK_U32 mpp_buffer_to_addr(MppBuffer buffer, size_t offset)
{
    MppBufferImpl *impl = (MppBufferImpl *)buffer;

    if (NULL == impl) {
        mpp_err_f("NULL buffer convert to zero address\n");
        return 0;
    }

    if (impl->info.fd >= (1 << 10)) {
        mpp_err_f("buffer fd %d is too large\n");
        return 0;
    }

    if (impl->offset + offset >= SZ_4M) {
        mpp_err_f("offset %d + %d is larger than 4M use extra info to send offset\n");
        return 0;
    }

    RK_U32 addr = impl->info.fd + ((impl->offset + offset) << 10);

    return addr;
}

MPP_RET mpp_buffer_group_init(MppBufferGroupImpl **group, const char *tag, const char *caller,
                              MppBufferMode mode, MppBufferType type)
{
    MPP_BUF_FUNCTION_ENTER();
    mpp_assert(caller);

    *group = MppBufferService::get_instance()->get_group(tag, caller, mode, type, 0);

    MPP_BUF_FUNCTION_LEAVE();
    return ((*group) ? (MPP_OK) : (MPP_NOK));
}

MPP_RET mpp_buffer_group_deinit(MppBufferGroupImpl *p)
{
    if (NULL == p) {
        mpp_err_f("found NULL pointer\n");
        return MPP_ERR_NULL_PTR;
    }

    MPP_BUF_FUNCTION_ENTER();

    MppBufferService::get_instance()->put_group(__FUNCTION__, p);

    MPP_BUF_FUNCTION_LEAVE();
    return MPP_OK;
}

MPP_RET mpp_buffer_group_reset(MppBufferGroupImpl *p)
{
    if (NULL == p) {
        mpp_err_f("found NULL pointer\n");
        return MPP_ERR_NULL_PTR;
    }

    MPP_BUF_FUNCTION_ENTER();

    pthread_mutex_lock(&p->buf_lock);

    AutoMutex auto_lock(MppBufferService::get_lock());
    buf_grp_add_log(p, GRP_RESET, NULL);

    if (!list_empty(&p->list_used)) {
        MppBufferImpl *pos, *n;

        list_for_each_entry_safe(pos, n, &p->list_used, MppBufferImpl, list_status) {
            buf_add_log(pos, BUF_DISCARD, NULL);
            pos->discard = 1;
        }
    }

    // remove unused list
    if (!list_empty(&p->list_unused)) {
        MppBufferImpl *pos, *n;
        list_for_each_entry_safe(pos, n, &p->list_unused, MppBufferImpl, list_status) {
            deinit_buffer_no_lock(pos, __FUNCTION__);
            p->count_unused--;
        }
    }

    pthread_mutex_unlock(&p->buf_lock);

    MPP_BUF_FUNCTION_LEAVE();
    return MPP_OK;
}

MPP_RET mpp_buffer_group_set_callback(MppBufferGroupImpl *p,
                                      MppBufCallback callback, void *arg)
{
    if (NULL == p) {
        mpp_err_f("found NULL pointer\n");
        return MPP_ERR_NULL_PTR;
    }

    MPP_BUF_FUNCTION_ENTER();

    p->callback = callback;
    p->arg      = arg;

    MPP_BUF_FUNCTION_LEAVE();
    return MPP_OK;
}

void mpp_buffer_group_dump(MppBufferGroupImpl *group, const char *caller)
{
    mpp_log("\ndumping buffer group %p id %d from %s\n", group,
            group->group_id, caller);
    mpp_log("mode %s\n", mode2str[group->mode]);
    mpp_log("type %s\n", type2str[group->type]);
    mpp_log("limit size %d count %d\n", group->limit_size, group->limit_count);

    mpp_log("used buffer count %d\n", group->count_used);

    MppBufferImpl *pos, *n;
    list_for_each_entry_safe(pos, n, &group->list_used, MppBufferImpl, list_status) {
        dump_buffer_info(pos);
    }

    mpp_log("unused buffer count %d\n", group->count_unused);
    list_for_each_entry_safe(pos, n, &group->list_unused, MppBufferImpl, list_status) {
        dump_buffer_info(pos);
    }

    if (group->logs)
        buf_logs_dump(group->logs);
}

void mpp_buffer_service_dump(const char *info)
{
    AutoMutex auto_lock(MppBufferService::get_lock());

    MppBufferService::get_instance()->dump(info);
}

void MppBufferService::inc_total(RK_U32 size)
{
    AutoMutex auto_lock(MppBufferService::get_lock());
    total_size += size;
    if (total_size > total_max)
        total_max = total_size;
}

void MppBufferService::dec_total(RK_U32 size)
{
    AutoMutex auto_lock(MppBufferService::get_lock());
    total_size -= size;
}

RK_U32 mpp_buffer_total_now()
{
    AutoMutex auto_lock(MppBufferService::get_lock());
    return MppBufferService::get_instance()->get_total_now();
}

RK_U32 mpp_buffer_total_max()
{
    AutoMutex auto_lock(MppBufferService::get_lock());
    return MppBufferService::get_instance()->get_total_max();
}

MppBufferGroupImpl *mpp_buffer_get_misc_group(MppBufferMode mode, MppBufferType type)
{
    MppBufferGroupImpl *misc;
    RK_U32 id;

    type = (MppBufferType)(type & MPP_BUFFER_TYPE_MASK);
    if (type == MPP_BUFFER_TYPE_NORMAL)
        return NULL;

    mpp_assert(mode < MPP_BUFFER_MODE_BUTT);
    mpp_assert(type < MPP_BUFFER_TYPE_BUTT);

    AutoMutex auto_lock(MppBufferService::get_lock());

    id = MppBufferService::get_instance()->get_misc(mode, type);
    if (!id) {
        char tag[32];
        RK_S32 offset = 0;

        offset += snprintf(tag + offset, sizeof(tag) - offset, "misc");
        offset += snprintf(tag + offset, sizeof(tag) - offset, "_%s",
                           type == MPP_BUFFER_TYPE_ION ? "ion" :
                           type == MPP_BUFFER_TYPE_DRM ? "drm" : "na");
        offset += snprintf(tag + offset, sizeof(tag) - offset, "_%s",
                           mode == MPP_BUFFER_INTERNAL ? "int" : "ext");

        misc = MppBufferService::get_instance()->get_group(tag, __FUNCTION__, mode, type, 1);
    } else
        misc = MppBufferService::get_instance()->get_group_by_id(id);

    return misc;
}

MppBufferService::MppBufferService()
    : group_id(1),
      group_count(0),
      finalizing(0),
      finished(0),
      total_size(0),
      total_max(0),
      misc_count(0)
{
    RK_S32 i, j;

    INIT_LIST_HEAD(&mListGroup);
    INIT_LIST_HEAD(&mListOrphan);

    // NOTE: Do not create misc group at beginning. Only create on when needed.
    for (i = 0; i < MPP_BUFFER_MODE_BUTT; i++)
        for (j = 0; j < MPP_BUFFER_TYPE_BUTT; j++)
            misc[i][j] = 0;

    for (i = 0; i < (RK_S32)HASH_SIZE(mHashGroup); i++)
        INIT_HLIST_HEAD(&mHashGroup[i]);

    for (i = 0; i < MPP_BUFFER_TYPE_BUTT; i++)
        mpp_allocator_get(&mAllocator[i], &mAllocatorApi[i], (MppBufferType)i);
}

MppBufferService::~MppBufferService()
{
    RK_S32 i, j;

    finalizing = 1;

    // first remove legacy group which is the normal case
    if (misc_count) {
        mpp_log_f("cleaning misc group\n");
        for (i = 0; i < MPP_BUFFER_MODE_BUTT; i++)
            for (j = 0; j < MPP_BUFFER_TYPE_BUTT; j++) {
                RK_U32 id = misc[i][j];

                if (id) {
                    put_group(__FUNCTION__, get_group_by_id(id));
                    misc[i][j] = 0;
                }
            }
    }

    // then remove the remaining group which is the leak one
    if (!list_empty(&mListGroup)) {
        MppBufferGroupImpl *pos, *n;

        mpp_log_f("cleaning leaked group\n");
        list_for_each_entry_safe(pos, n, &mListGroup, MppBufferGroupImpl, list_group) {
            put_group(__FUNCTION__, pos);
        }
    }

    // remove all orphan buffer group
    if (!list_empty(&mListOrphan)) {
        MppBufferGroupImpl *pos, *n;

        mpp_log_f("cleaning leaked buffer\n");

        list_for_each_entry_safe(pos, n, &mListOrphan, MppBufferGroupImpl, list_group) {
            pos->clear_on_exit = 1;
            pos->is_finalizing = 1;
            put_group(__FUNCTION__, pos);
        }
    }
    finished = 1;

    for (i = 0; i < MPP_BUFFER_TYPE_BUTT; i++)
        mpp_allocator_put(&mAllocator[i]);
}

RK_U32 MppBufferService::get_group_id()
{
    RK_U32 id;
    static RK_U32 overflowed = 0;

    /* avoid 0 group id */
    if (!group_id)
        group_id++;

    id = group_id++;
    /* check overflow */
    if (!id) {
        overflowed = 1;
        id = group_id++;
    }

    // avoid group_id reuse
    if (overflowed) {
        /* when it is overflow avoid the used id */
        while (get_group_by_id(id))
            id = group_id++;
    }

    group_count++;

    return id;
}

MppBufferGroupImpl *MppBufferService::get_group(const char *tag, const char *caller,
                                                MppBufferMode mode, MppBufferType type,
                                                RK_U32 is_misc)
{
    MppBufferType buffer_type = (MppBufferType)(type & MPP_BUFFER_TYPE_MASK);
    MppBufferGroupImpl *p = mpp_calloc(MppBufferGroupImpl, 1);
    if (NULL == p) {
        mpp_err("MppBufferService failed to allocate group context\n");
        return NULL;
    }

    AutoMutex auto_lock(get_lock());
    RK_U32 id = get_group_id();

    INIT_LIST_HEAD(&p->list_group);
    INIT_LIST_HEAD(&p->list_used);
    INIT_LIST_HEAD(&p->list_unused);
    INIT_HLIST_NODE(&p->hlist);

    mpp_env_get_u32("mpp_buffer_debug", &mpp_buffer_debug, 0);
    p->log_runtime_en   = (mpp_buffer_debug & MPP_BUF_DBG_OPS_RUNTIME) ? (1) : (0);
    p->log_history_en   = (mpp_buffer_debug & MPP_BUF_DBG_OPS_HISTORY) ? (1) : (0);

    if (tag) {
        snprintf(p->tag, sizeof(p->tag), "%s_%d", tag, id);
    } else {
        snprintf(p->tag, sizeof(p->tag), "unknown");
    }
    p->caller   = caller;
    p->mode     = mode;
    p->type     = buffer_type;
    p->limit    = BUFFER_GROUP_SIZE_DEFAULT;
    p->group_id = id;
    p->clear_on_exit = (mpp_buffer_debug & MPP_BUF_DBG_CLR_ON_EXIT) ? (1) : (0);
    p->dump_on_exit  = (mpp_buffer_debug & MPP_BUF_DBG_DUMP_ON_EXIT) ? (1) : (0);

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&p->buf_lock, &attr);
    pthread_mutexattr_destroy(&attr);

    list_add_tail(&p->list_group, &mListGroup);
    hash_add(mHashGroup, &p->hlist, hash_32(p->group_id, MAX_GROUP_BIT));

    p->allocator = mAllocator[type];
    p->alloc_api = mAllocatorApi[type];

    mpp_assert(p->allocator);
    mpp_assert(p->alloc_api);

    if (p->log_history_en)
        p->logs = buf_logs_init(BUFFER_OPS_MAX_COUNT);

    buf_grp_add_log(p, GRP_CREATE, caller);

    mpp_assert(mode < MPP_BUFFER_MODE_BUTT);
    mpp_assert(buffer_type < MPP_BUFFER_TYPE_BUTT);

    if (is_misc) {
        misc[mode][buffer_type] = id;
        p->is_misc = 1;
        misc_count++;
    }

    return p;
}

RK_U32 MppBufferService::get_misc(MppBufferMode mode, MppBufferType type)
{
    type = (MppBufferType)(type & MPP_BUFFER_TYPE_MASK);
    if (type == MPP_BUFFER_TYPE_NORMAL)
        return 0;

    mpp_assert(mode < MPP_BUFFER_MODE_BUTT);
    mpp_assert(type < MPP_BUFFER_TYPE_BUTT);

    return misc[mode][type];
}

void MppBufferService::put_group(const char *caller, MppBufferGroupImpl *p)
{
    if (finished)
        return ;

    AutoMutex auto_lock(get_lock());
    buf_grp_add_log(p, GRP_RELEASE, caller);

    // remove unused list
    if (!list_empty(&p->list_unused)) {
        MppBufferImpl *pos, *n;
        list_for_each_entry_safe(pos, n, &p->list_unused, MppBufferImpl, list_status) {
            deinit_buffer_no_lock(pos, caller);
            p->count_unused--;
        }
    }

    if (list_empty(&p->list_used)) {
        destroy_group(p);
    } else {
        if (!finalizing || (finalizing && p->dump_on_exit)) {
            mpp_err("mpp_group %p tag %s caller %s mode %s type %s deinit with %d bytes not released\n",
                    p, p->tag, p->caller, mode2str[p->mode], type2str[p->type], p->usage);

            mpp_buffer_group_dump(p, caller);
        }

        /* if clear on exit we need to release remaining buffer */
        if (p->clear_on_exit) {
            MppBufferImpl *pos, *n;

            if (p->dump_on_exit)
                mpp_err("force release all remaining buffer\n");

            list_for_each_entry_safe(pos, n, &p->list_used, MppBufferImpl, list_status) {
                if (p->dump_on_exit)
                    mpp_err("clearing buffer %p\n", pos);
                pos->ref_count = 0;
                pos->used = 0;
                pos->discard = 0;
                deinit_buffer_no_lock(pos, caller);
                p->count_used--;
            }

            destroy_group(p);
        } else {
            // otherwise move the group to list_orphan and wait for buffer release
            buf_grp_add_log(p, GRP_ORPHAN, caller);
            list_del_init(&p->list_group);
            list_add_tail(&p->list_group, &mListOrphan);
            p->is_orphan = 1;
        }
    }
}

void MppBufferService::destroy_group(MppBufferGroupImpl *group)
{
    MppBufferMode mode = group->mode;
    MppBufferType type = group->type;
    RK_U32 id = group->group_id;

    mpp_assert(group->count_used == 0);
    mpp_assert(group->count_unused == 0);
    if (group->count_unused || group->count_used) {
        mpp_err("mpp_buffer_group_deinit mismatch counter used %4d unused %4d found\n",
                group->count_used, group->count_unused);
        group->count_unused = 0;
        group->count_used   = 0;
    }

    buf_grp_add_log(group, GRP_DESTROY, __FUNCTION__);

    list_del_init(&group->list_group);
    hash_del(&group->hlist);
    pthread_mutex_destroy(&group->buf_lock);
    if (group->logs) {
        buf_logs_deinit(group->logs);
        group->logs = NULL;
    }
    mpp_free(group);
    group_count--;

    if (id == misc[mode][type]) {
        misc[mode][type] = 0;
        misc_count--;
    }
}

MppBufferGroupImpl *MppBufferService::get_group_by_id(RK_U32 id)
{
    MppBufferGroupImpl *impl = NULL;
    RK_U32 key = hash_32(id, MAX_GROUP_BIT);

    hash_for_each_possible(mHashGroup, impl, hlist, key) {
        if (impl->group_id == id)
            break;
    }

    return impl;
}

void MppBufferService::dump(const char *info)
{
    MppBufferGroupImpl *group;
    struct hlist_node *n;
    RK_U32 key;

    mpp_log("dumping all buffer groups for %s\n", info);

    if (hash_empty(mHashGroup)) {
        mpp_log("no buffer group can be dumped\n");
    } else {
        hash_for_each_safe(mHashGroup, key, n, group, hlist) {
            mpp_buffer_group_dump(group, __FUNCTION__);
        }
    }
}

RK_U32 MppBufferService::is_finalizing()
{
    return finalizing;
}
