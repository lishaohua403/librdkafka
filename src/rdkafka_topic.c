/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012,2013 Magnus Edenhill
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met: 
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer. 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution. 
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE 
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "rd.h"
#include "rdkafka_int.h"
#include "rdkafka_msg.h"
#include "rdkafka_topic.h"
#include "rdkafka_partition.h"
#include "rdkafka_broker.h"
#include "rdkafka_cgrp.h"
#include "rdlog.h"
#include "rdsysqueue.h"
#include "rdtime.h"



const char *rd_kafka_topic_state_names[] = {
        "unknown",
        "exists",
        "notexists"
};


/**
 * Final destructor for topic. Refcnt must be 0.
 */
void rd_kafka_topic_destroy_final (rd_kafka_itopic_t *rkt) {

	rd_kafka_assert(rkt->rkt_rk, rd_refcnt_get(&rkt->rkt_refcnt) == 0);

	if (rkt->rkt_topic)
		rd_kafkap_str_destroy(rkt->rkt_topic);

        rd_kafka_assert(rkt->rkt_rk, rd_list_empty(&rkt->rkt_desp));
        rd_list_destroy(&rkt->rkt_desp, NULL);

	rd_kafka_wrlock(rkt->rkt_rk);
	TAILQ_REMOVE(&rkt->rkt_rk->rk_topics, rkt, rkt_link);
	rkt->rkt_rk->rk_topic_cnt--;
	rd_kafka_wrunlock(rkt->rkt_rk);

	rd_kafka_anyconf_destroy(_RK_TOPIC, &rkt->rkt_conf);

	rwlock_destroy(&rkt->rkt_lock);
        rd_refcnt_destroy(&rkt->rkt_refcnt);

	rd_free(rkt);
}

/**
 * Application destroy
 */
void rd_kafka_topic_destroy (rd_kafka_topic_t *app_rkt) {
        rd_kafka_itopic_t *rkt = rd_kafka_topic_a2i(app_rkt);
        shptr_rd_kafka_itopic_t *s_rkt = rd_kafka_topic_a2s(app_rkt);

        rd_kafka_topic_wrlock(rkt);
        if (rkt->rkt_app_rkt == app_rkt)
                rkt->rkt_app_rkt = NULL;
        rd_kafka_topic_wrunlock(rkt);

	rd_kafka_topic_destroy0(s_rkt);
}


/**
 * Finds and returns a topic based on its name, or NULL if not found.
 * The 'rkt' refcount is increased by one and the caller must call
 * rd_kafka_topic_destroy() when it is done with the topic to decrease
 * the refcount.
 *
 * Locality: any thread
 */
shptr_rd_kafka_itopic_t *rd_kafka_topic_find_fl (const char *func, int line,
                                                rd_kafka_t *rk,
                                                const char *topic, int do_lock){
	rd_kafka_itopic_t *rkt;
        shptr_rd_kafka_itopic_t *s_rkt = NULL;

        if (do_lock)
                rd_kafka_rdlock(rk);
	TAILQ_FOREACH(rkt, &rk->rk_topics, rkt_link) {
		if (!rd_kafkap_str_cmp_str(rkt->rkt_topic, topic)) {
                        s_rkt = rd_kafka_topic_keep(rkt);
			break;
		}
	}
        if (do_lock)
                rd_kafka_rdunlock(rk);

	return s_rkt;
}

/**
 * Same semantics as ..find() but takes a Kafka protocol string instead.
 */
shptr_rd_kafka_itopic_t *rd_kafka_topic_find0_fl (const char *func, int line,
                                                 rd_kafka_t *rk,
                                                 const rd_kafkap_str_t *topic) {
	rd_kafka_itopic_t *rkt;
        shptr_rd_kafka_itopic_t *s_rkt = NULL;

	rd_kafka_rdlock(rk);
	TAILQ_FOREACH(rkt, &rk->rk_topics, rkt_link) {
		if (!rd_kafkap_str_cmp(rkt->rkt_topic, topic)) {
                        s_rkt = rd_kafka_topic_keep(rkt);
			break;
		}
	}
	rd_kafka_rdunlock(rk);

	return s_rkt;
}



/**
 * Create new topic handle. 
 *
 * Locality: any
 */
shptr_rd_kafka_itopic_t *rd_kafka_topic_new0 (rd_kafka_t *rk,
                                              const char *topic,
                                              rd_kafka_topic_conf_t *conf,
                                              int *existing,
                                              int do_lock) {
	rd_kafka_itopic_t *rkt;
        shptr_rd_kafka_itopic_t *s_rkt;

	/* Verify configuration.
	 * Maximum topic name size + headers must never exceed message.max.bytes
	 * which is min-capped to 1000.
	 * See rd_kafka_broker_produce_toppar() and rdkafka_conf.c */
	if (!topic || strlen(topic) > 512) {
		if (conf)
			rd_kafka_topic_conf_destroy(conf);
		rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__INVALID_ARG,
					EINVAL);
		return NULL;
	}

	if (do_lock)
                rd_kafka_wrlock(rk);
	if ((s_rkt = rd_kafka_topic_find(rk, topic, 0/*no lock*/))) {
                if (do_lock)
                        rd_kafka_wrunlock(rk);
		if (conf)
			rd_kafka_topic_conf_destroy(conf);
                if (existing)
                        *existing = 1;
		return s_rkt;
        }

        if (existing)
                *existing = 0;

	rkt = rd_calloc(1, sizeof(*rkt));

	rkt->rkt_topic     = rd_kafkap_str_new(topic, -1);
	rkt->rkt_rk        = rk;

	if (!conf) {
                if (rk->rk_conf.topic_conf)
                        conf = rd_kafka_topic_conf_dup(rk->rk_conf.topic_conf);
                else
                        conf = rd_kafka_topic_conf_new();
        }
	rkt->rkt_conf = *conf;
	rd_free(conf); /* explicitly not rd_kafka_topic_destroy()
                        * since we dont want to rd_free internal members,
                        * just the placeholder. The internal members
                        * were copied on the line above. */

	/* Default partitioner: consistent_random */
	if (!rkt->rkt_conf.partitioner)
		rkt->rkt_conf.partitioner = rd_kafka_msg_partitioner_consistent_random;

	if (rkt->rkt_conf.compression_codec == RD_KAFKA_COMPRESSION_INHERIT)
		rkt->rkt_conf.compression_codec = rk->rk_conf.compression_codec;

	rd_kafka_dbg(rk, TOPIC, "TOPIC", "New local topic: %.*s",
		     RD_KAFKAP_STR_PR(rkt->rkt_topic));

        rd_list_init(&rkt->rkt_desp, 16);
        rd_refcnt_init(&rkt->rkt_refcnt, 0);

        s_rkt = rd_kafka_topic_keep(rkt);

	rwlock_init(&rkt->rkt_lock);

	/* Create unassigned partition */
	rkt->rkt_ua = rd_kafka_toppar_new(rkt, RD_KAFKA_PARTITION_UA);

	TAILQ_INSERT_TAIL(&rk->rk_topics, rkt, rkt_link);
	rk->rk_topic_cnt++;

        if (do_lock)
                rd_kafka_wrunlock(rk);

	return s_rkt;
}

/**
 * Create new app topic handle.
 *
 * Locality: application thread
 */
rd_kafka_topic_t *rd_kafka_topic_new (rd_kafka_t *rk, const char *topic,
                                      rd_kafka_topic_conf_t *conf) {
        shptr_rd_kafka_itopic_t *s_rkt;
        rd_kafka_itopic_t *rkt;
        int existing;

        s_rkt = rd_kafka_topic_new0(rk, topic, conf, &existing, 1/*lock*/);
        if (!s_rkt)
                return NULL;

        rkt = rd_kafka_topic_s2i(s_rkt);

        /* Save a shared pointer to be used in callbacks. */
        rd_kafka_topic_wrlock(rkt);
        if (!rkt->rkt_app_rkt)
                rkt->rkt_app_rkt = rd_kafka_topic_s2a(s_rkt);
        rd_kafka_topic_wrunlock(rkt);

        /* Query for the topic leader (async) */
        if (!existing)
                rd_kafka_topic_leader_query(rk, rkt);

        return rkt->rkt_app_rkt;
}



/**
 * Sets the state for topic.
 * NOTE: rd_kafka_topic_wrlock(rkt) MUST be held
 */
static void rd_kafka_topic_set_state (rd_kafka_itopic_t *rkt, int state) {

        if ((int)rkt->rkt_state == state)
                return;

        rd_kafka_dbg(rkt->rkt_rk, TOPIC, "STATE",
                     "Topic %s changed state %s -> %s",
                     rkt->rkt_topic->str,
                     rd_kafka_topic_state_names[rkt->rkt_state],
                     rd_kafka_topic_state_names[state]);
        rkt->rkt_state = state;
}

/**
 * Returns the name of a topic.
 * NOTE:
 *   The topic Kafka String representation is crafted with an extra byte
 *   at the end for the Nul that is not included in the length, this way
 *   we can use the topic's String directly.
 *   This is not true for Kafka Strings read from the network.
 */
const char *rd_kafka_topic_name (const rd_kafka_topic_t *app_rkt) {
        const rd_kafka_itopic_t *rkt = rd_kafka_topic_a2i(app_rkt);
	return rkt->rkt_topic->str;
}





/**
 * Update the leader for a topic+partition.
 * Returns 1 if the leader was changed, else 0, or -1 if leader is unknown.
 * NOTE: rd_kafka_topic_wrlock(rkt) MUST be held.
 */
static int rd_kafka_topic_leader_update (rd_kafka_itopic_t *rkt,
                                         int32_t partition, int32_t leader_id,
					 rd_kafka_broker_t *rkb) {
	rd_kafka_t *rk = rkt->rkt_rk;
	rd_kafka_toppar_t *rktp;
        shptr_rd_kafka_toppar_t *s_rktp;

	s_rktp = rd_kafka_toppar_get(rkt, partition, 0);
        if (unlikely(!s_rktp)) {
                /* Have only seen this in issue #132.
                 * Probably caused by corrupt broker state. */
                rd_kafka_log(rk, LOG_WARNING, "LEADER",
                             "Topic %s: partition [%"PRId32"] is unknown "
                             "(partition_cnt %i)",
                             rkt->rkt_topic->str, partition,
                             rkt->rkt_partition_cnt);
                return -1;
        }

        rktp = rd_kafka_toppar_s2i(s_rktp);

        rd_kafka_toppar_lock(rktp);

	if (!rkb) {
		int had_leader = rktp->rktp_leader ? 1 : 0;

		rd_kafka_toppar_broker_delegate(rktp, NULL);

                rd_kafka_toppar_unlock(rktp);
		rd_kafka_toppar_destroy(s_rktp); /* from get() */

		return had_leader ? -1 : 0;
	}


	if (rktp->rktp_leader) {
		if (rktp->rktp_leader == rkb) {
			/* No change in broker */
                        rd_kafka_toppar_unlock(rktp);
			rd_kafka_toppar_destroy(s_rktp); /* from get() */
			return 0;
		}

		rd_kafka_dbg(rk, TOPIC, "TOPICUPD",
			     "Topic %s [%"PRId32"] migrated from "
			     "broker %"PRId32" to %"PRId32,
			     rkt->rkt_topic->str, partition,
			     rktp->rktp_leader->rkb_nodeid, rkb->rkb_nodeid);
	}

	rd_kafka_toppar_broker_delegate(rktp, rkb);

        rd_kafka_toppar_unlock(rktp);
	rd_kafka_toppar_destroy(s_rktp); /* from get() */

	return 1;
}


/**
 * Update the number of partitions for a topic and takes according actions.
 * Returns 1 if the partition count changed, else 0.
 * NOTE: rd_kafka_topic_wrlock(rkt) MUST be held.
 */
static int rd_kafka_topic_partition_cnt_update (rd_kafka_itopic_t *rkt,
						int32_t partition_cnt) {
	rd_kafka_t *rk = rkt->rkt_rk;
	shptr_rd_kafka_toppar_t **rktps;
	shptr_rd_kafka_toppar_t *rktp_ua;
        shptr_rd_kafka_toppar_t *s_rktp;
	rd_kafka_toppar_t *rktp;
	int32_t i;

	if (likely(rkt->rkt_partition_cnt == partition_cnt))
		return 0; /* No change in partition count */

        if (unlikely(rkt->rkt_partition_cnt != 0 &&
                     !rd_kafka_terminating(rkt->rkt_rk)))
                rd_kafka_log(rk, LOG_NOTICE, "PARTCNT",
                             "Topic %s partition count changed "
                             "from %"PRId32" to %"PRId32,
                             rkt->rkt_topic->str,
                             rkt->rkt_partition_cnt, partition_cnt);
        else
                rd_kafka_dbg(rk, TOPIC, "PARTCNT",
                             "Topic %s partition count changed "
                             "from %"PRId32" to %"PRId32,
                             rkt->rkt_topic->str,
                             rkt->rkt_partition_cnt, partition_cnt);


	/* Create and assign new partition list */
	if (partition_cnt > 0)
		rktps = rd_calloc(partition_cnt, sizeof(*rktps));
	else
		rktps = NULL;

	for (i = 0 ; i < partition_cnt ; i++) {
		if (i >= rkt->rkt_partition_cnt) {
			/* New partition. Check if its in the list of
			 * desired partitions first. */

                        s_rktp = rd_kafka_toppar_desired_get(rkt, i);

                        rktp = s_rktp ? rd_kafka_toppar_s2i(s_rktp) : NULL;
                        if (rktp) {
				rd_kafka_toppar_lock(rktp);
                                if (rktp->rktp_flags &
                                    RD_KAFKA_TOPPAR_F_UNKNOWN) {
                                        /* Remove from desp list since the 
                                         * partition is now known. */
                                        rktp->rktp_flags &=
                                                ~RD_KAFKA_TOPPAR_F_UNKNOWN;
                                        rd_kafka_toppar_desired_unlink(rktp);
                                }
                                rd_kafka_toppar_unlock(rktp);
			} else
				s_rktp = rd_kafka_toppar_new(rkt, i);
			rktps[i] = s_rktp;
		} else {
			/* Move existing partition */
			rktps[i] = rkt->rkt_p[i];
		}
	}

	rktp_ua = rd_kafka_toppar_get(rkt, RD_KAFKA_PARTITION_UA, 0);

        /* Propagate notexist errors for desired partitions */
        RD_LIST_FOREACH(s_rktp, &rkt->rkt_desp, i)
                rd_kafka_toppar_enq_error(rd_kafka_toppar_s2i(s_rktp),
                                          RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION);

	/* Remove excessive partitions if partition count decreased. */
	for (; i < rkt->rkt_partition_cnt ; i++) {
		s_rktp = rkt->rkt_p[i];
                rktp = rd_kafka_toppar_s2i(s_rktp);

		rd_kafka_toppar_lock(rktp);

                rd_kafka_toppar_broker_delegate(rktp, NULL);

		/* Partition has gone away, move messages to UA or error-out */
		if (likely(rktp_ua != NULL))
			rd_kafka_toppar_move_msgs(rd_kafka_toppar_s2i(rktp_ua),
                                                  rktp);
		else
                        rd_kafka_dr_msgq(rkt, &rktp->rktp_msgq,
                                         RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION);


                rd_kafka_toppar_purge_queues(rktp);

		if (rktp->rktp_flags & RD_KAFKA_TOPPAR_F_DESIRED) {
                        rd_kafka_dbg(rkt->rkt_rk, TOPIC, "DESIRED",
                                     "Topic %s [%"PRId32"] is desired "
                                     "but no longer known: "
                                     "moving back on desired list",
                                     rkt->rkt_topic->str, rktp->rktp_partition);

                        /* If this is a desired partition move it back on to
                         * the desired list since partition is no longer known*/
			rd_kafka_assert(rkt->rkt_rk,
                                        !(rktp->rktp_flags &
                                          RD_KAFKA_TOPPAR_F_UNKNOWN));
			rktp->rktp_flags |= RD_KAFKA_TOPPAR_F_UNKNOWN;
                        rd_kafka_toppar_desired_link(rktp);

                        if (!rd_kafka_terminating(rkt->rkt_rk))
                                rd_kafka_toppar_enq_error(
                                        rktp,
                                        RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION);
		}
		rd_kafka_toppar_unlock(rktp);

		rd_kafka_toppar_destroy(s_rktp);
	}

	if (likely(rktp_ua != NULL))
		rd_kafka_toppar_destroy(rktp_ua); /* .._get() above */

	if (rkt->rkt_p)
		rd_free(rkt->rkt_p);

	rkt->rkt_p = rktps;

	rkt->rkt_partition_cnt = partition_cnt;

	return 1;
}



/**
 * Topic 'rkt' does not exist: propagate to interested parties.
 * The topic's state must have been set to NOTEXISTS and
 * rd_kafka_topic_partition_cnt_update() must have been called prior to
 * calling this function.
 *
 * Locks: rd_kafka_topic_*lock() must be held.
 */
static void rd_kafka_topic_propagate_notexists (rd_kafka_itopic_t *rkt) {
        shptr_rd_kafka_toppar_t *s_rktp;
        int i;

        if (rkt->rkt_rk->rk_type != RD_KAFKA_CONSUMER)
                return;


        /* Notify consumers that the topic doesn't exist. */
        RD_LIST_FOREACH(s_rktp, &rkt->rkt_desp, i)
                rd_kafka_toppar_enq_error(rd_kafka_toppar_s2i(s_rktp),
                                          RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC);
}


/**
 * Assign messages on the UA partition to available partitions.
 * Locks: rd_kafka_topic_*lock() must be held.
 */
static void rd_kafka_topic_assign_uas (rd_kafka_itopic_t *rkt) {
	rd_kafka_t *rk = rkt->rkt_rk;
	shptr_rd_kafka_toppar_t *s_rktp_ua;
        rd_kafka_toppar_t *rktp_ua;
	rd_kafka_msg_t *rkm, *tmp;
	rd_kafka_msgq_t uas = RD_KAFKA_MSGQ_INITIALIZER(uas);
	rd_kafka_msgq_t failed = RD_KAFKA_MSGQ_INITIALIZER(failed);
	int cnt;

	if (rkt->rkt_rk->rk_type != RD_KAFKA_PRODUCER)
		return;

	s_rktp_ua = rd_kafka_toppar_get(rkt, RD_KAFKA_PARTITION_UA, 0);
	if (unlikely(!s_rktp_ua)) {
		rd_kafka_dbg(rk, TOPIC, "ASSIGNUA",
			     "No UnAssigned partition available for %s",
			     rkt->rkt_topic->str);
		return;
	}

        rktp_ua = rd_kafka_toppar_s2i(s_rktp_ua);

	/* Assign all unassigned messages to new topics. */
	rd_kafka_dbg(rk, TOPIC, "PARTCNT",
		     "Partitioning %i unassigned messages in topic %.*s to "
		     "%"PRId32" partitions",
		     rd_atomic32_get(&rktp_ua->rktp_msgq.rkmq_msg_cnt),
		     RD_KAFKAP_STR_PR(rkt->rkt_topic),
		     rkt->rkt_partition_cnt);

	rd_kafka_toppar_lock(rktp_ua);
	rd_kafka_msgq_move(&uas, &rktp_ua->rktp_msgq);
	cnt = rd_atomic32_get(&uas.rkmq_msg_cnt);
	rd_kafka_toppar_unlock(rktp_ua);

	TAILQ_FOREACH_SAFE(rkm, &uas.rkmq_msgs, rkm_link, tmp) {
		/* Fast-path for failing messages with forced partition */
		if (rkm->rkm_partition != RD_KAFKA_PARTITION_UA &&
		    rkm->rkm_partition >= rkt->rkt_partition_cnt &&
		    rkt->rkt_state != RD_KAFKA_TOPIC_S_UNKNOWN) {
			rd_kafka_msgq_enq(&failed, rkm);
			continue;
		}

		if (unlikely(rd_kafka_msg_partitioner(rkt, rkm, 0) != 0)) {
			/* Desired partition not available */
			rd_kafka_msgq_enq(&failed, rkm);
		}
	}

	rd_kafka_dbg(rk, TOPIC, "UAS",
		     "%i/%i messages were partitioned in topic %s",
		     cnt - rd_atomic32_get(&failed.rkmq_msg_cnt),
		     cnt, rkt->rkt_topic->str);

	if (rd_atomic32_get(&failed.rkmq_msg_cnt) > 0) {
		/* Fail the messages */
		rd_kafka_dbg(rk, TOPIC, "UAS",
			     "%"PRId32"/%i messages failed partitioning "
			     "in topic %s",
			     rd_atomic32_get(&uas.rkmq_msg_cnt), cnt,
			     rkt->rkt_topic->str);
		rd_kafka_dr_msgq(rkt, &failed,
				 rkt->rkt_state == RD_KAFKA_TOPIC_S_NOTEXISTS ?
				 RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC :
				 RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION);
	}

	rd_kafka_toppar_destroy(s_rktp_ua); /* from get() */
}


/**
 * Received metadata request contained no information about topic 'rkt'
 * and thus indicates the topic is not available in the cluster.
 */
void rd_kafka_topic_metadata_none (rd_kafka_itopic_t *rkt) {
	rd_kafka_topic_wrlock(rkt);

	if (unlikely(rd_atomic32_get(&rkt->rkt_rk->rk_terminate))) {
		/* Dont update metadata while terminating, do this
		 * after acquiring lock for proper synchronisation */
		rd_kafka_topic_wrunlock(rkt);
		return;
	}

	rkt->rkt_ts_metadata = rd_clock();

        rd_kafka_topic_set_state(rkt, RD_KAFKA_TOPIC_S_NOTEXISTS);

	/* Update number of partitions */
	rd_kafka_topic_partition_cnt_update(rkt, 0);

	/* Purge messages with forced partition */
	rd_kafka_topic_assign_uas(rkt);

        /* Propagate nonexistent topic info */
        rd_kafka_topic_propagate_notexists(rkt);

	rd_kafka_topic_wrunlock(rkt);
}


/**
 * Update a topic from metadata.
 * Returns 1 if the number of partitions changed, 0 if not, and -1 if the
 * topic is unknown.
 */
int rd_kafka_topic_metadata_update (rd_kafka_broker_t *rkb,
                                    const struct rd_kafka_metadata_topic *mdt) {
	rd_kafka_itopic_t *rkt;
        shptr_rd_kafka_itopic_t *s_rkt;
	int upd = 0;
	int j;
        rd_kafka_broker_t **partbrokers;
        int query_leader = 0;
        int old_state;

	/* Ignore topics in blacklist */
        if (rkb->rkb_rk->rk_conf.topic_blacklist &&
	    rd_kafka_pattern_match(rkb->rkb_rk->rk_conf.topic_blacklist,
                                   mdt->topic)) {
                rd_rkb_dbg(rkb, TOPIC, "BLACKLIST",
                           "Ignoring blacklisted topic \"%s\" in metadata",
                           mdt->topic);
                return -1;
        }

	/* Ignore metadata completely for temporary errors. (issue #513)
	 *   LEADER_NOT_AVAILABLE: Broker is rebalancing
	 */
	if (mdt->err == RD_KAFKA_RESP_ERR_LEADER_NOT_AVAILABLE &&
	    mdt->partition_cnt == 0) {
		rd_rkb_dbg(rkb, TOPIC, "METADATA",
			   "Temporary error in metadata reply for "
			   "topic %s (PartCnt %i): %s: ignoring",
			   mdt->topic, mdt->partition_cnt,
			   rd_kafka_err2str(mdt->err));
		return -1;
	}


	if (!(s_rkt = rd_kafka_topic_find(rkb->rkb_rk, mdt->topic, 1/*lock*/)))
		return -1; /* Ignore topics that we dont have locally. */

        rkt = rd_kafka_topic_s2i(s_rkt);

	if (mdt->err != RD_KAFKA_RESP_ERR_NO_ERROR)
		rd_rkb_dbg(rkb, TOPIC, "METADATA",
			   "Error in metadata reply for "
			   "topic %s (PartCnt %i): %s",
			   rkt->rkt_topic->str, mdt->partition_cnt,
			   rd_kafka_err2str(mdt->err));

	/* Look up brokers before acquiring rkt lock to preserve lock order */
	rd_kafka_rdlock(rkb->rkb_rk);

	if (unlikely(rd_atomic32_get(&rkb->rkb_rk->rk_terminate))) {
		/* Dont update metadata while terminating, do this
		 * after acquiring lock for proper synchronisation */
		rd_kafka_rdunlock(rkb->rkb_rk);
		rd_kafka_topic_destroy0(s_rkt); /* from find() */
		return -1;
	}

        partbrokers = rd_alloca(mdt->partition_cnt * sizeof(*partbrokers));

	for (j = 0 ; j < mdt->partition_cnt ; j++) {
		if (mdt->partitions[j].leader == -1) {
                        partbrokers[j] = NULL;
			continue;
		}

                partbrokers[j] =
                        rd_kafka_broker_find_by_nodeid(rkb->rkb_rk,
                                                       mdt->partitions[j].
                                                       leader);
	}
	rd_kafka_rdunlock(rkb->rkb_rk);


	rd_kafka_topic_wrlock(rkt);

        old_state = rkt->rkt_state;
	rkt->rkt_ts_metadata = rd_clock();

	/* Set topic state */
	if (mdt->err == RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART ||
	    mdt->err == RD_KAFKA_RESP_ERR_UNKNOWN/*auto.create.topics fails*/)
                rd_kafka_topic_set_state(rkt, RD_KAFKA_TOPIC_S_NOTEXISTS);
        else if (mdt->partition_cnt > 0)
                rd_kafka_topic_set_state(rkt, RD_KAFKA_TOPIC_S_EXISTS);

	/* Update number of partitions, but not if there are
	 * (possibly intermittent) errors (e.g., "Leader not available"). */
	if (mdt->err == RD_KAFKA_RESP_ERR_NO_ERROR)
		upd += rd_kafka_topic_partition_cnt_update(rkt,
							   mdt->partition_cnt);

	/* Update leader for each partition */
	for (j = 0 ; j < mdt->partition_cnt ; j++) {
                int r;
		rd_kafka_broker_t *leader;

		rd_rkb_dbg(rkb, METADATA, "METADATA",
			   "  Topic %s partition %i Leader %"PRId32,
			   rkt->rkt_topic->str,
			   mdt->partitions[j].id,
			   mdt->partitions[j].leader);

		leader = partbrokers[j];
		partbrokers[j] = NULL;

		/* Update leader for partition */
		r = rd_kafka_topic_leader_update(rkt,
                                                 mdt->partitions[j].id,
                                                 mdt->partitions[j].leader,
						 leader);

                if (r == -1)
                        query_leader = 1;

                upd += (r != 0 ? 1 : 0);

                /* Drop reference to broker (from find()) */
		if (leader)
			rd_kafka_broker_destroy(leader);

	}

	if (mdt->err != RD_KAFKA_RESP_ERR_NO_ERROR && rkt->rkt_partition_cnt) {
		/* (Possibly intermediate) topic-wide error:
		 * remove leaders for partitions */

		for (j = 0 ; j < rkt->rkt_partition_cnt ; j++) {
                        rd_kafka_toppar_t *rktp;
			if (!rkt->rkt_p[j])
                                continue;

                        rktp = rd_kafka_toppar_s2i(rkt->rkt_p[j]);
                        rd_kafka_toppar_lock(rktp);
                        rd_kafka_toppar_broker_delegate(rktp, NULL);
                        rd_kafka_toppar_unlock(rktp);
                }
        }

	/* Try to assign unassigned messages to new partitions, or fail them */
	if (upd > 0 || rkt->rkt_state == RD_KAFKA_TOPIC_S_NOTEXISTS)
		rd_kafka_topic_assign_uas(rkt);

        /* Trigger notexists propagation */
        if (old_state != (int)rkt->rkt_state &&
            rkt->rkt_state == RD_KAFKA_TOPIC_S_NOTEXISTS)
                rd_kafka_topic_propagate_notexists(rkt);

	rd_kafka_topic_wrunlock(rkt);

        /* Query for the topic leader (async) */
        if (query_leader)
                rd_kafka_topic_leader_query(rkt->rkt_rk, rkt);

	rd_kafka_topic_destroy0(s_rkt); /* from find() */

	/* Loose broker references */
	for (j = 0 ; j < mdt->partition_cnt ; j++)
		if (partbrokers[j])
			rd_kafka_broker_destroy(partbrokers[j]);


	return upd;
}



/**
 * Remove all partitions from a topic, including the ua.
 * WARNING: Any messages in partition queues will be LOST.
 */
void rd_kafka_topic_partitions_remove (rd_kafka_itopic_t *rkt) {
        shptr_rd_kafka_toppar_t *s_rktp;
        shptr_rd_kafka_itopic_t *s_rkt;
	int i;
	rd_kafka_msgq_t tmpq = RD_KAFKA_MSGQ_INITIALIZER(tmpq);

	/* Move all partition's queued messages to our temporary queue
	 * and purge that queue later outside the topic_wrlock since
	 * a message can hold a reference to the topic_t and thus
	 * would trigger a recursive lock dead-lock. */

	s_rkt = rd_kafka_topic_keep(rkt);
	rd_kafka_topic_wrlock(rkt);

	/* Setting the partition count to 0 moves all partitions to
	 * the desired list (rktp_desp). */
        rd_kafka_topic_partition_cnt_update(rkt, 0);

        /* Now clean out the desired partitions list.
         * Use reverse traversal to avoid excessive memory shuffling
         * in rd_list_remove() */
        RD_LIST_FOREACH_REVERSE(s_rktp, &rkt->rkt_desp, i) {
		rd_kafka_toppar_t *rktp = rd_kafka_toppar_s2i(s_rktp);
		/* Our reference */
		shptr_rd_kafka_toppar_t *s_rktp2 = rd_kafka_toppar_keep(rktp);
                rd_kafka_toppar_lock(rktp);
		rd_kafka_toppar_move_queues(rktp, &tmpq);
                rd_kafka_toppar_desired_del(rktp);
                rd_kafka_toppar_unlock(rktp);
                rd_kafka_toppar_destroy(s_rktp2);
        }

        rd_kafka_assert(rkt->rkt_rk, rkt->rkt_partition_cnt == 0);

	if (rkt->rkt_p)
		rd_free(rkt->rkt_p);

	rkt->rkt_p = NULL;
	rkt->rkt_partition_cnt = 0;

        if ((s_rktp = rkt->rkt_ua)) {
		rd_kafka_toppar_t *rktp = rd_kafka_toppar_s2i(s_rktp);
		rd_kafka_toppar_move_queues(rktp, &tmpq);
                rkt->rkt_ua = NULL;
                rd_kafka_toppar_destroy(s_rktp);
	}

	rd_kafka_topic_wrunlock(rkt);

	/* Now purge the messages outside the topic lock. */
	rd_kafka_dbg(rkt->rkt_rk, TOPIC, "TOPIC", "%.*s: purging %d messages",
		     RD_KAFKAP_STR_PR(rkt->rkt_topic),
		     rd_kafka_msgq_len(&tmpq));

	rd_kafka_msgq_purge(rkt->rkt_rk, &tmpq);

	rd_kafka_topic_destroy0(s_rkt);
}



/**
 * Scan all topics and partitions for:
 *  - timed out messages.
 *  - topics that needs to be created on the broker.
 *  - topics who's metadata is too old.
 */
int rd_kafka_topic_scan_all (rd_kafka_t *rk, rd_ts_t now) {
	rd_kafka_itopic_t *rkt;
	rd_kafka_toppar_t *rktp;
        shptr_rd_kafka_toppar_t *s_rktp;
	int totcnt = 0;
	int wrlocked = 0;


	rd_kafka_rdlock(rk);
	TAILQ_FOREACH(rkt, &rk->rk_topics, rkt_link) {
		int p;
                int cnt = 0, tpcnt = 0;
                rd_kafka_msgq_t timedout;

                rd_kafka_msgq_init(&timedout);

		rd_kafka_topic_wrlock(rkt);
		wrlocked = 1;

                /* Check if metadata information has timed out:
                 * older than 3 times the metadata.refresh.interval.ms */
                if (rkt->rkt_state != RD_KAFKA_TOPIC_S_UNKNOWN &&
		    rkt->rkt_rk->rk_conf.metadata_refresh_interval_ms >= 0 &&
                    rd_clock() > rkt->rkt_ts_metadata +
                    (rkt->rkt_rk->rk_conf.metadata_refresh_interval_ms *
                     1000 * 3)) {
                        rd_kafka_dbg(rk, TOPIC, "NOINFO",
                                     "Topic %s metadata information timed out "
                                     "(%"PRId64"ms old)",
                                     rkt->rkt_topic->str,
                                     (rd_clock() - rkt->rkt_ts_metadata)/1000);
                        rd_kafka_topic_set_state(rkt, RD_KAFKA_TOPIC_S_UNKNOWN);
                }

                /* Just need a read-lock from here on. */
                rd_kafka_topic_wrunlock(rkt);
                rd_kafka_topic_rdlock(rkt);
				wrlocked = 0;

		if (rkt->rkt_partition_cnt == 0) {
			/* If this partition is unknown by brokers try
			 * to create it by sending a topic-specific
			 * metadata request.
			 * This requires "auto.create.topics.enable=true"
			 * on the brokers. */

			/* Need to unlock topic lock first.. */
			rd_kafka_topic_rdunlock(rkt);
			rd_kafka_topic_leader_query0(rk, rkt, 0/*no_rk_lock*/);
			rd_kafka_topic_rdlock(rkt);
		}

		for (p = RD_KAFKA_PARTITION_UA ;
		     p < rkt->rkt_partition_cnt ; p++) {
			int did_tmout = 0;

			if (!(s_rktp = rd_kafka_toppar_get(rkt, p, 0)))
				continue;

                        rktp = rd_kafka_toppar_s2i(s_rktp);
			rd_kafka_toppar_lock(rktp);

			/* Scan toppar's message queues for timeouts */
			if (rd_kafka_msgq_age_scan(&rktp->rktp_xmit_msgq,
						   &timedout, now) > 0)
				did_tmout = 1;

			if (rd_kafka_msgq_age_scan(&rktp->rktp_msgq,
						   &timedout, now) > 0)
				did_tmout = 1;

			tpcnt += did_tmout;

			rd_kafka_toppar_unlock(rktp);
			rd_kafka_toppar_destroy(s_rktp);
		}

		if (wrlocked)
			rd_kafka_topic_wrunlock(rkt);
		else
			rd_kafka_topic_rdunlock(rkt);

                if ((cnt = rd_atomic32_get(&timedout.rkmq_msg_cnt)) > 0) {
                        totcnt += cnt;
                        rd_kafka_dbg(rk, MSG, "TIMEOUT",
                                     "%s: %"PRId32" message(s) "
                                     "from %i toppar(s) timed out",
                                     rkt->rkt_topic->str, cnt, tpcnt);
                        rd_kafka_dr_msgq(rkt, &timedout,
                                         RD_KAFKA_RESP_ERR__MSG_TIMED_OUT);
                }
	}
	rd_kafka_rdunlock(rk);

	return totcnt;
}


/**
 * Locks: rd_kafka_topic_*lock() must be held.
 */
int rd_kafka_topic_partition_available (const rd_kafka_topic_t *app_rkt,
					int32_t partition) {
	int avail;
	shptr_rd_kafka_toppar_t *s_rktp;
        rd_kafka_toppar_t *rktp;
        rd_kafka_broker_t *rkb;

	s_rktp = rd_kafka_toppar_get(rd_kafka_topic_a2i(app_rkt),
                                     partition, 0/*no ua-on-miss*/);
	if (unlikely(!s_rktp))
		return 0;

        rktp = rd_kafka_toppar_s2i(s_rktp);
        rkb = rd_kafka_toppar_leader(rktp, 1/*proper broker*/);
        avail = rkb ? 1 : 0;
        if (rkb)
                rd_kafka_broker_destroy(rkb);
	rd_kafka_toppar_destroy(s_rktp);
	return avail;
}


void *rd_kafka_topic_opaque (const rd_kafka_topic_t *app_rkt) {
        return rd_kafka_topic_a2i(app_rkt)->rkt_conf.opaque;
}
