// SPDX-License-Identifier: BSD-2-Clause
//
// Copyright (c) 2016-2018, NetApp, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "pn.h"
#include "conn.h"


struct ev_loop;


SPLAY_GENERATE(pm_nr_splay, pkt_meta, nr_node, pm_nr_cmp)


int pm_nr_cmp(const struct pkt_meta * const a, const struct pkt_meta * const b)
{
    return (a->hdr.nr > b->hdr.nr) - (a->hdr.nr < b->hdr.nr);
}


static inline __attribute__((always_inline, nonnull)) epoch_t
epoch_for_pn(const struct pn_space * pn)
{
    if (pn == &pn->c->pn_init.pn)
        return ep_init;
    if (pn == &pn->c->pn_hshk.pn)
        return ep_hshk;
    return pn->c->state == conn_opng ? ep_0rtt : ep_data;
}


void ack_alarm(struct ev_loop * const l __attribute__((unused)),
               ev_timer * const w,
               int e)
{
    struct pn_space * const pn = w->data;
    if (e)
        warn(DBG, "ACK timer fired on %s conn %s epoch %u", conn_type(pn->c),
             scid2str(pn->c), epoch_for_pn(pn));

    tx_ack(pn->c, epoch_for_pn(pn));
    ev_timer_stop(loop, &pn->ack_alarm);
}


void init_pn(struct pn_space * const pn, struct q_conn * const c)
{
    diet_init(&pn->recv);
    diet_init(&pn->acked);
    splay_init(&pn->sent_pkts);
    pn->lg_sent = UINT64_MAX;
    pn->c = c;

    // initialize ACK timeout
    pn->ack_alarm.data = pn;
    pn->ack_alarm.repeat = kDelayedAckTimeout;
    ev_init(&pn->ack_alarm, ack_alarm);
}


void free_pn(struct pn_space * const pn)
{
    ev_timer_stop(loop, &pn->ack_alarm);
    diet_free(&pn->recv);
    diet_free(&pn->acked);
}
