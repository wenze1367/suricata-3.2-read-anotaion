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
 *  \file
 *
 *  \author Victor Julien <victor@inliniac.net>
 *
 *  Flow implementation.
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
#include "flow-storage.h"

#include "stream-tcp-private.h"
#include "stream-tcp-reassemble.h"
#include "stream-tcp.h"

#include "util-unittest.h"
#include "util-unittest-helper.h"
#include "util-byte.h"
#include "util-misc.h"

#include "util-debug.h"
#include "util-privs.h"

#include "detect.h"
#include "detect-engine-state.h"
#include "stream.h"

#include "app-layer-parser.h"

#define FLOW_DEFAULT_EMERGENCY_RECOVERY 30

//#define FLOW_DEFAULT_HASHSIZE    262144
#define FLOW_DEFAULT_HASHSIZE    65536
//#define FLOW_DEFAULT_MEMCAP      128 * 1024 * 1024 /* 128 MB */
#define FLOW_DEFAULT_MEMCAP      (32 * 1024 * 1024) /* 32 MB */

#define FLOW_DEFAULT_PREALLOC    10000

/** atomic int that is used when freeing a flow from the hash. In this
 *  case we walk the hash to find a flow to free. This var records where
 *  we left off in the hash. Without this only the top rows of the hash
 *  are freed. This isn't just about fairness. Under severe presure, the
 *  hash rows on top would be all freed and the time to find a flow to
 *  free increased with every run. */
SC_ATOMIC_DECLARE(unsigned int, flow_prune_idx);

/** atomic flags */
SC_ATOMIC_DECLARE(unsigned int, flow_flags);

void FlowRegisterTests(void);
void FlowInitFlowProto();
int FlowSetProtoFreeFunc(uint8_t, void (*Free)(void *));

/* Run mode selected at suricata.c */
extern int run_mode;

void FlowCleanupAppLayer(Flow *f)
{
    if (f == NULL || f->proto == 0)
        return;

    AppLayerParserStateCleanup(f->proto, f->alproto, f->alstate, f->alparser);
    f->alstate = NULL;
    f->alparser = NULL;
    return;
}

/** \brief Make sure we have enough spare flows. 
 *
 *  Enforce the prealloc parameter, so keep at least prealloc flows in the
 *  spare queue and free flows going over the limit.
 *
 *  \retval 1 if the queue was properly updated (or if it already was in good shape)
 *  \retval 0 otherwise.
 */
int FlowUpdateSpareFlows(void)
{
    SCEnter();
    uint32_t toalloc = 0, tofree = 0, len;

    FQLOCK_LOCK(&flow_spare_q);
    len = flow_spare_q.len;
    FQLOCK_UNLOCK(&flow_spare_q);

    if (len < flow_config.prealloc) {
        toalloc = flow_config.prealloc - len;

        uint32_t i;
        for (i = 0; i < toalloc; i++) {
            Flow *f = FlowAlloc();
            if (f == NULL)
                return 0;

            FlowEnqueue(&flow_spare_q,f);
        }
    } else if (len > flow_config.prealloc) {
        tofree = len - flow_config.prealloc;

        uint32_t i;
        for (i = 0; i < tofree; i++) {
            /* FlowDequeue locks the queue */
            Flow *f = FlowDequeue(&flow_spare_q);
            if (f == NULL)
                return 1;

            FlowFree(f);
        }
    }

    return 1;
}

/** \brief Set the IPOnly scanned flag for 'direction'.
  *
  * \param f Flow to set the flag in
  * \param direction direction to set the flag in
  */
void FlowSetIPOnlyFlag(Flow *f, int direction)
{
    direction ? (f->flags |= FLOW_TOSERVER_IPONLY_SET) :
        (f->flags |= FLOW_TOCLIENT_IPONLY_SET);
    return;
}

/**
 *  \brief determine the direction of the packet compared to the flow
 *  \retval 0 to_server
 *  \retval 1 to_client
 */
int FlowGetPacketDirection(const Flow *f, const Packet *p)
{
    if (p->proto == IPPROTO_TCP || p->proto == IPPROTO_UDP || p->proto == IPPROTO_SCTP) {
        if (!(CMP_PORT(p->sp,p->dp))) {
            /* update flags and counters */
            if (CMP_PORT(f->sp,p->sp)) {
                return TOSERVER;
            } else {
                return TOCLIENT;
            }
        } else {
            if (CMP_ADDR(&f->src,&p->src)) {
                return TOSERVER;
            } else {
                return TOCLIENT;
            }
        }
    } else if (p->proto == IPPROTO_ICMP || p->proto == IPPROTO_ICMPV6) {
        if (CMP_ADDR(&f->src,&p->src)) {
            return TOSERVER;
        } else {
            return TOCLIENT;
        }
    }

    /* default to toserver */
    return TOSERVER;
}

/**
 *  \brief Check to update "seen" flags
 *
 *  \param p packet
 *
 *  \retval 1 true
 *  \retval 0 false
 */
static inline int FlowUpdateSeenFlag(const Packet *p)
{
    if (PKT_IS_ICMPV4(p)) {
        if (ICMPV4_IS_ERROR_MSG(p)) {
            return 0;
        }
    }

    return 1;
}

/** \brief Update Packet and Flow
 *
 *  Updates packet and flow based on the new packet.
 *
 *  \param f locked flow
 *  \param p packet
 *
 *  \note overwrites p::flowflags;
 流更新
通过FlowWorker线程函数中调用FlowUpdate，FlowUpdate中又调用了FlowHandlePacketUpdate来更新流。 
在获取到包所属的流后，接下来将根据这个包对流进行更新。
************
 流更新
 在获取到包所属的流后，接下来将根据这个包对流进行更新。
 
 使用FlowReference将p->flow指向刚获取流，该函数内部会使用FlowIncrUsecnt增加该流的使用计数。注意，该机制的目的与通常的引用计数不同，不是为了在没有引用时回收资源，而是为了避免出现误删除等问题。
 更新流的lastts_sec，即最近一次更新的时间。超时的判断就是基于这个时间的。
 调用FlowGetPacketDirection获取数据包的方向。注意，这里的方向是相对于流来说的，而流的初始方向由该流的第一个包决定，若当前包与流的第一个包的方向一致（源端口相同或源地址相同），则方向为TOSERVER（可以认为是正向），否则为TOCLIENT（反向）。
 若数据包方向为TOSERVER，则添加流状态标志FLOW_TO_DST_SEEN，并且为数据包的flowflags添加FLOW_PKT_TOSERVER；否则给流添加FLOW_TO_SRC_SEEN，给包添加FLOW_PKT_TOSERVER。
 若流的FLOW_TO_DST_SEEN和FLOW_TO_SRC_SEEN都设置了，说明流已经建立，给包添加FLOW_PKT_ESTABLISHED标志。后面检测时会用到这个标志。
 若流的FLOW_NOPACKET_INSPECTION（绕过包检测）或FLOW_NOPAYLOAD_INSPECTION（绕过负载检测）设置了，则给包也添加相应标志。
 对流的更新完毕，解锁流（FLOWLOCK_UNLOCK）。
 设置包的PKT_HAS_FLOW标志，然后返回。

 */
void FlowHandlePacketUpdate(Flow *f, Packet *p)
{
    SCLogDebug("packet %"PRIu64" -- flow %p", p->pcap_cnt, f);

    int state = SC_ATOMIC_GET(f->flow_state);

    if (state != FLOW_STATE_CAPTURE_BYPASSED) {
        /* update the last seen timestamp of this flow */
        COPY_TIMESTAMP(&p->ts, &f->lastts);
    } else {
        /* still seeing packet, we downgrade to local bypass */
        if (p->ts.tv_sec - f->lastts.tv_sec > FLOW_BYPASSED_TIMEOUT / 2) {
            SCLogDebug("Downgrading flow to local bypass");
            COPY_TIMESTAMP(&p->ts, &f->lastts);
            FlowUpdateState(f, FLOW_STATE_LOCAL_BYPASSED);
        }
    }

    /* update flags and counters;
	调用FlowGetPacketDirection获取数据包的方向。注意，这里的方向是相对于流来说的，
	而流的初始方向由该流的第一个包决定，若当前包与流的第一个包的方向一致（源端口相同或源地址相同），
	则方向为TOSERVER（可以认为是正向），否则为TOCLIENT（反向）。
    */
    if (FlowGetPacketDirection(f, p) == TOSERVER) {
        f->todstpktcnt++;
        f->todstbytecnt += GET_PKT_LEN(p);
        p->flowflags = FLOW_PKT_TOSERVER;
        if (!(f->flags & FLOW_TO_DST_SEEN)) {
            if (FlowUpdateSeenFlag(p)) {
                f->flags |= FLOW_TO_DST_SEEN;
                p->flowflags |= FLOW_PKT_TOSERVER_FIRST;
            }
        }
        /* xfer proto detect ts flag to first packet in ts dir */
        if (f->flags & FLOW_PROTO_DETECT_TS_DONE) {
            f->flags &= ~FLOW_PROTO_DETECT_TS_DONE;
            p->flags |= PKT_PROTO_DETECT_TS_DONE;
        }
		/*若数据包方向为TOSERVER，则增加对应的引用计数，添加流状态标志FLOW_TO_DST_SEEN，
		并且为数据包的flowflags添加FLOW_PKT_TOSERVER。*/
    } else {
        f->tosrcpktcnt++;
        f->tosrcbytecnt += GET_PKT_LEN(p);
        p->flowflags = FLOW_PKT_TOCLIENT;
        if (!(f->flags & FLOW_TO_SRC_SEEN)) {
            if (FlowUpdateSeenFlag(p)) {
                f->flags |= FLOW_TO_SRC_SEEN;
                p->flowflags |= FLOW_PKT_TOCLIENT_FIRST;
            }
        }
        /* xfer proto detect tc flag to first packet in tc dir */
        if (f->flags & FLOW_PROTO_DETECT_TC_DONE) {
            f->flags &= ~FLOW_PROTO_DETECT_TC_DONE;
            p->flags |= PKT_PROTO_DETECT_TC_DONE;
        }
    }
	
	//若数据包方向为TOCLIENT，则增加对应的引用计数，添加流状态标志FLOW_TO_SRC_SEEN，并且为数据包的flowflags添加FLOW_PKT_TOSERVER。
    if ((f->flags & (FLOW_TO_DST_SEEN|FLOW_TO_SRC_SEEN)) == (FLOW_TO_DST_SEEN|FLOW_TO_SRC_SEEN)) {
        SCLogDebug("pkt %p FLOW_PKT_ESTABLISHED", p);
        p->flowflags |= FLOW_PKT_ESTABLISHED;

        if (f->proto != IPPROTO_TCP) {
            FlowUpdateState(f, FLOW_STATE_ESTABLISHED);
        }
    }

    /*set the detection bypass flags*/	
	/*若流的FLOW_TO_DST_SEEN和FLOW_TO_SRC_SEEN都设置了，说明流已经建立，
	给包添加FLOW_PKT_ESTABLISHED标志。后面检测时会用到这个标志。
	*/
    if (f->flags & FLOW_NOPACKET_INSPECTION) {
        SCLogDebug("setting FLOW_NOPACKET_INSPECTION flag on flow %p", f);
        DecodeSetNoPacketInspectionFlag(p);
    }
    if (f->flags & FLOW_NOPAYLOAD_INSPECTION) {
        SCLogDebug("setting FLOW_NOPAYLOAD_INSPECTION flag on flow %p", f);
        DecodeSetNoPayloadInspectionFlag(p);
    }
	//若流的FLOW_NOPACKET_INSPECTION（绕过包检测）或FLOW_NOPAYLOAD_INSPECTION（绕过负载检测）设置了，则给包也添加相应标志。
}

/** \brief Entry point for packet flow handling
 *
 * This is called for every packet.
 *
 *  \param tv threadvars
 *  \param dtv decode thread vars (for flow output api thread data)
 *  \param p packet to handle flow for
 */
void FlowHandlePacket(ThreadVars *tv, DecodeThreadVars *dtv, Packet *p)
{
    /* Get this packet's flow from the hash. FlowHandlePacket() will setup
     * a new flow if nescesary. If we get NULL, we're out of flow memory.
     * The returned flow is locked. */
     //获取包对应的流，找不到就新建流，返回流指针，返回空说明内存不足;
    Flow *f = FlowGetFlowFromHash(tv, dtv, p, &p->flow);
    if (f == NULL)
        return;

    /* set the flow in the packet */
	//设置包的标记为属于某条流;
    p->flags |= PKT_HAS_FLOW;
    return;
}

/** \brief initialize the configuration
 *  \warning Not thread safe 
 Suricata中用于管理和维护流的模块称为Flow Engine，主要由两部分实现，
 第一部分的入口点是FlowHandlePacket函数，用于为新数据包进行流查找/分配，
 另一部分是FlowManagerThread线程，用于对超时的流进行删除。
 
 初始化 
 初始化在FlowInitConfig中完成，与之相关的配置结构体为FlowConfig，其字段含义如下： 
 字段含义hash_rand用于随机化hash table的分布，后面会介绍hash_size流表所使用的hash table的大小
 （桶数量），默认为65536，memcapflow engine所允许使用的最大内存数量，默认为32MB，
 prealloc初始时预分配的流的数量，
 默认为10000，emergency_recovery退出紧急状态前需要删除的流的百分比，默认为30;
********
 初始化的流程如下：
 1.使用FlowQueueInit初始化一个用于存放空闲流（预分配的/回收后的）的flow_spare_q。
 这里专门使用了一个FlowQueue结构体来实现队列，内部是一个标准的链式队列（首指针、尾指针、元素个数），
 与Ring Buffer那种基于数组实现的循环队列相比，更适合存储容量未知的数据。
 2.从配置中读取emergency-recovery、memcap、hash_size、prealloc等值，填充到flow_config的相应字段。
 3.建立流表：按照hash_size为流表flow_hash分配内存，初始化各个bucket的锁，并将这个hash表大小记录
 到当前flow engine占用的内存flow_memuse中。
 4.预分配流：调用FlowAlloc新建prealloc个Flow，然后使用FlowEnqueue将其放入flow_spare_q中。
 5.调用FlowInitFlowProto，初始化各种协议相关的timeout和清理/状态函数。

 */
void FlowInitConfig(char quiet)
{
    SCLogDebug("initializing flow engine...");

	 //初始化FlowConfig类型的变量low_config。
    memset(&flow_config,  0, sizeof(flow_config));
    SC_ATOMIC_INIT(flow_flags);
    SC_ATOMIC_INIT(flow_memuse);
    SC_ATOMIC_INIT(flow_prune_idx);

	/*使用FlowQueueInit初始化一个用于存放空闲流（预分配的/回收后的）的flow_spare_q和flow_recycle_q。
	这里专门使用了一个FlowQueue结构体来实现队列，内部是一个标准的链式队列（首指针、尾指针、元素个数），
	与Ring Buffer那种基于数组实现的循环队列相比，更适合存储容量未知的数据。
	*/
    FlowQueueInit(&flow_spare_q);
    FlowQueueInit(&flow_recycle_q);

#ifndef AFLFUZZ_NO_RANDOM
    unsigned int seed = RandomTimePreseed();
    /* set defaults ；
	设置默认值hash_size：65536;
	*/
    flow_config.hash_rand   = (int)( FLOW_DEFAULT_HASHSIZE * (rand_r(&seed) / RAND_MAX + 1.0));
#endif
    flow_config.hash_size   = FLOW_DEFAULT_HASHSIZE;

	/*设置默认值memcap：32*1024*1024  (32M);*/
    flow_config.memcap      = FLOW_DEFAULT_MEMCAP;

	/*设置默认值prealloc：10000;*/
    flow_config.prealloc    = FLOW_DEFAULT_PREALLOC;

    /* If we have specific config, overwrite the defaults with them,
     * otherwise, leave the default values */
    intmax_t val = 0;

	/*从配置中读取emergency-recovery;*/
    if (ConfGetInt("flow.emergency-recovery", &val) == 1) {
        if (val <= 100 && val >= 1) {
            flow_config.emergency_recovery = (uint8_t)val;
        } else {
            SCLogError(SC_ERR_INVALID_VALUE, "flow.emergency-recovery must be in the range of 1 and 100 (as percentage)");
            flow_config.emergency_recovery = FLOW_DEFAULT_EMERGENCY_RECOVERY;
        }
    } else {
        SCLogDebug("flow.emergency-recovery, using default value");
        flow_config.emergency_recovery = FLOW_DEFAULT_EMERGENCY_RECOVERY;
    }

    /* Check if we have memcap and hash_size defined at config */
    char *conf_val;
    uint32_t configval = 0;

    /** set config values for memcap, prealloc and hash_size;
    从配置中读取memcap;
    */
    if ((ConfGet("flow.memcap", &conf_val)) == 1)
    {
        if (ParseSizeStringU64(conf_val, &flow_config.memcap) < 0) {
            SCLogError(SC_ERR_SIZE_PARSE, "Error parsing flow.memcap "
                       "from conf file - %s.  Killing engine",
                       conf_val);
            exit(EXIT_FAILURE);
        }
    }

	/*从配置中读取hash_size;*/
    if ((ConfGet("flow.hash-size", &conf_val)) == 1)
    {
        if (ByteExtractStringUint32(&configval, 10, strlen(conf_val),
                                    conf_val) > 0) {
            flow_config.hash_size = configval;
        }
    }

	/*从配置中读取prealloc;*/
    if ((ConfGet("flow.prealloc", &conf_val)) == 1)
    {
        if (ByteExtractStringUint32(&configval, 10, strlen(conf_val),
                                    conf_val) > 0) {
            flow_config.prealloc = configval;
        }
    }
    SCLogDebug("Flow config from suricata.yaml: memcap: %"PRIu64", hash-size: "
               "%"PRIu32", prealloc: %"PRIu32, flow_config.memcap,
               flow_config.hash_size, flow_config.prealloc);

    /* alloc hash memory ;
	按照hash_size为流表flow_hash分配内存，并全部初始化为0;
	*/
    uint64_t hash_size = flow_config.hash_size * sizeof(FlowBucket);
    if (!(FLOW_CHECK_MEMCAP(hash_size))) {
        SCLogError(SC_ERR_FLOW_INIT, "allocating flow hash failed: "
                "max flow memcap is smaller than projected hash size. "
                "Memcap: %"PRIu64", Hash table size %"PRIu64". Calculate "
                "total hash size by multiplying \"flow.hash-size\" with %"PRIuMAX", "
                "which is the hash bucket size.", flow_config.memcap, hash_size,
                (uintmax_t)sizeof(FlowBucket));
        exit(EXIT_FAILURE);
    }
	
    flow_hash = SCMallocAligned(flow_config.hash_size * sizeof(FlowBucket), CLS);
    if (unlikely(flow_hash == NULL)) {
        SCLogError(SC_ERR_FATAL, "Fatal error encountered in FlowInitConfig. Exiting...");
        exit(EXIT_FAILURE);
    }
    memset(flow_hash, 0, flow_config.hash_size * sizeof(FlowBucket));

    uint32_t i = 0;
	
	/*初始化各个bucket的锁;*/
    for (i = 0; i < flow_config.hash_size; i++) {
        FBLOCK_INIT(&flow_hash[i]);
        SC_ATOMIC_INIT(flow_hash[i].next_ts);
    }
	
	/*将hash表大小记录到当前flow engine占用的内存flow_memuse中;*/
    (void) SC_ATOMIC_ADD(flow_memuse, (flow_config.hash_size * sizeof(FlowBucket)));

    if (quiet == FALSE) {
        SCLogConfig("allocated %llu bytes of memory for the flow hash... "
                  "%" PRIu32 " buckets of size %" PRIuMAX "",
                  SC_ATOMIC_GET(flow_memuse), flow_config.hash_size,
                  (uintmax_t)sizeof(FlowBucket));
    }

    /* pre allocate flows */
    for (i = 0; i < flow_config.prealloc; i++) {
        if (!(FLOW_CHECK_MEMCAP(sizeof(Flow) + FlowStorageSize()))) {
            SCLogError(SC_ERR_FLOW_INIT, "preallocating flows failed: "
                    "max flow memcap reached. Memcap %"PRIu64", "
                    "Memuse %"PRIu64".", flow_config.memcap,
                    ((uint64_t)SC_ATOMIC_GET(flow_memuse) + (uint64_t)sizeof(Flow)));
            exit(EXIT_FAILURE);
        }

		/*调用FlowAlloc新建prealloc个Flow;*/
        Flow *f = FlowAlloc();
        if (f == NULL) {
            SCLogError(SC_ERR_FLOW_INIT, "preallocating flow failed: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }

		/*使用FlowEnqueue将f放入flow_spare_q中;*/
        FlowEnqueue(&flow_spare_q,f);
    }

    if (quiet == FALSE) {
        SCLogConfig("preallocated %" PRIu32 " flows of size %" PRIuMAX "",
                flow_spare_q.len, (uintmax_t)(sizeof(Flow) + + FlowStorageSize()));
        SCLogConfig("flow memory usage: %llu bytes, maximum: %"PRIu64,
                SC_ATOMIC_GET(flow_memuse), flow_config.memcap);
    }

	/*调用FlowInitFlowProto，初始化各种协议相关的timeout和清理/状态函数，
	此处可在配置文件flow-timeouts里设置*/
    FlowInitFlowProto();

    return;
}

/** \brief print some flow stats
 *  \warning Not thread safe */
static void FlowPrintStats (void)
{
    return;
}

/** \brief shutdown the flow engine
 *  \warning Not thread safe */
void FlowShutdown(void)
{
    Flow *f;
    uint32_t u;

    FlowPrintStats();

    /* free queues */
    while((f = FlowDequeue(&flow_spare_q))) {
        FlowFree(f);
    }
    while((f = FlowDequeue(&flow_recycle_q))) {
        FlowFree(f);
    }

    /* clear and free the hash */
    if (flow_hash != NULL) {
        /* clean up flow mutexes */
        for (u = 0; u < flow_config.hash_size; u++) {
            f = flow_hash[u].head;
            while (f) {
#ifdef DEBUG_VALIDATION
                BUG_ON(SC_ATOMIC_GET(f->use_cnt) != 0);
#endif
                Flow *n = f->hnext;
                uint8_t proto_map = FlowGetProtoMapping(f->proto);
                FlowClearMemory(f, proto_map);
                FlowFree(f);
                f = n;
            }

            FBLOCK_DESTROY(&flow_hash[u]);
            SC_ATOMIC_DESTROY(flow_hash[u].next_ts);
        }
        SCFreeAligned(flow_hash);
        flow_hash = NULL;
    }
    (void) SC_ATOMIC_SUB(flow_memuse, flow_config.hash_size * sizeof(FlowBucket));
    FlowQueueDestroy(&flow_spare_q);
    FlowQueueDestroy(&flow_recycle_q);

    SC_ATOMIC_DESTROY(flow_prune_idx);
    SC_ATOMIC_DESTROY(flow_memuse);
    SC_ATOMIC_DESTROY(flow_flags);
    return;
}

/**
 *  \brief  Function to set the default timeout, free function and flow state
 *          function for all supported flow_proto.
 */

void FlowInitFlowProto(void)
{
    FlowTimeoutsInit();

#define SET_DEFAULTS(p, n, e, c, b, ne, ee, ce, be)     \
    flow_timeouts_normal[(p)].new_timeout = (n);     \
    flow_timeouts_normal[(p)].est_timeout = (e);     \
    flow_timeouts_normal[(p)].closed_timeout = (c);  \
    flow_timeouts_normal[(p)].bypassed_timeout = (b); \
    flow_timeouts_emerg[(p)].new_timeout = (ne);     \
    flow_timeouts_emerg[(p)].est_timeout = (ee);     \
    flow_timeouts_emerg[(p)].closed_timeout = (ce); \
    flow_timeouts_emerg[(p)].bypassed_timeout = (be); \

    SET_DEFAULTS(FLOW_PROTO_DEFAULT,
                FLOW_DEFAULT_NEW_TIMEOUT, FLOW_DEFAULT_EST_TIMEOUT,
                    FLOW_DEFAULT_CLOSED_TIMEOUT, FLOW_DEFAULT_BYPASSED_TIMEOUT,
                FLOW_DEFAULT_EMERG_NEW_TIMEOUT, FLOW_DEFAULT_EMERG_EST_TIMEOUT,
                    FLOW_DEFAULT_EMERG_CLOSED_TIMEOUT, FLOW_DEFAULT_EMERG_BYPASSED_TIMEOUT);
    SET_DEFAULTS(FLOW_PROTO_TCP,
                FLOW_IPPROTO_TCP_NEW_TIMEOUT, FLOW_IPPROTO_TCP_EST_TIMEOUT,
                    FLOW_DEFAULT_CLOSED_TIMEOUT, FLOW_IPPROTO_TCP_BYPASSED_TIMEOUT,
                FLOW_IPPROTO_TCP_EMERG_NEW_TIMEOUT, FLOW_IPPROTO_TCP_EMERG_EST_TIMEOUT,
                    FLOW_DEFAULT_EMERG_CLOSED_TIMEOUT, FLOW_DEFAULT_EMERG_BYPASSED_TIMEOUT);
    SET_DEFAULTS(FLOW_PROTO_UDP,
                FLOW_IPPROTO_UDP_NEW_TIMEOUT, FLOW_IPPROTO_UDP_EST_TIMEOUT,
                    FLOW_DEFAULT_CLOSED_TIMEOUT, FLOW_IPPROTO_UDP_BYPASSED_TIMEOUT,
                FLOW_IPPROTO_UDP_EMERG_NEW_TIMEOUT, FLOW_IPPROTO_UDP_EMERG_EST_TIMEOUT,
                    FLOW_DEFAULT_EMERG_CLOSED_TIMEOUT, FLOW_DEFAULT_EMERG_BYPASSED_TIMEOUT);
    SET_DEFAULTS(FLOW_PROTO_ICMP,
                FLOW_IPPROTO_ICMP_NEW_TIMEOUT, FLOW_IPPROTO_ICMP_EST_TIMEOUT,
                    FLOW_DEFAULT_CLOSED_TIMEOUT, FLOW_IPPROTO_ICMP_BYPASSED_TIMEOUT,
                FLOW_IPPROTO_ICMP_EMERG_NEW_TIMEOUT, FLOW_IPPROTO_ICMP_EMERG_EST_TIMEOUT,
                    FLOW_DEFAULT_EMERG_CLOSED_TIMEOUT, FLOW_DEFAULT_EMERG_BYPASSED_TIMEOUT);

    flow_freefuncs[FLOW_PROTO_DEFAULT].Freefunc = NULL;
    flow_freefuncs[FLOW_PROTO_TCP].Freefunc = NULL;
    flow_freefuncs[FLOW_PROTO_UDP].Freefunc = NULL;
    flow_freefuncs[FLOW_PROTO_ICMP].Freefunc = NULL;

    /* Let's see if we have custom timeouts defined from config */
    const char *new = NULL;
    const char *established = NULL;
    const char *closed = NULL;
    const char *bypassed = NULL;
    const char *emergency_new = NULL;
    const char *emergency_established = NULL;
    const char *emergency_closed = NULL;
    const char *emergency_bypassed = NULL;

    ConfNode *flow_timeouts = ConfGetNode("flow-timeouts");
    if (flow_timeouts != NULL) {
        ConfNode *proto = NULL;
        uint32_t configval = 0;

        /* Defaults. */
        proto = ConfNodeLookupChild(flow_timeouts, "default");
        if (proto != NULL) {
            new = ConfNodeLookupChildValue(proto, "new");
            established = ConfNodeLookupChildValue(proto, "established");
            closed = ConfNodeLookupChildValue(proto, "closed");
            bypassed = ConfNodeLookupChildValue(proto, "bypassed");
            emergency_new = ConfNodeLookupChildValue(proto, "emergency-new");
            emergency_established = ConfNodeLookupChildValue(proto,
                "emergency-established");
            emergency_closed = ConfNodeLookupChildValue(proto,
                "emergency-closed");
            emergency_bypassed = ConfNodeLookupChildValue(proto,
                "emergency-bypassed");

            if (new != NULL &&
                ByteExtractStringUint32(&configval, 10, strlen(new), new) > 0) {

                    flow_timeouts_normal[FLOW_PROTO_DEFAULT].new_timeout = configval;
            }
            if (established != NULL &&
                ByteExtractStringUint32(&configval, 10, strlen(established),
                                        established) > 0) {

                flow_timeouts_normal[FLOW_PROTO_DEFAULT].est_timeout = configval;
            }
            if (closed != NULL &&
                ByteExtractStringUint32(&configval, 10, strlen(closed),
                                        closed) > 0) {

                flow_timeouts_normal[FLOW_PROTO_DEFAULT].closed_timeout = configval;
            }
            if (bypassed != NULL &&
                    ByteExtractStringUint32(&configval, 10,
                                            strlen(bypassed),
                                            bypassed) > 0) {

                flow_timeouts_normal[FLOW_PROTO_DEFAULT].bypassed_timeout = configval;
            }
            if (emergency_new != NULL &&
                ByteExtractStringUint32(&configval, 10, strlen(emergency_new),
                                        emergency_new) > 0) {

                flow_timeouts_emerg[FLOW_PROTO_DEFAULT].new_timeout = configval;
            }
            if (emergency_established != NULL &&
                    ByteExtractStringUint32(&configval, 10,
                                            strlen(emergency_established),
                                            emergency_established) > 0) {

                flow_timeouts_emerg[FLOW_PROTO_DEFAULT].est_timeout= configval;
            }
            if (emergency_closed != NULL &&
                    ByteExtractStringUint32(&configval, 10,
                                            strlen(emergency_closed),
                                            emergency_closed) > 0) {

                flow_timeouts_emerg[FLOW_PROTO_DEFAULT].closed_timeout = configval;
            }
            if (emergency_bypassed != NULL &&
                    ByteExtractStringUint32(&configval, 10,
                                            strlen(emergency_bypassed),
                                            emergency_bypassed) > 0) {

                flow_timeouts_emerg[FLOW_PROTO_DEFAULT].bypassed_timeout = configval;
            }
        }

        /* TCP. */
        proto = ConfNodeLookupChild(flow_timeouts, "tcp");
        if (proto != NULL) {
            new = ConfNodeLookupChildValue(proto, "new");
            established = ConfNodeLookupChildValue(proto, "established");
            closed = ConfNodeLookupChildValue(proto, "closed");
            bypassed = ConfNodeLookupChildValue(proto, "bypassed");
            emergency_new = ConfNodeLookupChildValue(proto, "emergency-new");
            emergency_established = ConfNodeLookupChildValue(proto,
                "emergency-established");
            emergency_closed = ConfNodeLookupChildValue(proto,
                "emergency-closed");
            emergency_bypassed = ConfNodeLookupChildValue(proto,
                "emergency-bypassed");

            if (new != NULL &&
                ByteExtractStringUint32(&configval, 10, strlen(new), new) > 0) {

                flow_timeouts_normal[FLOW_PROTO_TCP].new_timeout = configval;
            }
            if (established != NULL &&
                ByteExtractStringUint32(&configval, 10, strlen(established),
                                        established) > 0) {

                flow_timeouts_normal[FLOW_PROTO_TCP].est_timeout = configval;
            }
            if (closed != NULL &&
                ByteExtractStringUint32(&configval, 10, strlen(closed),
                                        closed) > 0) {

                flow_timeouts_normal[FLOW_PROTO_TCP].closed_timeout = configval;
            }
            if (bypassed != NULL &&
                    ByteExtractStringUint32(&configval, 10,
                                            strlen(bypassed),
                                            bypassed) > 0) {

                flow_timeouts_normal[FLOW_PROTO_TCP].bypassed_timeout = configval;
            }
            if (emergency_new != NULL &&
                ByteExtractStringUint32(&configval, 10, strlen(emergency_new),
                                        emergency_new) > 0) {

                flow_timeouts_emerg[FLOW_PROTO_TCP].new_timeout = configval;
            }
            if (emergency_established != NULL &&
                ByteExtractStringUint32(&configval, 10,
                                        strlen(emergency_established),
                                        emergency_established) > 0) {

                flow_timeouts_emerg[FLOW_PROTO_TCP].est_timeout = configval;
            }
            if (emergency_closed != NULL &&
                ByteExtractStringUint32(&configval, 10,
                                        strlen(emergency_closed),
                                        emergency_closed) > 0) {

                flow_timeouts_emerg[FLOW_PROTO_TCP].closed_timeout = configval;
            }
            if (emergency_bypassed != NULL &&
                    ByteExtractStringUint32(&configval, 10,
                                            strlen(emergency_bypassed),
                                            emergency_bypassed) > 0) {

                flow_timeouts_emerg[FLOW_PROTO_TCP].bypassed_timeout = configval;
            }
        }

        /* UDP. */
        proto = ConfNodeLookupChild(flow_timeouts, "udp");
        if (proto != NULL) {
            new = ConfNodeLookupChildValue(proto, "new");
            established = ConfNodeLookupChildValue(proto, "established");
            bypassed = ConfNodeLookupChildValue(proto, "bypassed");
            emergency_new = ConfNodeLookupChildValue(proto, "emergency-new");
            emergency_established = ConfNodeLookupChildValue(proto,
                "emergency-established");
            emergency_bypassed = ConfNodeLookupChildValue(proto,
                "emergency-bypassed");

            if (new != NULL &&
                ByteExtractStringUint32(&configval, 10, strlen(new), new) > 0) {

                flow_timeouts_normal[FLOW_PROTO_UDP].new_timeout = configval;
            }
            if (established != NULL &&
                ByteExtractStringUint32(&configval, 10, strlen(established),
                                        established) > 0) {

                flow_timeouts_normal[FLOW_PROTO_UDP].est_timeout = configval;
            }
            if (bypassed != NULL &&
                    ByteExtractStringUint32(&configval, 10,
                                            strlen(bypassed),
                                            bypassed) > 0) {

                flow_timeouts_normal[FLOW_PROTO_UDP].bypassed_timeout = configval;
            }
            if (emergency_new != NULL &&
                ByteExtractStringUint32(&configval, 10, strlen(emergency_new),
                                        emergency_new) > 0) {

                flow_timeouts_emerg[FLOW_PROTO_UDP].new_timeout = configval;
            }
            if (emergency_established != NULL &&
                ByteExtractStringUint32(&configval, 10,
                                        strlen(emergency_established),
                                        emergency_established) > 0) {

                flow_timeouts_emerg[FLOW_PROTO_UDP].est_timeout = configval;
            }
            if (emergency_bypassed != NULL &&
                    ByteExtractStringUint32(&configval, 10,
                                            strlen(emergency_bypassed),
                                            emergency_bypassed) > 0) {

                flow_timeouts_emerg[FLOW_PROTO_UDP].bypassed_timeout = configval;
            }
        }

        /* ICMP. */
        proto = ConfNodeLookupChild(flow_timeouts, "icmp");
        if (proto != NULL) {
            new = ConfNodeLookupChildValue(proto, "new");
            established = ConfNodeLookupChildValue(proto, "established");
            bypassed = ConfNodeLookupChildValue(proto, "bypassed");
            emergency_new = ConfNodeLookupChildValue(proto, "emergency-new");
            emergency_established = ConfNodeLookupChildValue(proto,
                "emergency-established");
            emergency_bypassed = ConfNodeLookupChildValue(proto,
                "emergency-bypassed");

            if (new != NULL &&
                ByteExtractStringUint32(&configval, 10, strlen(new), new) > 0) {

                flow_timeouts_normal[FLOW_PROTO_ICMP].new_timeout = configval;
            }
            if (established != NULL &&
                ByteExtractStringUint32(&configval, 10, strlen(established),
                                        established) > 0) {

                flow_timeouts_normal[FLOW_PROTO_ICMP].est_timeout = configval;
            }
            if (bypassed != NULL &&
                    ByteExtractStringUint32(&configval, 10,
                                            strlen(bypassed),
                                            bypassed) > 0) {

                flow_timeouts_normal[FLOW_PROTO_ICMP].bypassed_timeout = configval;
            }
            if (emergency_new != NULL &&
                ByteExtractStringUint32(&configval, 10, strlen(emergency_new),
                                        emergency_new) > 0) {

                flow_timeouts_emerg[FLOW_PROTO_ICMP].new_timeout = configval;
            }
            if (emergency_established != NULL &&
                ByteExtractStringUint32(&configval, 10,
                                        strlen(emergency_established),
                                        emergency_established) > 0) {

                flow_timeouts_emerg[FLOW_PROTO_ICMP].est_timeout = configval;
            }
            if (emergency_bypassed != NULL &&
                    ByteExtractStringUint32(&configval, 10,
                                            strlen(emergency_bypassed),
                                            emergency_bypassed) > 0) {

                flow_timeouts_emerg[FLOW_PROTO_UDP].bypassed_timeout = configval;
            }
        }
    }

    return;
}

/**
 *  \brief  Function clear the flow memory before queueing it to spare flow
 *          queue.
 *
 *  \param  f           pointer to the flow needed to be cleared.
 *  \param  proto_map   mapped value of the protocol to FLOW_PROTO's.
 */

int FlowClearMemory(Flow* f, uint8_t proto_map)
{
    SCEnter();

    /* call the protocol specific free function if we have one */
    if (flow_freefuncs[proto_map].Freefunc != NULL) {
        flow_freefuncs[proto_map].Freefunc(f->protoctx);
    }

    FlowFreeStorage(f);

    FLOW_RECYCLE(f);

    SCReturnInt(1);
}

/**
 *  \brief  Function to set the function to get protocol specific flow state.
 *
 *  \param   proto  protocol of which function is needed to be set.
 *  \param   Free   Function pointer which will be called to free the protocol
 *                  specific memory.
 */

int FlowSetProtoFreeFunc (uint8_t proto, void (*Free)(void *))
{
    uint8_t proto_map;
    proto_map = FlowGetProtoMapping(proto);

    flow_freefuncs[proto_map].Freefunc = Free;
    return 1;
}

AppProto FlowGetAppProtocol(const Flow *f)
{
    return f->alproto;
}

void *FlowGetAppState(const Flow *f)
{
    return f->alstate;
}

/**
 *  \brief get 'disruption' flags: GAP/DEPTH/PASS
 *  \param f locked flow
 *  \param flags existing flags to be ammended
 *  \retval flags original flags + disrupt flags (if any)
 *  \TODO handle UDP
 */
uint8_t FlowGetDisruptionFlags(const Flow *f, uint8_t flags)
{
    if (f->proto != IPPROTO_TCP) {
        return flags;
    }
    if (f->protoctx == NULL) {
        return flags;
    }

    uint8_t newflags = flags;
    TcpSession *ssn = f->protoctx;
    TcpStream *stream = flags & STREAM_TOSERVER ? &ssn->client : &ssn->server;

    if (stream->flags & STREAMTCP_STREAM_FLAG_DEPTH_REACHED) {
        newflags |= STREAM_DEPTH;
    }
    if (stream->flags & STREAMTCP_STREAM_FLAG_GAP) {
        newflags |= STREAM_GAP;
    }
    /* todo: handle pass case (also for UDP!) */

    return newflags;
}

void FlowUpdateState(Flow *f, enum FlowState s)
{
    /* set the state */
    SC_ATOMIC_SET(f->flow_state, s);

    if (f->fb) {
        /* and reset the flow buckup next_ts value so that the flow manager
         * has to revisit this row */
        SC_ATOMIC_SET(f->fb->next_ts, 0);
    }
}

/************************************Unittests*******************************/

#ifdef UNITTESTS
#include "threads.h"

/**
 *  \test   Test the setting of the per protocol timeouts.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int FlowTest01 (void)
{
    uint8_t proto_map;

    FlowInitFlowProto();
    proto_map = FlowGetProtoMapping(IPPROTO_TCP);
    FAIL_IF(flow_timeouts_normal[proto_map].new_timeout != FLOW_IPPROTO_TCP_NEW_TIMEOUT);
    FAIL_IF(flow_timeouts_normal[proto_map].est_timeout != FLOW_IPPROTO_TCP_EST_TIMEOUT);
    FAIL_IF(flow_timeouts_emerg[proto_map].new_timeout != FLOW_IPPROTO_TCP_EMERG_NEW_TIMEOUT);
    FAIL_IF(flow_timeouts_emerg[proto_map].est_timeout != FLOW_IPPROTO_TCP_EMERG_EST_TIMEOUT);

    proto_map = FlowGetProtoMapping(IPPROTO_UDP);
    FAIL_IF(flow_timeouts_normal[proto_map].new_timeout != FLOW_IPPROTO_UDP_NEW_TIMEOUT);
    FAIL_IF(flow_timeouts_normal[proto_map].est_timeout != FLOW_IPPROTO_UDP_EST_TIMEOUT);
    FAIL_IF(flow_timeouts_emerg[proto_map].new_timeout != FLOW_IPPROTO_UDP_EMERG_NEW_TIMEOUT);
    FAIL_IF(flow_timeouts_emerg[proto_map].est_timeout != FLOW_IPPROTO_UDP_EMERG_EST_TIMEOUT);

    proto_map = FlowGetProtoMapping(IPPROTO_ICMP);
    FAIL_IF(flow_timeouts_normal[proto_map].new_timeout != FLOW_IPPROTO_ICMP_NEW_TIMEOUT);
    FAIL_IF(flow_timeouts_normal[proto_map].est_timeout != FLOW_IPPROTO_ICMP_EST_TIMEOUT);
    FAIL_IF(flow_timeouts_emerg[proto_map].new_timeout != FLOW_IPPROTO_ICMP_EMERG_NEW_TIMEOUT);
    FAIL_IF(flow_timeouts_emerg[proto_map].est_timeout != FLOW_IPPROTO_ICMP_EMERG_EST_TIMEOUT);

    proto_map = FlowGetProtoMapping(IPPROTO_DCCP);
    FAIL_IF(flow_timeouts_normal[proto_map].new_timeout != FLOW_DEFAULT_NEW_TIMEOUT);
    FAIL_IF(flow_timeouts_normal[proto_map].est_timeout != FLOW_DEFAULT_EST_TIMEOUT);
    FAIL_IF(flow_timeouts_emerg[proto_map].new_timeout != FLOW_DEFAULT_EMERG_NEW_TIMEOUT);
    FAIL_IF(flow_timeouts_emerg[proto_map].est_timeout != FLOW_DEFAULT_EMERG_EST_TIMEOUT);

    PASS;
}

/*Test function for the unit test FlowTest02*/

static void test(void *f) {}

/**
 *  \test   Test the setting of the per protocol free function to free the
 *          protocol specific memory.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int FlowTest02 (void)
{
    FlowSetProtoFreeFunc(IPPROTO_DCCP, test);
    FlowSetProtoFreeFunc(IPPROTO_TCP, test);
    FlowSetProtoFreeFunc(IPPROTO_UDP, test);
    FlowSetProtoFreeFunc(IPPROTO_ICMP, test);

    FAIL_IF(flow_freefuncs[FLOW_PROTO_DEFAULT].Freefunc != test);
    FAIL_IF(flow_freefuncs[FLOW_PROTO_TCP].Freefunc != test);
    FAIL_IF(flow_freefuncs[FLOW_PROTO_UDP].Freefunc != test);
    FAIL_IF(flow_freefuncs[FLOW_PROTO_ICMP].Freefunc != test);

    PASS;
}

/**
 *  \test   Test flow allocations when it reach memcap
 *
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int FlowTest07 (void)
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

    /* This means that the engine entered emerg mode: should happen as easy
     * with flow mgr activated */
    if (SC_ATOMIC_GET(flow_flags) & FLOW_EMERGENCY)
        result = 1;

    memcpy(&flow_config, &backup, sizeof(FlowConfig));
    FlowShutdown();

    return result;
}

/**
 *  \test   Test flow allocations when it reach memcap
 *
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int FlowTest08 (void)
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

    /* By default we use 30  for timing out new flows. This means
     * that the Emergency mode should be set */
    TimeSetIncrementTime(20);
    ini = end + 1;
    end = end + 2;
    UTHBuildPacketOfFlows(ini, end, 0);

    /* This means that the engine released 5 flows by emergency timeout */
    if (SC_ATOMIC_GET(flow_flags) & FLOW_EMERGENCY)
        result = 1;

    memcpy(&flow_config, &backup, sizeof(FlowConfig));
    FlowShutdown();

    return result;
}

/**
 *  \test   Test flow allocations when it reach memcap
 *
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int FlowTest09 (void)
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

    /* No timeout will work */
    TimeSetIncrementTime(5);
    ini = end + 1;
    end = end + 2;
    UTHBuildPacketOfFlows(ini, end, 0);

    /* engine in emerg mode */
    if (SC_ATOMIC_GET(flow_flags) & FLOW_EMERGENCY)
        result = 1;

    memcpy(&flow_config, &backup, sizeof(FlowConfig));
    FlowShutdown();

    return result;
}

#endif /* UNITTESTS */

/**
 *  \brief   Function to register the Flow Unitests.
 */
void FlowRegisterTests (void)
{
#ifdef UNITTESTS
    UtRegisterTest("FlowTest01 -- Protocol Specific Timeouts", FlowTest01);
    UtRegisterTest("FlowTest02 -- Setting Protocol Specific Free Function",
                   FlowTest02);
    UtRegisterTest("FlowTest07 -- Test flow Allocations when it reach memcap",
                   FlowTest07);
    UtRegisterTest("FlowTest08 -- Test flow Allocations when it reach memcap",
                   FlowTest08);
    UtRegisterTest("FlowTest09 -- Test flow Allocations when it reach memcap",
                   FlowTest09);

    FlowMgrRegisterTests();
    RegisterFlowStorageTests();
#endif /* UNITTESTS */
}
