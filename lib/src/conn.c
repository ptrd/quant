// SPDX-License-Identifier: BSD-2-Clause
//
// Copyright (c) 2016-2019, NetApp, Inc.
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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>

#define klib_unused

// IWYU pragma: no_include <picotls/../picotls.h>

#include <ev.h>
#include <picotls.h> // IWYU pragma: keep
#include <picotls/openssl.h>
#include <quant/quant.h>
#include <warpcore/warpcore.h>

#include "conn.h"
#include "diet.h"
#include "frame.h"
#include "pkt.h"
#include "quic.h"
#include "recovery.h"
#include "stream.h"


#undef CONN_STATE
#define CONN_STATE(k, v) [v] = #k

const char * const conn_state_str[] = {CONN_STATES};

struct q_conn_sl c_ready = sl_head_initializer(c_ready);

khash_t(conns_by_ipnp) * conns_by_ipnp;
khash_t(conns_by_id) * conns_by_id;


static inline int __attribute__((nonnull))
sockaddr_in_cmp(const struct sockaddr_in * const a,
                const struct sockaddr_in * const b)
{
    int diff =
        (a->sin_family > b->sin_family) - (a->sin_family < b->sin_family);
    if (diff)
        return diff;

    diff = (a->sin_port > b->sin_port) - (a->sin_port < b->sin_port);
    if (diff)
        return diff;

    return (a->sin_addr.s_addr > b->sin_addr.s_addr) -
           (a->sin_addr.s_addr < b->sin_addr.s_addr);
}


SPLAY_GENERATE(cids_by_seq, cid, node_seq, cids_by_seq_cmp)


bool vers_supported(const uint32_t v)
{
    if (is_force_vneg_vers(v) || is_rsvd_vers(v))
        return false;

    for (uint8_t i = 0; i < ok_vers_len; i++)
        if (v == ok_vers[i])
            return true;

    // we're out of matching candidates
    warn(INF, "no vers in common");
    return false;
}


struct ooo_0rtt_by_cid ooo_0rtt_by_cid = splay_initializer(&ooo_0rtt_by_cid);


SPLAY_GENERATE(ooo_0rtt_by_cid, ooo_0rtt, node, ooo_0rtt_by_cid_cmp)


static inline epoch_t __attribute__((always_inline, nonnull))
epoch_in(const struct q_conn * const c)
{
    const size_t epoch = ptls_get_read_epoch(c->tls.t);
    switch (epoch) {
    case 0:
        return ep_init;
    case 1:
        return ep_0rtt;
    case 2:
        return ep_hshk;
    case 3:
        return ep_data;
    default:
        die("unhandled epoch %zu", epoch);
    }
}


static inline uint64_t __attribute__((const, always_inline))
conns_by_ipnp_key(const uint16_t sport,
                  const uint16_t dport,
                  const uint32_t dip)
{
    return ((uint64_t)dip << sizeof(dip) * 8) |
           ((uint64_t)sport << sizeof(sport) * 8) | (uint64_t)dport;
}


static struct q_conn * __attribute__((nonnull))
get_conn_by_ipnp(const uint16_t sport, const struct sockaddr_in * const peer)
{
    const khiter_t k =
        kh_get(conns_by_ipnp, conns_by_ipnp,
               (khint64_t)conns_by_ipnp_key(sport, peer->sin_port,
                                            peer->sin_addr.s_addr));
    if (unlikely(k == kh_end(conns_by_ipnp)))
        return 0;
    return kh_val(conns_by_ipnp, k);
}


static struct q_conn * __attribute__((nonnull))
get_conn_by_cid(struct cid * const scid)
{
    const khiter_t k = kh_get(conns_by_id, conns_by_id, scid);
    if (unlikely(k == kh_end(conns_by_id)))
        return 0;
    return kh_val(conns_by_id, k);
}


static inline void __attribute__((nonnull))
cids_by_id_ins(khash_t(cids_by_id) * const cbi, struct cid * const id)
{
    int ret;
    const khiter_t k = kh_put(cids_by_id, cbi, id, &ret);
    ensure(ret >= 1, "inserted");
    kh_val(cbi, k) = id;
}


static inline void __attribute__((nonnull))
cids_by_id_del(khash_t(cids_by_id) * const cbi, struct cid * const id)
{
    const khiter_t k = kh_get(cids_by_id, cbi, id);
    ensure(k != kh_end(cbi), "found");
    kh_del(cids_by_id, cbi, k);
}


static struct cid * __attribute__((nonnull))
get_cid_by_id(const khash_t(cids_by_id) * const cbi, struct cid * const id)
{
    const khiter_t k = kh_get(cids_by_id, cbi, id);
    if (unlikely(k == kh_end(cbi)))
        return 0;
    return kh_val(cbi, k);
}


static void __attribute__((nonnull)) use_next_dcid(struct q_conn * const c)
{
    const struct cid which = {.seq = c->dcid->seq + 1};
    struct cid * const dcid = splay_find(cids_by_seq, &c->dcids_by_seq, &which);
    ensure(dcid, "have dcid");

    warn(NTE, "migration to dcid %s for %s conn (was %s)", cid2str(dcid),
         conn_type(c), cid2str(c->dcid));

    c->tx_retire_cid = c->dcid->retired = true;
    c->dcid = dcid;
}


#ifndef NDEBUG
static void log_sent_pkts(struct q_conn * const c)
{
    for (epoch_t e = ep_init; e < ep_data; e++) {
        char sent_pkts_buf[1024] = "";
        uint64_t prev = UINT64_MAX;
        struct pkt_meta * p = 0;
        struct pn_space * const pn = pn_for_epoch(c, e);
        splay_foreach (p, pm_by_nr, &pn->sent_pkts) {
            char tmp[1024] = "";
            snprintf(tmp, sizeof(tmp), "%s%s" FMT_PNR_OUT NRM " ",
                     has_stream_data(p) ? REV : "",
                     is_ack_eliciting(&p->frames) ? BLD : "",
                     prev == UINT64_MAX
                         ? p->hdr.nr
                         : shorten_ack_nr(p->hdr.nr, p->hdr.nr - prev));
            strncat(sent_pkts_buf, tmp,
                    sizeof(sent_pkts_buf) - strlen(sent_pkts_buf) - 1);
            prev = p->hdr.nr;
        }
        if (sent_pkts_buf[0])
            warn(DBG, "epoch %u%s unacked: %s", e, e == 1 ? "/3" : "",
                 sent_pkts_buf);
    }
}
#endif


static void __attribute__((nonnull))
rtx_pkt(struct q_stream * const s, struct w_iov * const v)
{
    ensure(meta(v).is_rtx == false, "cannot RTX an RTX");
    // on RTX, remember orig pkt meta data
    const uint16_t data_start = meta(v).stream_data_start;
    struct w_iov * const r = alloc_iov(s->c->w, 0, data_start);
    pm_cpy(&meta(r), &meta(v), true); // copy pkt meta data
    memcpy(r->buf - data_start, v->buf - data_start, data_start);
    meta(r).is_rtx = true;
    sl_insert_head(&meta(v).rtx, &meta(r), rtx_next);
    sl_insert_head(&meta(r).rtx, &meta(v), rtx_next);

    // we reinsert meta(v) with its new pkt nr in on_pkt_sent()
    ensure(splay_remove(pm_by_nr, &meta(v).pn->sent_pkts, &meta(v)), "removed");
    ensure(splay_insert(pm_by_nr, &meta(r).pn->sent_pkts, &meta(r)) == 0,
           "inserted");
}


static void __attribute__((nonnull)) do_tx(struct q_conn * const c)
{
    c->needs_tx = false;

    if (unlikely(sq_empty(&c->txq)))
        return;

    if (sq_len(&c->txq) > 1 && unlikely(is_lh(*sq_first(&c->txq)->buf)))
        coalesce(&c->txq);

    // transmit encrypted/protected packets
    w_tx(c->sock, &c->txq);
    while (w_tx_pending(&c->txq))
        w_nic_tx(c->w);

    // txq was allocated straight from warpcore, no metadata needs to be freed
    // const uint64_t avail = sq_len(&c->w->iov);
    // const uint64_t sql = sq_len(&c->txq);
    w_free(&c->txq);
    // warn(CRT, "w_free %" PRIu64 " (avail %" PRIu64 "->%" PRIu64 ")", sql,
    // avail, sq_len(&c->w->iov));
}


void do_conn_fc(struct q_conn * const c, const uint16_t len)
{
    if (unlikely(c->state == conn_clsg || c->state == conn_drng))
        return;

    if (len && c->out_data_str + len + MAX_PKT_LEN > c->tp_out.max_data)
        c->blocked = true;

    // check if we need to do connection-level flow control
    if (c->in_data_str * 2 > c->tp_in.max_data) {
        c->tx_max_data = true;
        c->tp_in.max_data *= 2;
    }
}


static void __attribute__((nonnull)) do_conn_mgmt(struct q_conn * const c)
{
    if (c->state == conn_clsg || c->state == conn_drng)
        return;

    // do we need to make more stream IDs available?
    if (likely(c->state == conn_estb)) {
        do_stream_id_fc(c, c->cnt_uni, false, true);
        do_stream_id_fc(c, c->cnt_bidi, true, true);
    }

    if (likely(c->tp_out.disable_migration == false) &&
        unlikely(c->do_migration == true)) {
        if (c->is_clnt &&
            // does the peer have a CID for us that they can switch to?
            splay_count(&c->scids_by_seq) >= 2) {
            const struct cid * const dcid =
                splay_max(cids_by_seq, &c->dcids_by_seq);
            // if higher-numbered destination CIDs are available, switch to next
            if (dcid && dcid->seq > c->dcid->seq) {
                use_next_dcid(c);
                // don't migrate again for a while
                c->do_migration = false;
                ev_timer_again(loop, &c->key_flip_alarm);
            }
        }
        // send new CID if the peer doesn't have one remaining
        c->tx_ncid = (splay_count(&c->scids_by_seq) < 2);
    }
}


static bool __attribute__((nonnull))
tx_stream(struct q_stream * const s, const uint32_t limit)
{
    struct q_conn * const c = s->c;
    const bool has_data = (sq_len(&s->out) && out_fully_acked(s) == false);

    // warn(ERR,
    //      "%s strm id=" FMT_SID ", cnt=%" PRIu64
    //      ", has_data=%u, needs_ctrl=%u, blocked=%u, fully_acked=%u",
    //      conn_type(c), s->id, sq_len(&s->out), has_data, needs_ctrl(s),
    //      s->blocked, out_fully_acked(s));

    // check if we should skip TX on this stream
    if (has_data == false || s->blocked ||
        // unless for 0-RTT, is this a regular stream during conn open?
        unlikely(c->try_0rtt == false && s->id >= 0 && c->state != conn_estb)) {
        // warn(ERR, "skip " FMT_SID, s->id);
        return true;
    }

    warn(DBG, "TX on %s conn %s strm " FMT_SID " w/%" PRIu64 " pkt%s in queue",
         conn_type(c), cid2str(c->scid), s->id, sq_len(&s->out),
         plural(sq_len(&s->out)));

    uint32_t encoded = 0;
    struct w_iov * v = s->out_una;
    bool wnd_full = false;
    sq_foreach_from (v, &s->out, next) {
        if (unlikely(has_wnd(c, v->len) == false && limit == 0)) {
            wnd_full = true;
            break;
        }

        if (unlikely(meta(v).is_acked)) {
            // warn(INF, "skip ACK'ed pkt " FMT_PNR_OUT, meta(v).hdr.nr);
            continue;
        }

        if (limit == 0 && meta(v).udp_len && meta(v).is_lost == false) {
            // warn(INF, "skip non-lost TX'ed pkt " FMT_PNR_OUT,
            // meta(v).hdr.nr);
            continue;
        }

        const bool do_rtx = meta(v).is_lost || (limit && meta(v).udp_len);
        if (unlikely(do_rtx))
            rtx_pkt(s, v);

        if (likely(c->state == conn_estb && s->id >= 0)) {
            do_stream_fc(s, v->len);
            do_conn_fc(c, v->len);
        }

        if (unlikely(enc_pkt(s, do_rtx, true, limit > 0, v) == false))
            // XXX do we need to undo rtx_pkt() here?
            continue;
        encoded++;

        if (unlikely(s->blocked || c->blocked))
            break;

        if (unlikely(limit && encoded == limit)) {
            warn(DBG, "tx limit %u reached", limit);
            break;
        }
    }

#ifndef NDEBUG
    log_sent_pkts(c);
#endif
    return (unlikely(limit) && encoded == limit) || wnd_full == false;
}


static void __attribute__((nonnull))
tx_ack(struct q_conn * const c, const epoch_t e, const bool tx_ack_eliciting)
{
    do_conn_mgmt(c);
    struct w_iov * const v = alloc_iov(c->w, 0, 0);
    enc_pkt(c->cstreams[e], false, false, tx_ack_eliciting, v);
    do_tx(c);
    if (is_ack_eliciting(&meta(v).frames) == false)
        // enc_pkt() can end up encoding ACK eliciting frames, so check
        free_iov(v);
}


void tx(struct q_conn * const c, const uint32_t limit)
{
    if (unlikely(c->state == conn_drng))
        return;

    if (unlikely(c->state == conn_qlse)) {
        enter_closing(c);
        tx_ack(c, ep_data, false);
        goto done;
    }

    if (unlikely(c->tx_rtry)) {
        tx_ack(c, ep_init, false);
        goto done;
    }

    if (unlikely(c->state == conn_opng) && c->is_clnt && c->try_0rtt &&
        c->pn_data.out_0rtt.aead == 0)
        // if we have no 0-rtt keys here, the ticket didn't have any - disable
        c->try_0rtt = false;

    if (unlikely(c->blocked))
        goto done;

    do_conn_mgmt(c);

    if (likely(c->state != conn_clsg))
        for (epoch_t e = ep_init; e <= ep_data; e++) {
            if (c->cstreams[e] == 0)
                continue;
            if (tx_stream(c->cstreams[e], limit) == false)
                goto done;
        }

    struct q_stream * s;
    kh_foreach_value(c->streams_by_id, s, {
        if (tx_stream(s, limit) == false)
            break;
    });

done:;
    // make sure we send enough packets when we're called with a limit
    uint64_t sent = sq_len(&c->txq);
    if (likely(sent))
        do_tx(c);
    while ((unlikely(limit) && sent < limit) ||
           (c->tx_path_chlg && sent == 0)) {
        tx_ack(c, c->tls.epoch_out, true);
        sent++;
    }
}


void tx_w(struct ev_loop * const l __attribute__((unused)),
          ev_async * const w,
          int e __attribute__((unused)))
{
    tx(w->data, 0);
}


static inline void __attribute__((nonnull))
conns_by_id_ins(struct q_conn * const c, struct cid * const id)
{
    int ret;
    const khiter_t k = kh_put(conns_by_id, conns_by_id, id, &ret);
    ensure(ret >= 1, "inserted");
    kh_val(conns_by_id, k) = c;
}


static inline void __attribute__((nonnull))
conns_by_id_del(struct cid * const id)
{
    const khiter_t k = kh_get(conns_by_id, conns_by_id, id);
    ensure(k != kh_end(conns_by_id), "found");
    kh_del(conns_by_id, conns_by_id, k);
}


static void __attribute__((nonnull)) update_act_scid(struct q_conn * const c)
{
    // server picks a new random cid
    struct cid nscid = {.len = SERV_SCID_LEN};
    ptls_openssl_random_bytes(nscid.id, sizeof(nscid.id) + sizeof(nscid.srt));
    cid_cpy(&c->odcid, c->scid);
    warn(NTE, "hshk switch to scid %s for %s %s conn (was %s)", cid2str(&nscid),
         conn_state_str[c->state], conn_type(c), cid2str(c->scid));
    conns_by_id_del(c->scid);
    cids_by_id_del(c->scids_by_id, c->scid);
    cid_cpy(c->scid, &nscid);
    cids_by_id_ins(c->scids_by_id, c->scid);
    conns_by_id_ins(c, c->scid);
}


void add_scid(struct q_conn * const c, struct cid * const id)
{
    ensure(id->len, "len 0");
    struct cid * scid = splay_find(cids_by_seq, &c->scids_by_seq, id);
    ensure(scid == 0, "cid is new");
    scid = get_cid_by_id(c->scids_by_id, id);
    ensure(scid == 0, "cid is new");

    // warn(ERR, "new scid %s", cid2str(id));
    scid = calloc(1, sizeof(*scid));
    ensure(scid, "could not calloc");
    cid_cpy(scid, id);
    ensure(splay_insert(cids_by_seq, &c->scids_by_seq, scid) == 0, "inserted");
    cids_by_id_ins(c->scids_by_id, scid);
    if (c->scid == 0)
        c->scid = scid;
    conns_by_id_ins(c, scid);
}


void add_dcid(struct q_conn * const c, const struct cid * const id)
{
    struct cid * dcid = splay_find(cids_by_seq, &c->dcids_by_seq, id);

    if (dcid == 0) {
        // warn(ERR, "new dcid %s", cid2str(id));
        dcid = calloc(1, sizeof(*dcid));
        ensure(dcid, "could not calloc");
        cid_cpy(dcid, id);
        ensure(splay_insert(cids_by_seq, &c->dcids_by_seq, dcid) == 0,
               "inserted");
        if (c->dcid == 0)
            c->dcid = dcid;
    } else {
        warn(NTE, "hshk switch to dcid %s for %s conn (was %s)", cid2str(id),
             conn_type(c), cid2str(c->dcid));
        cid_cpy(dcid, id);
    }
}


static inline void __attribute__((nonnull))
conns_by_ipnp_ins(struct q_conn * const c)
{
    int ret;
    const khiter_t k =
        kh_put(conns_by_ipnp, conns_by_ipnp,
               (khint64_t)conns_by_ipnp_key(c->sport, c->peer.sin_port,
                                            c->peer.sin_addr.s_addr),
               &ret);
    ensure(ret >= 1, "inserted");
    kh_val(conns_by_ipnp, k) = c;
}


static inline void __attribute__((nonnull))
conns_by_ipnp_del(struct q_conn * const c)
{
    const khiter_t k =
        kh_get(conns_by_ipnp, conns_by_ipnp,
               (khint64_t)conns_by_ipnp_key(c->sport, c->peer.sin_port,
                                            c->peer.sin_addr.s_addr));
    ensure(k != kh_end(conns_by_ipnp), "found");
    kh_del(conns_by_ipnp, conns_by_ipnp, k);
}


static void __attribute__((nonnull))
conns_by_ipnp_update(struct q_conn * const c,
                     const struct sockaddr_in * const peer)
{
    conns_by_ipnp_del(c);
    c->peer = *peer;
    conns_by_ipnp_ins(c);
}


static void __attribute__((nonnull)) rx_crypto(struct q_conn * const c)
{
    struct q_stream * const s = c->cstreams[epoch_in(c)];
    while (!sq_empty(&s->in)) {
        // take the data out of the crypto stream
        struct w_iov * const iv = sq_first(&s->in);
        sq_remove_head(&s->in, next);
        meta(iv).stream = 0;
        // and process it
        if (tls_io(s, iv))
            continue;

        if (c->state == conn_idle || c->state == conn_opng) {
            conn_to_state(c, conn_estb);
            if (c->is_clnt)
                maybe_api_return(q_connect, c, 0);
            else {
                // TODO: find a better way to send NEW_TOKEN
                make_rtry_tok(c);
                if (c->needs_accept == false) {
                    sl_insert_head(&accept_queue, c, node_aq);
                    c->needs_accept = true;
                }
                maybe_api_return(q_accept, 0, 0);
            }
        }
    }
}


static void __attribute__((nonnull))
vneg_or_rtry_resp(struct q_conn * const c, const bool is_vneg)
{
    // reset FC state
    c->in_data_str = c->out_data_str = 0;

    for (epoch_t e = ep_init; e <= ep_data; e++)
        if (c->cstreams[e])
            reset_stream(c->cstreams[e], true);

    struct q_stream * s;
    kh_foreach_value(c->streams_by_id, s, { reset_stream(s, false); });

    // reset packet number spaces
    const uint64_t lg_sent_ini = c->pn_init.pn.lg_sent;
    reset_pn(&c->pn_init.pn);
    reset_pn(&c->pn_hshk.pn);
    reset_pn(&c->pn_data.pn);
    if (is_vneg)
        // we need to continue in the pkt nr sequence
        c->pn_init.pn.lg_sent = lg_sent_ini;

    // reset CC state
    init_rec(c);

    // reset TLS state and create new CH
    init_tls(c, (char *)c->tls.alpn.base);
    tls_io(c->cstreams[ep_init], 0);
}


#ifndef NDEBUG
static bool __attribute__((const))
pkt_ok_for_epoch(const uint8_t flags, const epoch_t epoch)
{
    switch (epoch) {
    case ep_init:
        return pkt_type(flags) == LH_INIT || pkt_type(flags) == LH_RTRY;
    case ep_0rtt:
    case ep_hshk:
        return is_lh(flags);
    case ep_data:
        return true;
    }
}
#endif


static bool __attribute__((nonnull)) rx_pkt(struct q_conn * const c,
                                            struct w_iov * v,
                                            struct w_iov_sq * const x,
                                            const struct cid * const odcid
#ifdef NDEBUG
                                            __attribute__((unused))
#endif
                                            ,
                                            const uint8_t * const tok,
                                            const uint16_t tok_len)
{
    bool ok = false;

    log_pkt("RX", v, v->ip, v->port, odcid, tok, tok_len);
    c->in_data += meta(v).udp_len;

    if (unlikely(meta(v).is_reset)) {
        warn(INF, BLU BLD "STATELESS RESET" NRM " token=%s",
             hex2str(c->dcid->srt, sizeof(c->dcid->srt)));
        goto done;
    }

    switch (c->state) {
    case conn_idle:
        c->vers = meta(v).hdr.vers;
        if (c->tx_rtry) {
            // tx_rtry is currently always set on port 4434
            if (meta(v).hdr.type == LH_INIT && tok_len) {
                if (verify_rtry_tok(c, tok, tok_len) == false) {
                    warn(ERR, "retry token verification failed");
                    enter_closing(c);
                    goto done;
                } else
                    c->tx_rtry = false;
            } else {
                warn(INF, "sending retry");
                // send a RETRY
                make_rtry_tok(c);
                ok = c->needs_tx = true;
                update_act_scid(c);
                goto done;
            }
        }

        // this is a new connection

        // warn(INF, "supporting clnt-requested vers 0x%08x", c->vers);
        if (dec_frames(c, &v) == UINT16_MAX)
            goto done;

        // if the CH doesn't include any crypto frames, bail
        if (has_frame(v, FRM_CRY) == false) {
            warn(ERR, "initial pkt w/o crypto frames");
            enter_closing(c);
            goto done;
        }

        init_tp(c);

        // check if any reordered 0-RTT packets are cached for this CID
        const struct ooo_0rtt which = {.cid = meta(v).hdr.dcid};
        struct ooo_0rtt * const zo =
            splay_find(ooo_0rtt_by_cid, &ooo_0rtt_by_cid, &which);
        if (zo) {
            warn(INF, "have reordered 0-RTT pkt (t=%f sec) for %s conn %s",
                 ev_now(loop) - zo->t, conn_type(c), cid2str(c->scid));
            ensure(splay_remove(ooo_0rtt_by_cid, &ooo_0rtt_by_cid, zo),
                   "removed");
            sq_insert_head(x, zo->v, next);
            free(zo);
        }
        conn_to_state(c, conn_opng);

        // server limits response to 3x incoming pkt
        c->path_val_win = 3 * meta(v).udp_len;

        // server picks a new random cid
        update_act_scid(c);

        ok = true;
        break;

    case conn_opng:
        if (meta(v).hdr.vers == 0) {
            // this is a vneg pkt
            meta(v).hdr.nr = UINT64_MAX;
            if (c->vers != ok_vers[0]) {
                // we must have already reacted to a prior vneg pkt
                warn(INF, "ignoring spurious vneg response");
                goto done;
            }

            // handle an incoming vers-neg packet
            const uint32_t try_vers = clnt_vneg(&v->buf[meta(v).hdr.hdr_len],
                                                v->len - meta(v).hdr.hdr_len);
            if (try_vers == 0) {
                // no version in common with serv
                enter_closing(c);
                goto done;
            }

            vneg_or_rtry_resp(c, true);
            c->vers = try_vers;
            warn(INF, "serv didn't like vers 0x%08x, retrying with 0x%08x",
                 c->vers_initial, c->vers);
            ok = true;
            goto done;
        }

        if (unlikely(meta(v).hdr.vers != c->vers)) {
            warn(ERR,
                 "serv response w/vers 0x%08x to CI w/vers 0x%08x, ignoring",
                 meta(v).hdr.vers, c->vers);
            goto done;
        }

        if (meta(v).hdr.type == LH_RTRY) {
            meta(v).hdr.nr = UINT64_MAX;
            if (c->tok_len) {
                // we already had an earlier RETRY on this connection
                err_close(c, ERR_PROTOCOL_VIOLATION, 0, "rx 2nd retry");
                goto done;
            }

            // handle an incoming retry packet
            c->tok_len = tok_len;
            memcpy(c->tok, tok, c->tok_len);
            vneg_or_rtry_resp(c, false);
            warn(INF, "handling serv retry w/tok %s",
                 hex2str(c->tok, c->tok_len));
            ok = true;
            goto done;
        }

        // server accepted version -
        // if we get here, this should be a regular server-hello
        ok = (dec_frames(c, &v) != UINT16_MAX);
        break;

    case conn_estb:
    case conn_qlse:
    case conn_clsg:
    case conn_drng:
        if (is_lh(meta(v).hdr.flags) && meta(v).hdr.vers == 0) {
            // we shouldn't get another vers-neg packet here, ignore
            warn(NTE, "ignoring spurious vneg response");
            goto done;
        }

        // ignore 0-RTT packets if we're not doing 0-RTT
        if (c->did_0rtt == false && meta(v).hdr.type == LH_0RTT) {
            warn(NTE, "ignoring 0-RTT pkt");
            goto done;
        }

        if (dec_frames(c, &v) == UINT16_MAX)
            goto done;

        ok = true;
        break;

    case conn_clsd:
        warn(NTE, "ignoring pkt for closed %s conn", conn_type(c));
        break;
    }

done:
    if (unlikely(ok == false))
        return false;

    if (likely(meta(v).hdr.nr != UINT64_MAX)) {
        struct pn_space * const pn = pn_for_pkt_type(c, meta(v).hdr.type);
        // update ECN info
        switch (v->flags & IPTOS_ECN_MASK) {
        case IPTOS_ECN_ECT1:
            pn->ect1_cnt++;
            break;
        case IPTOS_ECN_ECT0:
            pn->ect0_cnt++;
            break;
        case IPTOS_ECN_CE:
            pn->ce_cnt++;
            break;
        }
        pn->pkts_rxed_since_last_ack_tx++;
    }

    return true;
}


#ifdef FUZZING
void
#else
static void __attribute__((nonnull))
#endif
rx_pkts(struct w_iov_sq * const x,
        struct q_conn_sl * const crx,
        const struct w_sock * const ws)
{
    struct cid outer_dcid = {0};
    while (!sq_empty(x)) {
        struct w_iov * const xv = sq_first(x);
        sq_remove_head(x, next);

        // warn(DBG, "rx idx %u (avail %" PRIu64 ") len %u type 0x%02x",
        //      w_iov_idx(xv), sq_len(&xv->w->iov), xv->len, *xv->buf);

#if !defined(NDEBUG) && !defined(FUZZING) &&                                   \
    !defined(NO_FUZZER_CORPUS_COLLECTION)
        // when called from the fuzzer, v->ip is zero
        if (xv->ip)
            write_to_corpus(corpus_pkt_dir, xv->buf, xv->len);
#endif

        // allocate new w_iov for the (eventual) unencrypted data and meta-data
        struct w_iov * const v = alloc_iov(ws->w, 0, 0);
        v->ip = xv->ip;
        v->port = xv->port;
        v->flags = xv->flags;

        const bool is_clnt = w_connected(ws);
        struct q_conn * c = 0;
        struct cid odcid;
        uint8_t tok[MAX_PKT_LEN];
        uint16_t tok_len = 0;
        if (unlikely(!dec_pkt_hdr_beginning(xv, v, is_clnt, &odcid, tok,
                                            &tok_len))) {
            // we might still need to send a vneg packet
            if (w_connected(ws) == false) {
                warn(ERR, "received invalid %u-byte %s pkt, sending vneg",
                     v->len,
                     pkt_type_str(meta(v).hdr.flags, &meta(v).hdr.vers));
                tx_vneg_resp(ws, v);
            } else
                warn(ERR, "received invalid %u-byte %s pkt), ignoring", v->len,
                     pkt_type_str(meta(v).hdr.flags, &meta(v).hdr.vers));
            // can't log packet, because it may be too short for log_pkt()
            goto drop;
        }

        const struct sockaddr_in peer = {.sin_family = AF_INET,
                                         .sin_port = v->port,
                                         .sin_addr = {.s_addr = v->ip}};

        c = get_conn_by_cid(&meta(v).hdr.dcid);
        if (c == 0) {
            if (meta(v).hdr.dcid.len == 0)
                c = get_conn_by_ipnp(w_get_sport(ws), &peer);
            if (likely(is_lh(meta(v).hdr.flags)) && !is_clnt) {
                if (c && meta(v).hdr.type == LH_0RTT) {
                    if (c->did_0rtt)
                        warn(INF,
                             "got 0-RTT pkt for orig cid %s, new is %s, "
                             "accepting",
                             cid2str(&meta(v).hdr.dcid), cid2str(c->scid));
                    else {
                        warn(WRN,
                             "got 0-RTT pkt for orig cid %s, new is %s, "
                             "but rejected 0-RTT, ignoring",
                             cid2str(&meta(v).hdr.dcid), cid2str(c->scid));
                        goto drop;
                    }
                } else if (meta(v).hdr.type == LH_INIT) {
                    // validate minimum packet size
                    if (xv->len < MIN_INI_LEN) {
                        warn(ERR, "%u-byte Initial pkt too short (< %u)",
                             xv->len, MIN_INI_LEN);
                        goto drop;
                    }

                    if (vers_supported(meta(v).hdr.vers) == false ||
                        is_force_vneg_vers(meta(v).hdr.vers)) {
                        log_pkt("RX", v, v->ip, v->port, &odcid, tok, tok_len);
                        warn(WRN, "clnt-requested vers 0x%08x not supported",
                             meta(v).hdr.vers);
                        tx_vneg_resp(ws, v);
                        goto drop;
                    }

                    warn(NTE, "new serv conn on port %u from %s:%u w/cid=%s",
                         ntohs(w_get_sport(ws)), inet_ntoa(peer.sin_addr),
                         ntohs(peer.sin_port), cid2str(&meta(v).hdr.dcid));
                    c = new_conn(w_engine(ws), meta(v).hdr.vers,
                                 &meta(v).hdr.scid, &meta(v).hdr.dcid, &peer, 0,
                                 ntohs(w_get_sport(ws)), 0);
                    init_tls(c, 0);

                    // TODO: remove this interop hack eventually
                    if (ntohs(c->sport) == 4434)
                        c->tx_rtry = true;
                }
            }

        } else {
            if (meta(v).hdr.scid.len) {
                if (cid_cmp(&meta(v).hdr.scid, c->dcid) != 0) {
                    if (meta(v).hdr.vers && meta(v).hdr.type == LH_RTRY &&
                        cid_cmp(&odcid, c->dcid) != 0) {
                        log_pkt("RX", v, v->ip, v->port, &odcid, tok, tok_len);
                        warn(ERR, "retry dcid mismatch %s != %s, ignoring pkt",
                             hex2str(&odcid.id, odcid.len), cid2str(c->dcid));
                        goto drop;
                    }
                    if (c->state == conn_opng)
                        add_dcid(c, &meta(v).hdr.scid);
                }
            }

            if (cid_cmp(&meta(v).hdr.dcid, c->scid) != 0) {
                struct cid * const scid =
                    get_cid_by_id(c->scids_by_id, &meta(v).hdr.dcid);
                if (unlikely(scid == 0)) {
                    log_pkt("RX", v, v->ip, v->port, &odcid, tok, tok_len);
                    warn(ERR, "unknown scid %s, ignoring pkt",
                         cid2str(&meta(v).hdr.dcid));
                    goto drop;
                }

                if (scid->seq < c->scid->seq)
                    warn(DBG, "pkt has prev scid %s, accepting", cid2str(scid));
                else {
                    warn(NTE, "migration to scid %s for %s conn (was %s)",
                         cid2str(scid), conn_type(c), cid2str(c->scid));
                    c->scid = scid;
                }
            }

            // check if this pkt came from a new source IP and/or port
            if (sockaddr_in_cmp(&c->peer, &peer) != 0) {
                warn(NTE, "pkt came from new peer %s:%u, probing",
                     inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
                if (c->dcid->len == 0)
                    conns_by_ipnp_update(c, &peer);
                ptls_openssl_random_bytes(&c->path_chlg_out,
                                          sizeof(c->path_chlg_out));
                c->tx_path_chlg = true;
            }
        }

        if (c == 0) {
            warn(INF, "cannot find conn %s for %u-byte %s pkt",
                 cid2str(&meta(v).hdr.dcid), v->len,
                 pkt_type_str(meta(v).hdr.flags, &meta(v).hdr.vers));
#ifndef FUZZING
            // if this is a 0-RTT pkt, track it (may be reordered)
            if (meta(v).hdr.type == LH_0RTT) {
                struct ooo_0rtt * const zo = calloc(1, sizeof(*zo));
                ensure(zo, "could not calloc");
                cid_cpy(&zo->cid, &meta(v).hdr.dcid);
                zo->v = v;
                zo->t = ev_now(loop);
                ensure(splay_insert(ooo_0rtt_by_cid, &ooo_0rtt_by_cid, zo) == 0,
                       "inserted");
                log_pkt("RX", v, v->ip, v->port, &odcid, tok, tok_len);
                warn(INF, "caching 0-RTT pkt for unknown conn %s",
                     cid2str(&meta(v).hdr.dcid));
                goto next;
            }
#endif
            log_pkt("RX", v, v->ip, v->port, &odcid, tok, tok_len);
            warn(INF, "ignoring unexpected %s pkt for conn %s",
                 pkt_type_str(meta(v).hdr.flags, &meta(v).hdr.vers),
                 cid2str(&meta(v).hdr.dcid));
            goto drop;
        }

        if (likely((meta(v).hdr.vers && meta(v).hdr.type != LH_RTRY) ||
                   !is_lh(meta(v).hdr.flags))) {
            bool decoal;
            if (unlikely(meta(v).hdr.type == LH_INIT &&
                         c->cstreams[ep_init] == 0)) {
                // we already abandoned Initial pkt processing, ignore
                log_pkt("RX", v, v->ip, v->port, &odcid, tok, tok_len);
                warn(INF, "ignoring %u-byte %s pkt due to abandoned processing",
                     v->len,
                     pkt_type_str(meta(v).hdr.flags, &meta(v).hdr.vers));
                goto drop;
            } else if (unlikely(dec_pkt_hdr_remainder(xv, v, c, x, &decoal) ==
                                false)) {
                v->len = xv->len;
                log_pkt("RX", v, v->ip, v->port, &odcid, tok, tok_len);
                warn(ERR, "%s %u-byte %s pkt, ignoring",
                     pkt_ok_for_epoch(meta(v).hdr.flags, epoch_in(c))
                         ? "crypto fail on"
                         : "rx invalid",
                     v->len,
                     pkt_type_str(meta(v).hdr.flags, &meta(v).hdr.vers));
                goto drop;
            }

            // that dcid in split-out coalesced pkt matches outer pkt
            if (unlikely(decoal) && outer_dcid.len == 0) {
                // save outer dcid for checking
                cid_cpy(&outer_dcid, &meta(v).hdr.dcid);
                goto decoal_done;
            }

            if (unlikely(outer_dcid.len) &&
                cid_cmp(&outer_dcid, &meta(v).hdr.dcid) != 0) {
                warn(ERR,
                     "outer dcid %s != inner dcid %s during "
                     "decoalescing, ignoring %s pkt",
                     cid2str(&outer_dcid), cid2str(&meta(v).hdr.dcid),
                     pkt_type_str(meta(v).hdr.flags, &meta(v).hdr.vers));
                goto drop;
            }

            if (likely(decoal == false))
                // forget outer dcid
                outer_dcid.len = 0;
        }
    decoal_done:

        if (rx_pkt(c, v, x, &odcid, tok, tok_len)) {
            rx_crypto(c);

            // remember that we had a RX event on this connection
            if (!c->had_rx) {
                c->had_rx = true;
                sl_insert_head(crx, c, node_rx_int);
                c->min_rx_epoch = epoch_for_pkt_type(meta(v).hdr.type);
            } else
                c->min_rx_epoch =
                    MIN(c->min_rx_epoch, epoch_for_pkt_type(meta(v).hdr.type));
        }

        if (meta(v).stream == 0)
            // we didn't place this pkt in any stream - bye!
            goto drop;
        else if (unlikely(meta(v).stream->state == strm_clsd &&
                          sq_empty(&meta(v).stream->in)))
            free_stream(meta(v).stream);
        goto next;

    drop:
        free_iov(v);
    next:
        // warn(CRT, "w_free_iov idx %u (avail %" PRIu64 ")", w_iov_idx(xv),
        //      sq_len(&xv->w->iov) + 1);
        w_free_iov(xv);
    }
}


static void
rx(struct ev_loop * const l, ev_io * const rx_w, int _e __attribute__((unused)))
{
    // read from NIC
    struct w_sock * const ws = rx_w->data;
    w_nic_rx(w_engine(ws), -1);
    struct w_iov_sq x = w_iov_sq_initializer(x);
    struct q_conn_sl crx = sl_head_initializer(crx);
    w_rx(ws, &x);
    rx_pkts(&x, &crx, ws);

    // for all connections that had RX events
    while (!sl_empty(&crx)) {
        struct q_conn * const c = sl_first(&crx);
        sl_remove_head(&crx, node_rx_int);

        // clear the helper flags set above
        c->had_rx = false;

        if (unlikely(c->state == conn_drng))
            continue;

        // reset idle timeout
        ev_timer_again(l, &c->idle_alarm);

        // is a TX needed for this connection?
        if (c->needs_tx)
            tx(c, 0); // this clears c->needs_tx if we TX'ed

        for (epoch_t e = c->min_rx_epoch; e <= ep_data; e++) {
            if (c->cstreams[e] == 0 || e == ep_0rtt)
                // don't ACK abandoned and 0rtt pn spaces
                continue;
            struct pn_space * const pn = pn_for_epoch(c, e);
            switch (needs_ack(pn)) {
            case imm_ack:
                tx_ack(c, e, false);
                break;
            case del_ack:
                ev_timer_again(loop, &c->ack_alarm);
                break;
            case no_ack:
            case grat_ack:
                break;
            }
        }

        if (unlikely(c->tx_rtry))
            // if we sent a retry, forget the entire connection existed
            free_conn(c);
        else if (c->have_new_data) {
            if (!c->in_c_ready) {
                sl_insert_head(&c_ready, c, node_rx_ext);
                c->in_c_ready = true;
                maybe_api_return(q_rx_ready, 0, 0);
            }
        }
    }
}


void err_close(struct q_conn * const c,
               const uint16_t code,
               const uint8_t frm,
               const char * const fmt,
               ...)
{
#ifndef FUZZING
    if (unlikely(c->err_code)) {
        warn(WRN, "ignoring new err 0x%04x; existing err is 0x%04x (%s) ", code,
             c->err_code, c->err_reason);
        return;
    }
#endif

    va_list ap;
    va_start(ap, fmt);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
    const int ret = vsnprintf(c->err_reason, sizeof(c->err_reason), fmt, ap);
    ensure(ret >= 0, "vsnprintf() failed");
    va_end(ap);

    warn(ERR, "%s", c->err_reason);
    c->err_code = code;
    c->err_reason_len =
        (uint8_t)MIN((unsigned long)ret + 1, sizeof(c->err_reason));
    c->err_frm = frm;
    enter_closing(c);
}


static void __attribute__((nonnull))
key_flip(struct ev_loop * const l __attribute__((unused)),
         ev_timer * const w,
         int e __attribute__((unused)))
{
    struct q_conn * const c = w->data;
    c->do_key_flip = c->key_flips_enabled;
    // XXX we borrow the key flip timer for this
    c->do_migration = !c->tp_out.disable_migration;
}


static void __attribute__((nonnull))
enter_closed(struct ev_loop * const l __attribute__((unused)),
             ev_timer * const w,
             int e __attribute__((unused)))
{
    struct q_conn * const c = w->data;
    conn_to_state(c, conn_clsd);

    // terminate whatever API call is currently active
    maybe_api_return(c, 0);
    // TODO it looks like we don't need to cancel these calls anymore
    // maybe_api_return(q_accept, 0, 0);
    // maybe_api_return(q_rx_ready, 0, 0);
}


void enter_closing(struct q_conn * const c)
{
    if (c->state == conn_clsg)
        return;

    // stop alarms
    ev_timer_stop(loop, &c->rec.ld_alarm);
    ev_timer_stop(loop, &c->idle_alarm);
    ev_timer_stop(loop, &c->key_flip_alarm);
    ev_timer_stop(loop, &c->ack_alarm);

#ifndef FUZZING
    if ((c->state == conn_idle || c->state == conn_opng) && c->err_code == 0) {
#endif
        // no need to go closing->draining in these cases
        ev_invoke(loop, &c->closing_alarm, 0);
        return;
#ifndef FUZZING
    }
#endif

    // if we're going closing->draining, don't start the timer again
    if (!ev_is_active(&c->closing_alarm)) {
        // start closing/draining alarm (3 * RTO)
        const ev_tstamp dur =
            (3 * (is_zero(c->rec.srtt) ? kInitialRtt : c->rec.srtt) +
             4 * c->rec.rttvar);
        ev_timer_init(&c->closing_alarm, enter_closed, dur, 0);
#ifndef FUZZING
        ev_timer_start(loop, &c->closing_alarm);
        warn(DBG, "closing/draining alarm in %f sec on %s conn %s", dur,
             conn_type(c), cid2str(c->scid));
#endif
    }

    if (c->state != conn_drng) {
        c->needs_tx = true;
        conn_to_state(c, conn_clsg);
    }
}


static void __attribute__((nonnull))
idle_alarm(struct ev_loop * const l __attribute__((unused)),
           ev_timer * const w,
           int e __attribute__((unused)))
{
    struct q_conn * const c = w->data;
    warn(DBG, "idle timeout on %s conn %s", conn_type(c), cid2str(c->scid));

    conn_to_state(c, conn_drng);
    enter_closing(c);
}


static void __attribute__((nonnull))
ack_alarm(struct ev_loop * const l __attribute__((unused)),
          ev_timer * const w,
          int e __attribute__((unused)))
{
    struct q_conn * const c = w->data;
    warn(DBG, "ACK timer fired on %s conn %s", conn_type(c), cid2str(c->scid));
    tx_ack(c, ep_data, false);
}


void update_conn_conf(struct q_conn * const c,
                      const struct q_conn_conf * const cc)
{
    c->spinbit_enabled = cc ? cc->enable_spinbit : 0;

    // (re)set idle alarm
    c->idle_alarm.repeat = c->tp_in.idle_to =
        cc && cc->idle_timeout ? cc->idle_timeout : 10;
    ev_timer_again(loop, &c->idle_alarm);

    c->tp_out.disable_migration = cc ? cc->disable_migration : false;
    c->key_flips_enabled = cc ? cc->enable_tls_key_updates : false;

    if (c->tp_out.disable_migration == false || c->key_flips_enabled) {
        c->key_flip_alarm.repeat = cc ? cc->tls_key_update_frequency : 3;
        ev_timer_again(loop, &c->key_flip_alarm);
    }

#ifndef NDEBUG
    // XXX for testing, do a key flip and a migration ASAP (if enabled)
    c->do_key_flip = c->key_flips_enabled;
    c->do_migration = !c->tp_out.disable_migration;
#endif
}


struct q_conn * new_conn(struct w_engine * const w,
                         const uint32_t vers,
                         const struct cid * const dcid,
                         const struct cid * const scid,
                         const struct sockaddr_in * const peer,
                         const char * const peer_name,
                         const uint16_t port,
                         const struct q_conn_conf * const cc)
{
    struct q_conn * const c = calloc(1, sizeof(*c));
    ensure(c, "could not calloc");

    if (peer)
        c->peer = *peer;

    if (peer_name) {
        c->is_clnt = true;
        ensure(c->peer_name = strdup(peer_name), "could not dup peer_name");
    }

    // init next CIDs
    c->next_sid_bidi = c->is_clnt ? 0 : STRM_FL_SRV;
    c->next_sid_uni = c->is_clnt ? STRM_FL_UNI : STRM_FL_UNI | STRM_FL_SRV;
    c->tx_ncid = true;

    // init dcid
    splay_init(&c->dcids_by_seq);
    if (c->is_clnt) {
        struct cid ndcid = {.len = SERV_SCID_LEN};
        ptls_openssl_random_bytes(ndcid.id,
                                  sizeof(ndcid.id) + sizeof(ndcid.srt));
        cid_cpy(&c->odcid, &ndcid);
        add_dcid(c, &ndcid);
    } else if (dcid)
        add_dcid(c, dcid);

    c->vers = c->vers_initial = vers;
    c->scids_by_id = kh_init(cids_by_id);
    diet_init(&c->closed_streams);
    sq_init(&c->txq);

    // TODO most of these should become configurable via q_conn_conf
    c->tp_in.ack_del_exp = c->tp_out.ack_del_exp = DEF_ACK_DEL_EXP;
    c->tp_in.max_ack_del = c->tp_out.max_ack_del = 25;
    c->tp_in.max_data = INIT_MAX_BIDI_STREAMS * INIT_STRM_DATA_BIDI;
    c->tp_in.max_strm_data_uni = INIT_STRM_DATA_UNI;
    c->tp_in.max_strm_data_bidi_local = c->tp_in.max_strm_data_bidi_remote =
        INIT_STRM_DATA_BIDI;
    c->tp_in.max_streams_bidi = INIT_MAX_BIDI_STREAMS;
    c->tp_in.max_streams_uni = INIT_MAX_UNI_STREAMS;

    // initialize idle timeout
    c->idle_alarm.data = c;
    ev_init(&c->idle_alarm, idle_alarm);

    // initialize closing alarm
    c->closing_alarm.data = c;
    ev_init(&c->closing_alarm, enter_closed);

    // initialize key flip alarm (XXX also abused for migration)
    c->key_flip_alarm.data = c;
    ev_init(&c->key_flip_alarm, key_flip);

    // initialize ACK timeout
    c->ack_alarm.data = c;
    c->ack_alarm.repeat = c->tp_out.max_ack_del / 1000.0;
    ev_init(&c->ack_alarm, ack_alarm);

    // initialize packet number spaces
    init_pn(&c->pn_init.pn, c);
    init_pn(&c->pn_hshk.pn, c);
    init_pn(&c->pn_data.pn, c);

    // initialize recovery state
    init_rec(c);
    c->sockopt.enable_ecn = true;
    if (c->is_clnt)
        c->path_val_win = UINT64_MAX;

    // initialize socket and start a TX watcher
    ev_async_init(&c->tx_w, tx_w);
    c->tx_w.data = c;
    ev_set_priority(&c->tx_w, EV_MAXPRI - 1);
    ev_async_start(loop, &c->tx_w);

    c->w = w;
    c->sock = w_get_sock(w, w->ip, htons(port),
                         c->is_clnt && peer ? peer->sin_addr.s_addr : 0,
                         c->is_clnt && peer ? peer->sin_port : 0);
    if (c->sock == 0) {
        // TODO need to update zero checksums in update_conn_conf() somehow
        c->sockopt.enable_udp_zero_checksums =
            cc && cc->enable_udp_zero_checksums;
        c->rx_w.data = c->sock = w_bind(w, htons(port), &c->sockopt);
        if (unlikely(c->sock == 0))
            goto fail;
        ev_io_init(&c->rx_w, rx, w_fd(c->sock), EV_READ);
        ev_set_priority(&c->rx_w, EV_MAXPRI);
        ev_io_start(loop, &c->rx_w);
        c->holds_sock = true;
    } else if (unlikely(peer == 0))
        goto fail;
    c->sport = w_get_sport(c->sock);

    if (likely(c->is_clnt || c->holds_sock == false))
        update_conn_conf(c, cc);

    // init scid and add connection to global data structures
    if (dcid == 0 || dcid->len == 0)
        conns_by_ipnp_ins(c);
    splay_init(&c->scids_by_seq);
    struct cid nscid = {0};
    if (c->is_clnt) {
        nscid.len = CLNT_SCID_LEN;
        ptls_openssl_random_bytes(nscid.id, sizeof(nscid.id));
    } else if (scid)
        cid_cpy(&nscid, scid);
    if (nscid.len) {
        ptls_openssl_random_bytes(nscid.srt, sizeof(nscid.srt));
        add_scid(c, &nscid);
    }

    // create crypto streams
    c->streams_by_id = kh_init(streams_by_id);
    for (epoch_t e = ep_init; e <= ep_data; e++)
        if (e != ep_0rtt)
            new_stream(c, crpt_strm_id(e));

    if (nscid.len)
        warn(DBG, "%s conn %s on port %u created", conn_type(c),
             cid2str(c->scid), ntohs(c->sport));

    conn_to_state(c, conn_idle);
    return c;

fail:
    kh_destroy(cids_by_id, c->scids_by_id);
    free(c);
    return 0;
}


void free_scid(struct q_conn * const c, struct cid * const id)
{
    ensure(splay_remove(cids_by_seq, &c->scids_by_seq, id), "removed");
    cids_by_id_del(c->scids_by_id, id);
    conns_by_id_del(id);
    free(id);
}


void free_dcid(struct q_conn * const c, struct cid * const id)
{
    ensure(splay_remove(cids_by_seq, &c->dcids_by_seq, id), "removed");
    free(id);
}


void free_conn(struct q_conn * const c)
{
    // exit any active API call on the connection
    maybe_api_return(c, 0);

    if (c->holds_sock) {
        // only close the socket for the final server connection
        ev_io_stop(loop, &c->rx_w);
        w_close(c->sock);
    }
    ev_timer_stop(loop, &c->rec.ld_alarm);
    ev_timer_stop(loop, &c->closing_alarm);
    ev_timer_stop(loop, &c->key_flip_alarm);
    ev_timer_stop(loop, &c->idle_alarm);
    ev_timer_stop(loop, &c->ack_alarm);

    struct q_stream * s;
    kh_foreach_value(c->streams_by_id, s, { free_stream(s); });
    kh_destroy(streams_by_id, c->streams_by_id);

    for (epoch_t e = ep_init; e <= ep_data; e++)
        if (c->cstreams[e])
            free_stream(c->cstreams[e]);

    free_tls(c, false);

    // free packet number spaces
    free_pn(&c->pn_init.pn);
    free_pn(&c->pn_hshk.pn);
    free_pn(&c->pn_data.pn);

    ev_async_stop(loop, &c->tx_w);

    diet_free(&c->closed_streams);
    free(c->peer_name);

    // remove connection from global lists and free CID splays
    if (c->dcid == 0 || c->dcid->len == 0)
        conns_by_ipnp_del(c);

    while (!splay_empty(&c->scids_by_seq)) {
        struct cid * const id = splay_min(cids_by_seq, &c->scids_by_seq);
        free_scid(c, id);
    }

    while (!splay_empty(&c->dcids_by_seq)) {
        struct cid * const id = splay_min(cids_by_seq, &c->dcids_by_seq);
        free_dcid(c, id);
    }

    kh_destroy(cids_by_id, c->scids_by_id);

    if (c->in_c_ready)
        sl_remove(&c_ready, c, q_conn, node_rx_ext);

    if (c->needs_accept)
        sl_remove(&accept_queue, c, q_conn, node_aq);

    free(c);
}
