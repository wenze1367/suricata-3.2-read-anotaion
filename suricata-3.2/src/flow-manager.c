/* Copyright (C) 2007-2013 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Anoop Saldanha <anoopsaldanha@gmail.com>
 * \author Victor Julien <victor@inliniac.net>
 */

#include "suricata-common.h"
#include "suricata.h"
#include "decode.h"
#include "conf.h"
#include "threadvars.h"
#include "tm-threads.h"
#include "runmodes.h"

#include "util-random.h"
#include "util-time.h"

#include "flow.h"
#include "flow-queue.h"
#include "flow-hash.h"
#include "flow-util.h"
#include "flow-var.h"
#include "flow-private.h"
#include "flow-timeout.h"
#include "flow-manager.h"

#include "stream-tcp-private.h"
#include "stream-tcp-reassemble.h"
#include "stream-tcp.h"

#include "util-unittest.h"
#include "util-unittest-helper.h"
#include "util-byte.h"

#include "util-debug.h"
#include "util-privs.h"
#include "util-signal.h"

#include "threads.h"
#include "detect.h"
#include "detect-engine-state.h"
#include "stream.h"

#include "app-layer-parser.h"

#include "host-timeout.h"
#include "defrag-timeout.h"
#include "ippair-timeout.h"

#include "output-flow.h"

/* Run mode selected at suricata.c */
extern int run_mode;

/* multi flow mananger support */
static uint32_t flowmgr_number = 1;
/* atomic counter for flow managers, to assign instance id */
SC_ATOMIC_DECLARE(uint32_t, flowmgr_cnt);

/* multi flow recycler support */
static uint32_t flowrec_number = 1;
/* atomic counter for flow recyclers, to assign instance id */
SC_ATOMIC_DECLARE(uint32_t, flowrec_cnt);

SC_ATOMIC_EXTERN(unsigned int, flow_flags);


typedef FlowProtoTimeout *FlowProtoTimeoutPtr;
SC_ATOMIC_DECLARE(FlowProtoTimeoutPtr, flow_timeouts);

void FlowTimeoutsInit(void)
{
    SC_ATOMIC_SET(flow_timeouts, flow_timeouts_normal);
}

void FlowTimeoutsEmergency(void)
{
    SC_ATOMIC_SET(flow_timeouts, flow_timeouts_emerg);
}

/* 1 seconds */
#define FLOW_NORMAL_MODE_UPDATE_DELAY_SEC 1
#define FLOW_NORMAL_MODE_UPDATE_DELAY_NSEC 0
/* 0.1 seconds */
#define FLOW_EMERG_MODE_UPDATE_DELAY_SEC 0
#define FLOW_EMERG_MODE_UPDATE_DELAY_NSEC 100000
#define NEW_FLOW_COUNT_COND 10

typedef struct FlowTimeoutCounters_ {
    uint32_t new;
    uint32_t est;
    uint32_t clo;
    uint32_t byp;
    uint32_t tcp_reuse;

    uint32_t flows_checked;
    uint32_t flows_notimeout;
    uint32_t flows_timeout;
    uint32_t flows_timeout_inuse;
    uint32_t flows_removed;

    uint32_t rows_checked;
    uint32_t rows_skipped;
    uint32_t rows_empty;
    uint32_t rows_busy;
    uint32_t rows_maxlen;
} FlowTimeoutCounters;

/**
 * \brief Used to disable flow manager thread(s).
 *
 * \todo Kinda hackish since it uses the tv name to identify flow manager
 *       thread.  We need an all weather identification scheme.
 */
void FlowDisableFlowManagerThread(void)
{
#ifdef AFLFUZZ_DISABLE_MGTTHREADS
    return;
#endif
    ThreadVars *tv = NULL;
    int cnt = 0;

    /* wake up threads */
    uint32_t u;
    for (u = 0; u < flowmgr_number; u++)
        SCCtrlCondSignal(&flow_manager_ctrl_cond);

    SCMutexLock(&tv_root_lock);

    /* flow manager thread(s) is/are a part of mgmt threads */
    tv = tv_root[TVT_MGMT];

    while (tv != NULL)
    {
        if (strncasecmp(tv->name, thread_name_flow_mgr,
            strlen(thread_name_flow_mgr)) == 0)
        {
            TmThreadsSetFlag(tv, THV_KILL);
            cnt++;

            /* value in seconds */
#define THREAD_KILL_MAX_WAIT_TIME 60
            /* value in microseconds */
#define WAIT_TIME 100

            double total_wait_time = 0;
            while (!TmThreadsCheckFlag(tv, THV_RUNNING_DONE)) {
                usleep(WAIT_TIME);
                total_wait_time += WAIT_TIME / 1000000.0;
                if (total_wait_time > THREAD_KILL_MAX_WAIT_TIME) {
                    SCLogError(SC_ERR_FATAL, "Engine unable to "
                            "disable detect thread - \"%s\".  "
                            "Killing engine", tv->name);
                    exit(EXIT_FAILURE);
                }
            }
        }
        tv = tv->next;
    }
    SCMutexUnlock(&tv_root_lock);

    /* wake up threads, another try */
    for (u = 0; u < flowmgr_number; u++)
        SCCtrlCondSignal(&flow_manager_ctrl_cond);

    /* reset count, so we can kill and respawn (unix socket) */
    SC_ATOMIC_SET(flowmgr_cnt, 0);
    return;
}

/** \internal
 *  \brief get timeout for flow
 *
 *  \param f flow
 *  \param state flow state
 *
 *  \retval timeout timeout in seconds
 */
static inline uint32_t FlowGetFlowTimeout(const Flow *f, enum FlowState state)
{
    uint32_t timeout;
    FlowProtoTimeoutPtr flow_timeouts = SC_ATOMIC_GET(flow_timeouts);
    switch(state) {
        default:
        case FLOW_STATE_NEW:
            timeout = flow_timeouts[f->protomap].new_timeout;
            break;
        case FLOW_STATE_ESTABLISHED:
            timeout = flow_timeouts[f->protomap].est_timeout;
            break;
        case FLOW_STATE_CLOSED:
            timeout = flow_timeouts[f->protomap].closed_timeout;
            break;
        case FLOW_STATE_CAPTURE_BYPASSED:
            timeout = FLOW_BYPASSED_TIMEOUT;
            break;
        case FLOW_STATE_LOCAL_BYPASSED:
            timeout = flow_timeouts[f->protomap].bypassed_timeout;
            break;
    }
    return timeout;
}

/** \internal
 *  \brief check if a flow is timed out
 *
 *  \param f flow
 *  \param ts timestamp
 *
 *  \retval 0 not timed out
 *  \retval 1 timed out
 */
static int FlowManagerFlowTimeout(const Flow *f, enum FlowState state, struct timeval *ts, int32_t *next_ts)
{
    /* set the timeout value according to the flow operating mode,
     * flow's state and protocol.*/
    uint32_t timeout = FlowGetFlowTimeout(f, state);

    int32_t flow_times_out_at = (int32_t)(f->lastts.tv_sec + timeout);
    if (*next_ts == 0 || flow_times_out_at < *next_ts)
        *next_ts = flow_times_out_at;

    /* do the timeout check */
    if (flow_times_out_at >= ts->tv_sec) {
        return 0;
    }

    return 1;
}

/** \internal
 *  \brief See if we can really discard this flow. Check use_cnt reference
 *         counter and force reassembly if necessary.
 *
 *  \param f flow
 *  \param ts timestamp
 *
 *  \retval 0 not timed out just yet
 *  \retval 1 fully timed out, lets kill it
 */
static int FlowManagerFlowTimedOut(Flow *f, struct timeval *ts)
{
    /* never prune a flow that is used by a packet we
     * are currently processing in one of the threads */
    if (SC_ATOMIC_GET(f->use_cnt) > 0) {
        return 0;
    }

    int server = 0, client = 0;

    if (!(f->flags & FLOW_TIMEOUT_REASSEMBLY_DONE) &&
            FlowForceReassemblyNeedReassembly(f, &server, &client) == 1) {
        FlowForceReassemblyForFlow(f, server, client);
        return 0;
    }
#ifdef DEBUG
    /* this should not be possible */
    BUG_ON(SC_ATOMIC_GET(f->use_cnt) > 0);
#endif

    return 1;
}

/**
 *  \internal
 *
 *  \brief check all flows in a hash row for timing out
 *
 *  \param f last flow in the hash row
 *  \param ts timestamp
 *  \param emergency bool indicating emergency mode
 *  \param counters ptr to FlowTimeoutCounters structure
 *
 *  \retval cnt timed out flows.
 
 1.从后往前（Why?）对flow链表进行遍历。
 2.若使用trywrlock对flow上读写锁失败，则跳过。
 3.调用FlowGetFlowState，获取流的状态（NEW、ESTABLISHED、CLOSED）。
 4.调用FlowManagerFlowTimeout，判断该流失否应该超时，若不应该则解锁并跳过。
 首先需要用FlowGetFlowTimeout根据协议、状态和是否紧急来获取超时时间，
 然后看当前时间距离流的上一次更新时间lastts_sec是否已经大于这个超时时间。
 5.调用FlowManagerFlowTimedOut，看是否可以把丢弃这个流。若有use_cnt则不能丢弃，
 若正处在重组过程中也不能丢弃，并且会调FlowForceReassemblyForFlowV2去强制重组
 （只有重组完成后数据才会交给应用层分析），这样下次再唤醒后就能删除了。
 6.若可以丢弃，则从双向链表中删除这个流节点，然后调用FlowClearMemory清理该流所占内存，
 然后解锁并调用FlowMoveToSpare把它放到空闲流队列中去。
 */
static uint32_t FlowManagerHashRowTimeout(Flow *f, struct timeval *ts,
        int emergency, FlowTimeoutCounters *counters, int32_t *next_ts)
{
    uint32_t cnt = 0;
    uint32_t checked = 0;

    do {
        checked++;

        /* check flow timeout based on lastts and state. Both can be
         * accessed w/o Flow lock as we do have the hash row lock (so flow
         * can't disappear) and flow_state is atomic. lastts can only
         * be modified when we have both the flow and hash row lock */

        enum FlowState state = SC_ATOMIC_GET(f->flow_state);

        /* timeout logic goes here */
        if (FlowManagerFlowTimeout(f, state, ts, next_ts) == 0) {

            counters->flows_notimeout++;

            f = f->hprev;
            continue;
        }

        /* before grabbing the flow lock, make sure we have at least
         * 3 packets in the pool */
        PacketPoolWaitForN(3);

        FLOWLOCK_WRLOCK(f);

        Flow *next_flow = f->hprev;

        counters->flows_timeout++;

        /* check if the flow is fully timed out and
         * ready to be discarded. */
        if (FlowManagerFlowTimedOut(f, ts) == 1) {
            /* remove from the hash */
            if (f->hprev != NULL)
                f->hprev->hnext = f->hnext;
            if (f->hnext != NULL)
                f->hnext->hprev = f->hprev;
            if (f->fb->head == f)
                f->fb->head = f->hnext;
            if (f->fb->tail == f)
                f->fb->tail = f->hprev;

            f->hnext = NULL;
            f->hprev = NULL;

            if (f->flags & FLOW_TCP_REUSED)
                counters->tcp_reuse++;

            if (state == FLOW_STATE_NEW)
                f->flow_end_flags |= FLOW_END_FLAG_STATE_NEW;
            else if (state == FLOW_STATE_ESTABLISHED)
                f->flow_end_flags |= FLOW_END_FLAG_STATE_ESTABLISHED;
            else if (state == FLOW_STATE_CLOSED)
                f->flow_end_flags |= FLOW_END_FLAG_STATE_CLOSED;
            else if (state == FLOW_STATE_LOCAL_BYPASSED)
                f->flow_end_flags |= FLOW_END_FLAG_STATE_BYPASSED;
            else if (state == FLOW_STATE_CAPTURE_BYPASSED)
                f->flow_end_flags |= FLOW_END_FLAG_STATE_BYPASSED;

            if (emergency)
                f->flow_end_flags |= FLOW_END_FLAG_EMERGENCY;
            f->flow_end_flags |= FLOW_END_FLAG_TIMEOUT;

            /* no one is referring to this flow, use_cnt 0, removed from hash
             * so we can unlock it and pass it to the flow recycler */
            FLOWLOCK_UNLOCK(f);
            FlowEnqueue(&flow_recycle_q, f);

            cnt++;

            switch (state) {
                case FLOW_STATE_NEW:
                default:
                    counters->new++;
                    break;
                case FLOW_STATE_ESTABLISHED:
                    counters->est++;
                    break;
                case FLOW_STATE_CLOSED:
                    counters->clo++;
                    break;
                case FLOW_STATE_LOCAL_BYPASSED:
                case FLOW_STATE_CAPTURE_BYPASSED:
                    counters->byp++;
                    break;
            }
            counters->flows_removed++;
        } else {
            counters->flows_timeout_inuse++;
            FLOWLOCK_UNLOCK(f);
        }

        f = next_flow;
    } while (f != NULL);

    counters->flows_checked += checked;
    if (checked > counters->rows_maxlen)
        counters->rows_maxlen = checked;

    return cnt;
}

/**
 *  \brief time out flows from the hash
 *
 *  \param ts timestamp
 *  \param try_cnt number of flows to time out max (0 is unlimited)
 *  \param hash_min min hash index to consider
 *  \param hash_max max hash index to consider
 *  \param counters ptr to FlowTimeoutCounters structure
 *
 *  \retval cnt number of timed out flow.
1.循环遍历整个hash表，获取FlowBucket。
2.若使用trylock尝试锁上FlowBucket失败，说明有其他线程正在该bucket上进行操作，
则不去管这个bucket了，继续循环。原因？可能是因为超时的目的毕竟只是回收资源，
并不是强制的，因此直接跳过去而不是等待解锁，更节省CPU时间。
3.若这个bucket上没流，则解锁，然后继续循环。
4.调用FlowManagerHashRowTimeout对这个bucket上的flow链表（Row）进行超时。
5.扫描完整个hash表后，就可以退出了，返回值为删除的超时流数目。
 */
static uint32_t FlowTimeoutHash(struct timeval *ts, uint32_t try_cnt,
        uint32_t hash_min, uint32_t hash_max,
        FlowTimeoutCounters *counters)
{
    uint32_t idx = 0;
    uint32_t cnt = 0;
    int emergency = 0;

    if (SC_ATOMIC_GET(flow_flags) & FLOW_EMERGENCY)
        emergency = 1;

    for (idx = hash_min; idx < hash_max; idx++) {
        FlowBucket *fb = &flow_hash[idx];

        counters->rows_checked++;

        int32_t check_ts = SC_ATOMIC_GET(fb->next_ts);
        if (check_ts > (int32_t)ts->tv_sec) {
            counters->rows_skipped++;
            continue;
        }

        /* before grabbing the row lock, make sure we have at least
         * 9 packets in the pool */
        PacketPoolWaitForN(9);

        if (FBLOCK_TRYLOCK(fb) != 0) {
            counters->rows_busy++;
            continue;
        }

        /* flow hash bucket is now locked */

        if (fb->tail == NULL) {
            SC_ATOMIC_SET(fb->next_ts, INT_MAX);
            counters->rows_empty++;
            goto next;
        }

        int32_t next_ts = 0;

        /* we have a flow, or more than one */
        cnt += FlowManagerHashRowTimeout(fb->tail, ts, emergency, counters, &next_ts);

        SC_ATOMIC_SET(fb->next_ts, next_ts);

next:
        FBLOCK_UNLOCK(fb);

        if (try_cnt > 0 && cnt >= try_cnt)
            break;
    }

    return cnt;
}

/**
 *  \internal
 *
 *  \brief move all flows out of a hash row
 *
 *  \param f last flow in the hash row
 *
 *  \retval cnt removed out flows
 */
static uint32_t FlowManagerHashRowCleanup(Flow *f)
{
    uint32_t cnt = 0;

    do {
        FLOWLOCK_WRLOCK(f);

        Flow *next_flow = f->hprev;

        int state = SC_ATOMIC_GET(f->flow_state);

        /* remove from the hash */
        if (f->hprev != NULL)
            f->hprev->hnext = f->hnext;
        if (f->hnext != NULL)
            f->hnext->hprev = f->hprev;
        if (f->fb->head == f)
            f->fb->head = f->hnext;
        if (f->fb->tail == f)
            f->fb->tail = f->hprev;

        f->hnext = NULL;
        f->hprev = NULL;

        if (state == FLOW_STATE_NEW)
            f->flow_end_flags |= FLOW_END_FLAG_STATE_NEW;
        else if (state == FLOW_STATE_ESTABLISHED)
            f->flow_end_flags |= FLOW_END_FLAG_STATE_ESTABLISHED;
        else if (state == FLOW_STATE_CLOSED)
            f->flow_end_flags |= FLOW_END_FLAG_STATE_CLOSED;

        f->flow_end_flags |= FLOW_END_FLAG_SHUTDOWN;

        /* no one is referring to this flow, use_cnt 0, removed from hash
         * so we can unlock it and move it to the recycle queue. */
        FLOWLOCK_UNLOCK(f);

        FlowEnqueue(&flow_recycle_q, f);

        cnt++;

        f = next_flow;
    } while (f != NULL);

    return cnt;
}

/**
 *  \brief remove all flows from the hash
 *
 *  \retval cnt number of removes out flows
 */
static uint32_t FlowCleanupHash(void){
    uint32_t idx = 0;
    uint32_t cnt = 0;

    for (idx = 0; idx < flow_config.hash_size; idx++) {
        FlowBucket *fb = &flow_hash[idx];

        FBLOCK_LOCK(fb);

        if (fb->tail != NULL) {
            /* we have a flow, or more than one */
            cnt += FlowManagerHashRowCleanup(fb->tail);
        }

        FBLOCK_UNLOCK(fb);
    }

    return cnt;
}

extern int g_detect_disabled;

typedef struct FlowManagerThreadData_ {
    uint32_t instance;
    uint32_t min;
    uint32_t max;

    uint16_t flow_mgr_cnt_clo;
    uint16_t flow_mgr_cnt_new;
    uint16_t flow_mgr_cnt_est;
    uint16_t flow_mgr_cnt_byp;
    uint16_t flow_mgr_spare;
    uint16_t flow_emerg_mode_enter;
    uint16_t flow_emerg_mode_over;
    uint16_t flow_tcp_reuse;

    uint16_t flow_mgr_flows_checked;
    uint16_t flow_mgr_flows_notimeout;
    uint16_t flow_mgr_flows_timeout;
    uint16_t flow_mgr_flows_timeout_inuse;
    uint16_t flow_mgr_flows_removed;

    uint16_t flow_mgr_rows_checked;
    uint16_t flow_mgr_rows_skipped;
    uint16_t flow_mgr_rows_empty;
    uint16_t flow_mgr_rows_busy;
    uint16_t flow_mgr_rows_maxlen;

} FlowManagerThreadData;

static TmEcode FlowManagerThreadInit(ThreadVars *t, void *initdata, void **data)
{
    FlowManagerThreadData *ftd = SCCalloc(1, sizeof(FlowManagerThreadData));
    if (ftd == NULL)
        return TM_ECODE_FAILED;

    ftd->instance = SC_ATOMIC_ADD(flowmgr_cnt, 1);
    SCLogDebug("flow manager instance %u", ftd->instance);

    /* set the min and max value used for hash row walking
     * each thread has it's own section of the flow hash */
    uint32_t range = flow_config.hash_size / flowmgr_number;
    if (ftd->instance == 1)
        ftd->max = range;
    else if (ftd->instance == flowmgr_number) {
        ftd->min = (range * (ftd->instance - 1));
        ftd->max = flow_config.hash_size;
    } else {
        ftd->min = (range * (ftd->instance - 1));
        ftd->max = (range * ftd->instance);
    }
    BUG_ON(ftd->min > flow_config.hash_size || ftd->max > flow_config.hash_size);

    SCLogDebug("instance %u hash range %u %u", ftd->instance, ftd->min, ftd->max);

    /* pass thread data back to caller */
    *data = ftd;

    ftd->flow_mgr_cnt_clo = StatsRegisterCounter("flow_mgr.closed_pruned", t);
    ftd->flow_mgr_cnt_new = StatsRegisterCounter("flow_mgr.new_pruned", t);
    ftd->flow_mgr_cnt_est = StatsRegisterCounter("flow_mgr.est_pruned", t);
    ftd->flow_mgr_cnt_byp = StatsRegisterCounter("flow_mgr.bypassed_pruned", t);
    ftd->flow_mgr_spare = StatsRegisterCounter("flow.spare", t);
    ftd->flow_emerg_mode_enter = StatsRegisterCounter("flow.emerg_mode_entered", t);
    ftd->flow_emerg_mode_over = StatsRegisterCounter("flow.emerg_mode_over", t);
    ftd->flow_tcp_reuse = StatsRegisterCounter("flow.tcp_reuse", t);

    ftd->flow_mgr_flows_checked = StatsRegisterCounter("flow_mgr.flows_checked", t);
    ftd->flow_mgr_flows_notimeout = StatsRegisterCounter("flow_mgr.flows_notimeout", t);
    ftd->flow_mgr_flows_timeout = StatsRegisterCounter("flow_mgr.flows_timeout", t);
    ftd->flow_mgr_flows_timeout_inuse = StatsRegisterCounter("flow_mgr.flows_timeout_inuse", t);
    ftd->flow_mgr_flows_removed = StatsRegisterCounter("flow_mgr.flows_removed", t);

    ftd->flow_mgr_rows_checked = StatsRegisterCounter("flow_mgr.rows_checked", t);
    ftd->flow_mgr_rows_skipped = StatsRegisterCounter("flow_mgr.rows_skipped", t);
    ftd->flow_mgr_rows_empty = StatsRegisterCounter("flow_mgr.rows_empty", t);
    ftd->flow_mgr_rows_busy = StatsRegisterCounter("flow_mgr.rows_busy", t);
    ftd->flow_mgr_rows_maxlen = StatsRegisterCounter("flow_mgr.rows_maxlen", t);

    PacketPoolInit();
    return TM_ECODE_OK;
}

static TmEcode FlowManagerThreadDeinit(ThreadVars *t, void *data)
{
    PacketPoolDestroy();
    SCFree(data);
    return TM_ECODE_OK;
}


/** \brief Thread that manages the flow table and times out flows.
 *
 *  \param td ThreadVars casted to void ptr
 *
 *  Keeps an eye on the spare list, alloc flows if needed...
 流超时
流超时的实现由FlowManagerThread实现，该线程执行流程如下（通用的线程相关的处理未做记录）：
1.调用FlowForceReassemblySetup为强制重组功能进行设置。
2.进入主循环。
3.首先检查flow_flags是否有FLOW_EMERGENCY标志，若有则打开emerg开关，表明进入紧急状态（内存吃紧）。
4.调用FlowUpdateSpareFlows，保证当前空闲流的数量正好等于prealloc，少的就alloc，
多的就free。这样做应该是希望在性能和内存占用之间维持平衡。
5.流超时：调用FlowTimeoutHash进行实际的流超时处理。
6.其他超时：DefragTimeoutHash、HostTimeoutHash，与flow engine无关，估计是为了方便就放这里一起做了。
7.紧急情况处理：若emerg开关打开了，则进入紧急状态，将流超时的唤醒时间设置为1ms。
若当前已经删除了emergency_recovery比率的流，就退出紧急模式并恢复正常唤醒时间（1s）s。
8.接下来，就进入cond_timewait状态，如此循环下去。
9.若收到KILL信号而退出循环，进而结束进程。
 */
static TmEcode FlowManager(ThreadVars *th_v, void *thread_data)
{
    /* block usr2.  usr1 to be handled by the main thread only */
    UtilSignalBlock(SIGUSR2);

    FlowManagerThreadData *ftd = thread_data;
    struct timeval ts;
    uint32_t established_cnt = 0, new_cnt = 0, closing_cnt = 0;
    int emerg = FALSE;
    int prev_emerg = FALSE;
    struct timespec cond_time;
    int flow_update_delay_sec = FLOW_NORMAL_MODE_UPDATE_DELAY_SEC;
    int flow_update_delay_nsec = FLOW_NORMAL_MODE_UPDATE_DELAY_NSEC;
/* VJ leaving disabled for now, as hosts are only used by tags and the numbers
 * are really low. Might confuse ppl
    uint16_t flow_mgr_host_prune = StatsRegisterCounter("hosts.pruned", th_v);
    uint16_t flow_mgr_host_active = StatsRegisterCounter("hosts.active", th_v);
    uint16_t flow_mgr_host_spare = StatsRegisterCounter("hosts.spare", th_v);
*/
    memset(&ts, 0, sizeof(ts));

    while (1)
    {
        if (TmThreadsCheckFlag(th_v, THV_PAUSE)) {
            TmThreadsSetFlag(th_v, THV_PAUSED);
            TmThreadTestThreadUnPaused(th_v);
            TmThreadsUnsetFlag(th_v, THV_PAUSED);
        }

        if (SC_ATOMIC_GET(flow_flags) & FLOW_EMERGENCY) {
            emerg = TRUE;

            if (emerg == TRUE && prev_emerg == FALSE) {
                prev_emerg = TRUE;

                SCLogDebug("Flow emergency mode entered...");

                StatsIncr(th_v, ftd->flow_emerg_mode_enter);
            }
        }

        /* Get the time */
        memset(&ts, 0, sizeof(ts));
        TimeGet(&ts);
        SCLogDebug("ts %" PRIdMAX "", (intmax_t)ts.tv_sec);

        /* see if we still have enough spare flows */
        if (ftd->instance == 1)
            FlowUpdateSpareFlows();

        /* try to time out flows */
        FlowTimeoutCounters counters = { 0, 0, 0, 0, 0,0,0,0,0,0,0,0,0,0,0};
        FlowTimeoutHash(&ts, 0 /* check all */, ftd->min, ftd->max, &counters);


        if (ftd->instance == 1) {
            DefragTimeoutHash(&ts);
            //uint32_t hosts_pruned =
            HostTimeoutHash(&ts);
            IPPairTimeoutHash(&ts);
        }
/*
        StatsAddUI64(th_v, flow_mgr_host_prune, (uint64_t)hosts_pruned);
        uint32_t hosts_active = HostGetActiveCount();
        StatsSetUI64(th_v, flow_mgr_host_active, (uint64_t)hosts_active);
        uint32_t hosts_spare = HostGetSpareCount();
        StatsSetUI64(th_v, flow_mgr_host_spare, (uint64_t)hosts_spare);
*/
        StatsAddUI64(th_v, ftd->flow_mgr_cnt_clo, (uint64_t)counters.clo);
        StatsAddUI64(th_v, ftd->flow_mgr_cnt_new, (uint64_t)counters.new);
        StatsAddUI64(th_v, ftd->flow_mgr_cnt_est, (uint64_t)counters.est);
        StatsAddUI64(th_v, ftd->flow_mgr_cnt_byp, (uint64_t)counters.byp);
        StatsAddUI64(th_v, ftd->flow_tcp_reuse, (uint64_t)counters.tcp_reuse);

        StatsSetUI64(th_v, ftd->flow_mgr_flows_checked, (uint64_t)counters.flows_checked);
        StatsSetUI64(th_v, ftd->flow_mgr_flows_notimeout, (uint64_t)counters.flows_notimeout);
        StatsSetUI64(th_v, ftd->flow_mgr_flows_timeout, (uint64_t)counters.flows_timeout);
        StatsSetUI64(th_v, ftd->flow_mgr_flows_removed, (uint64_t)counters.flows_removed);
        StatsSetUI64(th_v, ftd->flow_mgr_flows_timeout_inuse, (uint64_t)counters.flows_timeout_inuse);

        StatsSetUI64(th_v, ftd->flow_mgr_rows_checked, (uint64_t)counters.rows_checked);
        StatsSetUI64(th_v, ftd->flow_mgr_rows_skipped, (uint64_t)counters.rows_skipped);
        StatsSetUI64(th_v, ftd->flow_mgr_rows_maxlen, (uint64_t)counters.rows_maxlen);
        StatsSetUI64(th_v, ftd->flow_mgr_rows_busy, (uint64_t)counters.rows_busy);
        StatsSetUI64(th_v, ftd->flow_mgr_rows_empty, (uint64_t)counters.rows_empty);

        uint32_t len = 0;
        FQLOCK_LOCK(&flow_spare_q);
        len = flow_spare_q.len;
        FQLOCK_UNLOCK(&flow_spare_q);
        StatsSetUI64(th_v, ftd->flow_mgr_spare, (uint64_t)len);

        /* Don't fear, FlowManagerThread is here...
         * clear emergency bit if we have at least xx flows pruned. */
        if (emerg == TRUE) {
            SCLogDebug("flow_sparse_q.len = %"PRIu32" prealloc: %"PRIu32
                       "flow_spare_q status: %"PRIu32"%% flows at the queue",
                       len, flow_config.prealloc, len * 100 / flow_config.prealloc);
            /* only if we have pruned this "emergency_recovery" percentage
             * of flows, we will unset the emergency bit */
            if (len * 100 / flow_config.prealloc > flow_config.emergency_recovery) {
                SC_ATOMIC_AND(flow_flags, ~FLOW_EMERGENCY);

                FlowTimeoutsReset();

                emerg = FALSE;
                prev_emerg = FALSE;

                flow_update_delay_sec = FLOW_NORMAL_MODE_UPDATE_DELAY_SEC;
                flow_update_delay_nsec = FLOW_NORMAL_MODE_UPDATE_DELAY_NSEC;
                SCLogInfo("Flow emergency mode over, back to normal... unsetting"
                          " FLOW_EMERGENCY bit (ts.tv_sec: %"PRIuMAX", "
                          "ts.tv_usec:%"PRIuMAX") flow_spare_q status(): %"PRIu32
                          "%% flows at the queue", (uintmax_t)ts.tv_sec,
                          (uintmax_t)ts.tv_usec, len * 100 / flow_config.prealloc);

                StatsIncr(th_v, ftd->flow_emerg_mode_over);
            } else {
                flow_update_delay_sec = FLOW_EMERG_MODE_UPDATE_DELAY_SEC;
                flow_update_delay_nsec = FLOW_EMERG_MODE_UPDATE_DELAY_NSEC;
            }
        }

        if (TmThreadsCheckFlag(th_v, THV_KILL)) {
            StatsSyncCounters(th_v);
            break;
        }

        cond_time.tv_sec = time(NULL) + flow_update_delay_sec;
        cond_time.tv_nsec = flow_update_delay_nsec;
        SCCtrlMutexLock(&flow_manager_ctrl_mutex);
        SCCtrlCondTimedwait(&flow_manager_ctrl_cond, &flow_manager_ctrl_mutex,
                            &cond_time);
        SCCtrlMutexUnlock(&flow_manager_ctrl_mutex);

        SCLogDebug("woke up... %s", SC_ATOMIC_GET(flow_flags) & FLOW_EMERGENCY ? "emergency":"");

        StatsSyncCountersIfSignalled(th_v);
    }

    SCLogPerf("%" PRIu32 " new flows, %" PRIu32 " established flows were "
              "timed out, %"PRIu32" flows in closed state", new_cnt,
              established_cnt, closing_cnt);

    return TM_ECODE_OK;
}

static uint64_t FlowGetMemuse(void)
{
    uint64_t flow_memuse = SC_ATOMIC_GET(flow_memuse);
    return flow_memuse;
}

/** \brief spawn the flow manager thread ;
在suricata.c的main函数执行完RunModeDispatch之后会在判断是否使用UNIX_SOCKET中
调用FlowManagerThreadSpawn创建流管理线程。
*/
void FlowManagerThreadSpawn()
{
#ifdef AFLFUZZ_DISABLE_MGTTHREADS
    return;
#endif
    intmax_t setting = 1;
    (void)ConfGetInt("flow.managers", &setting);

	//从配置文件获取希望开开启的flowmanagers线程的个数。
    if (setting < 1 || setting > 1024) {
        SCLogError(SC_ERR_INVALID_ARGUMENTS,
                "invalid flow.managers setting %"PRIdMAX, setting);
        exit(EXIT_FAILURE);
    }
    flowmgr_number = (uint32_t)setting;

    SCLogConfig("using %u flow manager threads", flowmgr_number);

	//内部调用pthread_cond_init初始化条件变量;
    SCCtrlCondInit(&flow_manager_ctrl_cond, NULL);
	
	//内部调用pthread_mutex_init初始化互斥锁
    SCCtrlMutexInit(&flow_manager_ctrl_mutex, NULL);

	//注册"flow.memuse"和回调函数FlowGetMemuse到全局变量stats_ctx的成员列表末尾。
    StatsRegisterGlobalCounter("flow.memuse", FlowGetMemuse);

    uint32_t u;
	
	//默认flowmgr_number为1;
    for (u = 0; u < flowmgr_number; u++)
    {
        ThreadVars *tv_flowmgr = NULL;

        char name[TM_THREAD_NAME_MAX];
        snprintf(name, sizeof(name), "%s#%02u", thread_name_flow_mgr, u+1);

	/*得到一个ThreadVars 变量tv_flowmgr，其中tv_flowmgr->tm_slots = tmm_modules[TMM_FLOWMANAGER]；
	还是最重要的在TmThreadCreate函数中调用TmThreadSetSlots，
	使得tv_flowmgr->tm_func = TmThreadsManagement；TmThreadsManagement是下面TmThreadSpawn创建线程时
	的运行函数，
	流管理的初始化及流管理函数FlowManager都是在TmThreadsManagement里面调的。
	*/
        tv_flowmgr = TmThreadCreateMgmtThreadByName(name,
                "FlowManager", 0);
        BUG_ON(tv_flowmgr == NULL);

        if (tv_flowmgr == NULL) {
            printf("ERROR: TmThreadsCreate failed\n");
            exit(1);
        }
        if (TmThreadSpawn(tv_flowmgr) != TM_ECODE_OK) {
            printf("ERROR: TmThreadSpawn failed\n");
            exit(1);
        }
		//创建流管理线程;
    }
    return;
}

typedef struct FlowRecyclerThreadData_ {
    void *output_thread_data;
} FlowRecyclerThreadData;

static TmEcode FlowRecyclerThreadInit(ThreadVars *t, void *initdata, void **data)
{
    FlowRecyclerThreadData *ftd = SCCalloc(1, sizeof(FlowRecyclerThreadData));
    if (ftd == NULL)
        return TM_ECODE_FAILED;

    if (OutputFlowLogThreadInit(t, NULL, &ftd->output_thread_data) != TM_ECODE_OK) {
        SCLogError(SC_ERR_THREAD_INIT, "initializing flow log API for thread failed");
        SCFree(ftd);
        return TM_ECODE_FAILED;
    }
    SCLogDebug("output_thread_data %p", ftd->output_thread_data);

    *data = ftd;
    return TM_ECODE_OK;
}

static TmEcode FlowRecyclerThreadDeinit(ThreadVars *t, void *data)
{
    FlowRecyclerThreadData *ftd = (FlowRecyclerThreadData *)data;
    if (ftd->output_thread_data != NULL)
        OutputFlowLogThreadDeinit(t, ftd->output_thread_data);

    SCFree(data);
    return TM_ECODE_OK;
}

/** \brief Thread that manages timed out flows.
 *
 *  \param td ThreadVars casted to void ptr
 */
static TmEcode FlowRecycler(ThreadVars *th_v, void *thread_data)
{
    /* block usr2. usr2 to be handled by the main thread only */
    UtilSignalBlock(SIGUSR2);

    struct timeval ts;
    struct timespec cond_time;
    int flow_update_delay_sec = FLOW_NORMAL_MODE_UPDATE_DELAY_SEC;
    int flow_update_delay_nsec = FLOW_NORMAL_MODE_UPDATE_DELAY_NSEC;
    uint64_t recycled_cnt = 0;
    FlowRecyclerThreadData *ftd = (FlowRecyclerThreadData *)thread_data;
    BUG_ON(ftd == NULL);

    memset(&ts, 0, sizeof(ts));

    while (1)
    {
        if (TmThreadsCheckFlag(th_v, THV_PAUSE)) {
            TmThreadsSetFlag(th_v, THV_PAUSED);
            TmThreadTestThreadUnPaused(th_v);
            TmThreadsUnsetFlag(th_v, THV_PAUSED);
        }

        /* Get the time */
        memset(&ts, 0, sizeof(ts));
        TimeGet(&ts);
        SCLogDebug("ts %" PRIdMAX "", (intmax_t)ts.tv_sec);

        uint32_t len = 0;
        FQLOCK_LOCK(&flow_recycle_q);
        len = flow_recycle_q.len;
        FQLOCK_UNLOCK(&flow_recycle_q);

        /* Loop through the queue and clean up all flows in it */
        if (len) {
            Flow *f;

            while ((f = FlowDequeue(&flow_recycle_q)) != NULL) {
                FLOWLOCK_WRLOCK(f);

                (void)OutputFlowLog(th_v, ftd->output_thread_data, f);

                FlowClearMemory (f, f->protomap);
                FLOWLOCK_UNLOCK(f);
                FlowMoveToSpare(f);
                recycled_cnt++;
            }
        }

        SCLogDebug("%u flows to recycle", len);

        if (TmThreadsCheckFlag(th_v, THV_KILL)) {
            StatsSyncCounters(th_v);
            break;
        }

        cond_time.tv_sec = time(NULL) + flow_update_delay_sec;
        cond_time.tv_nsec = flow_update_delay_nsec;
        SCCtrlMutexLock(&flow_recycler_ctrl_mutex);
        SCCtrlCondTimedwait(&flow_recycler_ctrl_cond,
                &flow_recycler_ctrl_mutex, &cond_time);
        SCCtrlMutexUnlock(&flow_recycler_ctrl_mutex);

        SCLogDebug("woke up...");

        StatsSyncCountersIfSignalled(th_v);
    }

    SCLogPerf("%"PRIu64" flows processed", recycled_cnt);

    return TM_ECODE_OK;
}

int FlowRecyclerReadyToShutdown(void)
{
    uint32_t len = 0;
    FQLOCK_LOCK(&flow_recycle_q);
    len = flow_recycle_q.len;
    FQLOCK_UNLOCK(&flow_recycle_q);

    return ((len == 0));
}

/** \brief spawn the flow recycler thread */
void FlowRecyclerThreadSpawn()
{
#ifdef AFLFUZZ_DISABLE_MGTTHREADS
    return;
#endif
    intmax_t setting = 1;
    (void)ConfGetInt("flow.recyclers", &setting);

    if (setting < 1 || setting > 1024) {
        SCLogError(SC_ERR_INVALID_ARGUMENTS,
                "invalid flow.recyclers setting %"PRIdMAX, setting);
        exit(EXIT_FAILURE);
    }
    flowrec_number = (uint32_t)setting;

    SCLogConfig("using %u flow recycler threads", flowrec_number);

    SCCtrlCondInit(&flow_recycler_ctrl_cond, NULL);
    SCCtrlMutexInit(&flow_recycler_ctrl_mutex, NULL);


    uint32_t u;
    for (u = 0; u < flowrec_number; u++)
    {
        ThreadVars *tv_flowmgr = NULL;

        char name[TM_THREAD_NAME_MAX];
        snprintf(name, sizeof(name), "%s#%02u", thread_name_flow_rec, u+1);

        tv_flowmgr = TmThreadCreateMgmtThreadByName(name,
                "FlowRecycler", 0);
        BUG_ON(tv_flowmgr == NULL);

        if (tv_flowmgr == NULL) {
            printf("ERROR: TmThreadsCreate failed\n");
            exit(1);
        }
        if (TmThreadSpawn(tv_flowmgr) != TM_ECODE_OK) {
            printf("ERROR: TmThreadSpawn failed\n");
            exit(1);
        }
    }
    return;
}

/**
 * \brief Used to disable flow recycler thread(s).
 *
 * \note this should only be called when the flow manager is already gone
 *
 * \todo Kinda hackish since it uses the tv name to identify flow recycler
 *       thread.  We need an all weather identification scheme.
 */
void FlowDisableFlowRecyclerThread(void)
{
#ifdef AFLFUZZ_DISABLE_MGTTHREADS
    return;
#endif
    ThreadVars *tv = NULL;
    int cnt = 0;

    /* move all flows still in the hash to the recycler queue */
    FlowCleanupHash();

    /* make sure all flows are processed */
    do {
        SCCtrlCondSignal(&flow_recycler_ctrl_cond);
        usleep(10);
    } while (FlowRecyclerReadyToShutdown() == 0);

    /* wake up threads */
    uint32_t u;
    for (u = 0; u < flowrec_number; u++)
        SCCtrlCondSignal(&flow_recycler_ctrl_cond);

    SCMutexLock(&tv_root_lock);

    /* flow recycler thread(s) is/are a part of mgmt threads */
    tv = tv_root[TVT_MGMT];

    while (tv != NULL)
    {
        if (strncasecmp(tv->name, thread_name_flow_rec,
            strlen(thread_name_flow_rec)) == 0)
        {
            TmThreadsSetFlag(tv, THV_KILL);
            cnt++;

            /* value in seconds */
#define THREAD_KILL_MAX_WAIT_TIME 60
            /* value in microseconds */
#define WAIT_TIME 100

            double total_wait_time = 0;
            while (!TmThreadsCheckFlag(tv, THV_RUNNING_DONE)) {
                usleep(WAIT_TIME);
                total_wait_time += WAIT_TIME / 1000000.0;
                if (total_wait_time > THREAD_KILL_MAX_WAIT_TIME) {
                    SCLogError(SC_ERR_FATAL, "Engine unable to "
                            "disable detect thread - \"%s\".  "
                            "Killing engine", tv->name);
                    exit(EXIT_FAILURE);
                }
            }
        }
        tv = tv->next;
    }

    /* wake up threads, another try */
    for (u = 0; u < flowrec_number; u++)
        SCCtrlCondSignal(&flow_recycler_ctrl_cond);

    SCMutexUnlock(&tv_root_lock);

    /* reset count, so we can kill and respawn (unix socket) */
    SC_ATOMIC_SET(flowrec_cnt, 0);
    return;
}

void TmModuleFlowManagerRegister (void)
{
    tmm_modules[TMM_FLOWMANAGER].name = "FlowManager";
    tmm_modules[TMM_FLOWMANAGER].ThreadInit = FlowManagerThreadInit;
    tmm_modules[TMM_FLOWMANAGER].ThreadDeinit = FlowManagerThreadDeinit;
//    tmm_modules[TMM_FLOWMANAGER].RegisterTests = FlowManagerRegisterTests;
    tmm_modules[TMM_FLOWMANAGER].Management = FlowManager;
    tmm_modules[TMM_FLOWMANAGER].cap_flags = 0;
    tmm_modules[TMM_FLOWMANAGER].flags = TM_FLAG_MANAGEMENT_TM;
    SCLogDebug("%s registered", tmm_modules[TMM_FLOWMANAGER].name);

    SC_ATOMIC_INIT(flowmgr_cnt);
    SC_ATOMIC_INIT(flow_timeouts);
}

void TmModuleFlowRecyclerRegister (void)
{
    tmm_modules[TMM_FLOWRECYCLER].name = "FlowRecycler";
    tmm_modules[TMM_FLOWRECYCLER].ThreadInit = FlowRecyclerThreadInit;
    tmm_modules[TMM_FLOWRECYCLER].ThreadDeinit = FlowRecyclerThreadDeinit;
//    tmm_modules[TMM_FLOWRECYCLER].RegisterTests = FlowRecyclerRegisterTests;
    tmm_modules[TMM_FLOWRECYCLER].Management = FlowRecycler;
    tmm_modules[TMM_FLOWRECYCLER].cap_flags = 0;
    tmm_modules[TMM_FLOWRECYCLER].flags = TM_FLAG_MANAGEMENT_TM;
    SCLogDebug("%s registered", tmm_modules[TMM_FLOWRECYCLER].name);

    SC_ATOMIC_INIT(flowrec_cnt);
}

#ifdef UNITTESTS

/**
 *  \test   Test the timing out of a flow with a fresh TcpSession
 *          (just initialized, no data segments) in normal mode.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int FlowMgrTest01 (void)
{
    TcpSession ssn;
    Flow f;
    FlowBucket fb;
    struct timeval ts;

    FlowQueueInit(&flow_spare_q);

    memset(&ssn, 0, sizeof(TcpSession));
    memset(&f, 0, sizeof(Flow));
    memset(&ts, 0, sizeof(ts));
    memset(&fb, 0, sizeof(FlowBucket));

    FBLOCK_INIT(&fb);

    FLOW_INITIALIZE(&f);
    f.flags |= FLOW_TIMEOUT_REASSEMBLY_DONE;

    TimeGet(&ts);
    f.lastts.tv_sec = ts.tv_sec - 5000;
    f.protoctx = &ssn;
    f.fb = &fb;

    f.proto = IPPROTO_TCP;

    int32_t next_ts = 0;
    int state = SC_ATOMIC_GET(f.flow_state);
    if (FlowManagerFlowTimeout(&f, state, &ts, &next_ts) != 1 && FlowManagerFlowTimedOut(&f, &ts) != 1) {
        FBLOCK_DESTROY(&fb);
        FLOW_DESTROY(&f);
        FlowQueueDestroy(&flow_spare_q);
        return 0;
    }

    FBLOCK_DESTROY(&fb);
    FLOW_DESTROY(&f);

    FlowQueueDestroy(&flow_spare_q);
    return 1;
}

/**
 *  \test   Test the timing out of a flow with a TcpSession
 *          (with data segments) in normal mode.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int FlowMgrTest02 (void)
{
    TcpSession ssn;
    Flow f;
    FlowBucket fb;
    struct timeval ts;
    TcpSegment seg;
    TcpStream client;
    uint8_t payload[3] = {0x41, 0x41, 0x41};

    FlowQueueInit(&flow_spare_q);

    memset(&ssn, 0, sizeof(TcpSession));
    memset(&f, 0, sizeof(Flow));
    memset(&fb, 0, sizeof(FlowBucket));
    memset(&ts, 0, sizeof(ts));
    memset(&seg, 0, sizeof(TcpSegment));
    memset(&client, 0, sizeof(TcpSegment));

    FBLOCK_INIT(&fb);
    FLOW_INITIALIZE(&f);
    f.flags |= FLOW_TIMEOUT_REASSEMBLY_DONE;

    TimeGet(&ts);
    seg.payload = payload;
    seg.payload_len = 3;
    seg.next = NULL;
    seg.prev = NULL;
    client.seg_list = &seg;
    ssn.client = client;
    ssn.server = client;
    ssn.state = TCP_ESTABLISHED;
    f.lastts.tv_sec = ts.tv_sec - 5000;
    f.protoctx = &ssn;
    f.fb = &fb;
    f.proto = IPPROTO_TCP;

    int32_t next_ts = 0;
    int state = SC_ATOMIC_GET(f.flow_state);
    if (FlowManagerFlowTimeout(&f, state, &ts, &next_ts) != 1 && FlowManagerFlowTimedOut(&f, &ts) != 1) {
        FBLOCK_DESTROY(&fb);
        FLOW_DESTROY(&f);
        FlowQueueDestroy(&flow_spare_q);
        return 0;
    }
    FBLOCK_DESTROY(&fb);
    FLOW_DESTROY(&f);
    FlowQueueDestroy(&flow_spare_q);
    return 1;

}

/**
 *  \test   Test the timing out of a flow with a fresh TcpSession
 *          (just initialized, no data segments) in emergency mode.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int FlowMgrTest03 (void)
{
    TcpSession ssn;
    Flow f;
    FlowBucket fb;
    struct timeval ts;

    FlowQueueInit(&flow_spare_q);

    memset(&ssn, 0, sizeof(TcpSession));
    memset(&f, 0, sizeof(Flow));
    memset(&ts, 0, sizeof(ts));
    memset(&fb, 0, sizeof(FlowBucket));

    FBLOCK_INIT(&fb);
    FLOW_INITIALIZE(&f);
    f.flags |= FLOW_TIMEOUT_REASSEMBLY_DONE;

    TimeGet(&ts);
    ssn.state = TCP_SYN_SENT;
    f.lastts.tv_sec = ts.tv_sec - 300;
    f.protoctx = &ssn;
    f.fb = &fb;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_EMERGENCY;

    int next_ts = 0;
    int state = SC_ATOMIC_GET(f.flow_state);
    if (FlowManagerFlowTimeout(&f, state, &ts, &next_ts) != 1 && FlowManagerFlowTimedOut(&f, &ts) != 1) {
        FBLOCK_DESTROY(&fb);
        FLOW_DESTROY(&f);
        FlowQueueDestroy(&flow_spare_q);
        return 0;
    }

    FBLOCK_DESTROY(&fb);
    FLOW_DESTROY(&f);
    FlowQueueDestroy(&flow_spare_q);
    return 1;
}

/**
 *  \test   Test the timing out of a flow with a TcpSession
 *          (with data segments) in emergency mode.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int FlowMgrTest04 (void)
{

    TcpSession ssn;
    Flow f;
    FlowBucket fb;
    struct timeval ts;
    TcpSegment seg;
    TcpStream client;
    uint8_t payload[3] = {0x41, 0x41, 0x41};

    FlowQueueInit(&flow_spare_q);

    memset(&ssn, 0, sizeof(TcpSession));
    memset(&f, 0, sizeof(Flow));
    memset(&fb, 0, sizeof(FlowBucket));
    memset(&ts, 0, sizeof(ts));
    memset(&seg, 0, sizeof(TcpSegment));
    memset(&client, 0, sizeof(TcpSegment));

    FBLOCK_INIT(&fb);
    FLOW_INITIALIZE(&f);
    f.flags |= FLOW_TIMEOUT_REASSEMBLY_DONE;

    TimeGet(&ts);
    seg.payload = payload;
    seg.payload_len = 3;
    seg.next = NULL;
    seg.prev = NULL;
    client.seg_list = &seg;
    ssn.client = client;
    ssn.server = client;
    ssn.state = TCP_ESTABLISHED;
    f.lastts.tv_sec = ts.tv_sec - 5000;
    f.protoctx = &ssn;
    f.fb = &fb;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_EMERGENCY;

    int next_ts = 0;
    int state = SC_ATOMIC_GET(f.flow_state);
    if (FlowManagerFlowTimeout(&f, state, &ts, &next_ts) != 1 && FlowManagerFlowTimedOut(&f, &ts) != 1) {
        FBLOCK_DESTROY(&fb);
        FLOW_DESTROY(&f);
        FlowQueueDestroy(&flow_spare_q);
        return 0;
    }

    FBLOCK_DESTROY(&fb);
    FLOW_DESTROY(&f);
    FlowQueueDestroy(&flow_spare_q);
    return 1;
}

/**
 *  \test   Test flow allocations when it reach memcap
 *
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int FlowMgrTest05 (void)
{
    int result = 0;

    FlowInitConfig(FLOW_QUIET);
    FlowConfig backup;
    memcpy(&backup, &flow_config, sizeof(FlowConfig));

    uint32_t ini = 0;
    uint32_t end = flow_spare_q.len;
    flow_config.memcap = 10000;
    flow_config.prealloc = 100;

    /* Let's get the flow_spare_q empty */
    UTHBuildPacketOfFlows(ini, end, 0);

    /* And now let's try to reach the memcap val */
    while (FLOW_CHECK_MEMCAP(sizeof(Flow))) {
        ini = end + 1;
        end = end + 2;
        UTHBuildPacketOfFlows(ini, end, 0);
    }

    /* should time out normal */
    TimeSetIncrementTime(2000);
    ini = end + 1;
    end = end + 2;;
    UTHBuildPacketOfFlows(ini, end, 0);

    struct timeval ts;
    TimeGet(&ts);
    /* try to time out flows */
    FlowTimeoutCounters counters = { 0, 0, 0, 0, 0,0,0,0,0,0,0,0,0,0,0};
    FlowTimeoutHash(&ts, 0 /* check all */, 0, flow_config.hash_size, &counters);

    if (flow_recycle_q.len > 0) {
        result = 1;
    }

    memcpy(&flow_config, &backup, sizeof(FlowConfig));
    FlowShutdown();
    return result;
}
#endif /* UNITTESTS */

/**
 *  \brief   Function to register the Flow Unitests.
 */
void FlowMgrRegisterTests (void)
{
#ifdef UNITTESTS
    UtRegisterTest("FlowMgrTest01 -- Timeout a flow having fresh TcpSession",
                   FlowMgrTest01);
    UtRegisterTest("FlowMgrTest02 -- Timeout a flow having TcpSession with segments",
                   FlowMgrTest02);
    UtRegisterTest("FlowMgrTest03 -- Timeout a flow in emergency having fresh TcpSession",
                   FlowMgrTest03);
    UtRegisterTest("FlowMgrTest04 -- Timeout a flow in emergency having TcpSession with segments",
                   FlowMgrTest04);
    UtRegisterTest("FlowMgrTest05 -- Test flow Allocations when it reach memcap",
                   FlowMgrTest05);
#endif /* UNITTESTS */
}
