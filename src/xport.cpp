#include "internal.hpp"

#include "rdma/aead.hpp"

#include <errno.h>

#include <cstring>
#include <algorithm>

namespace rdma {

namespace {

void append_icrc(std::vector<uint8_t>& b) { put_u32(b, crc32c(b.data(), b.size())); }

void make_nonce(const FrameHdr& h, uint8_t n[aead::kNonceLen]) {
  n[0] = n[1] = n[2] = n[3] = n[4] = 0;
  n[5] = h.op;
  n[6] = uint8_t(h.conn >> 8);
  n[7] = uint8_t(h.conn);
  n[8] = uint8_t(h.seq >> 24);
  n[9] = uint8_t(h.seq >> 16);
  n[10] = uint8_t(h.seq >> 8);
  n[11] = uint8_t(h.seq);
}

// 帧子密钥 = KDF(root, 世代);root=会话密钥(已协商)或 PSK。
// conn_id 字段携带世代,收发双方各自按帧头派生同一子密钥,2^20 帧换代 → nonce 永不复用。
void qp_blk_key(QpImpl* qp, uint16_t gen, uint8_t key[aead::kKeyLen]) {
  if (qp->blk_valid && qp->blk_gen_cached == gen) {
    std::memcpy(key, qp->blk_key, aead::kKeyLen);
    return;
  }
  const uint8_t* root = qp->has_session ? qp->session_key : qp->ctx->psk;
  uint8_t g[2] = {uint8_t(gen >> 8), uint8_t(gen)};
  aead::kdf(root, aead::kKeyLen, "LRP-sec-2 blk", g, 2, qp->blk_key, aead::kKeyLen);
  qp->blk_gen_cached = gen;
  qp->blk_valid = true;
  std::memcpy(key, qp->blk_key, aead::kKeyLen);
}

// 安全封装:out = 头+扩展+载荷 → 头+密文+tag(再由调用方补 CRC)
void secure_finish(QpImpl* qp, const FrameHdr& h, std::vector<uint8_t>& out) {
  if (!qp->ctx->sec) return;
  size_t msg_len = out.size() - kHdrLen;
  uint8_t nonce[aead::kNonceLen];
  make_nonce(h, nonce);
  std::vector<uint8_t> ct(msg_len);
  uint8_t tag[aead::kTagLen];
  uint8_t key[aead::kKeyLen];
  qp_blk_key(qp, h.conn, key);
  aead::seal(key, nonce, out.data(), kHdrLen, out.data() + kHdrLen, msg_len, ct.data(), tag);
  out.resize(kHdrLen);
  out.insert(out.end(), ct.begin(), ct.end());
  out.insert(out.end(), tag, tag + aead::kTagLen);
}

std::vector<uint8_t> build_ack(QpImpl* qp, Syndrome syn, uint32_t msn) {
  uint32_t seq = msn & kPsnMask;
  uint16_t credit = uint16_t(qp->rq.size());
  if (syn == Syndrome::Ack && qp->ack_cached && qp->ack_msn == seq) {
    if (qp->ack_credit == credit) {
      return qp->ack_pkt;  // 同 msn 同内容:重发原帧,nonce 对应固定内容
    }
    // credit 变化需换内容 → 强制换世代(新 conn=新子密钥+新 nonce),避免同 nonce 异内容
    qp->frames_sent = ((qp->frames_sent >> 20) + 1) << 20;
  }
  std::vector<uint8_t> b;
  FrameHdr h;
  h.op = uint8_t(Opcode::Ack);
  h.qpn = qp->remote_qpn;
  h.seq = seq;
  h.conn = uint16_t(qp->frames_sent >> 20);
  put_hdr(b, h);
  put_ack_ext(b, uint8_t(syn), msn, credit);
  qp->adv_credit = credit;
  secure_finish(qp, h, b);
  append_icrc(b);
  if (syn == Syndrome::Ack) {
    qp->ack_cached = true;
    qp->ack_msn = seq;
    qp->ack_credit = credit;
    qp->ack_pkt = b;
  }
  return b;
}

void cache_and_send(QpImpl* qp, uint32_t psn, std::vector<uint8_t> pkt) {
  CachedResponse& c = qp->resp_cache[psn % kSendWindow];
  c.used = true;
  c.pkt = pkt;
  chan_xmit(qp->ctx, qp->peer, pkt);
}

void complete_slot(QpImpl* qp, Outstanding& o, WcStatus status, uint32_t byte_len) {
  if (o.retries == 0 && o.sent && o.sent_at_ms > 0) {
    double rtt = double(now_ms() - o.sent_at_ms) * 1000.0;
    if (qp->srtt_us <= 0) {
      qp->srtt_us = rtt;
      qp->rttvar_us = rtt / 2.0;
    } else {
      double d = qp->srtt_us - rtt;
      if (d < 0) d = -d;
      qp->rttvar_us = 0.75 * qp->rttvar_us + 0.25 * d;
      qp->srtt_us = 0.875 * qp->srtt_us + 0.125 * rtt;
    }
    double rto_us = qp->srtt_us + 4.0 * qp->rttvar_us;
    double rto_ms = rto_us / 1000.0;
    if (rto_ms < 20.0) rto_ms = 20.0;
    if (rto_ms > 1000.0) rto_ms = 1000.0;
    qp->rto_ms = uint32_t(rto_ms);
  }
  Cqe c;
  c.wr_id = o.wr.wr_id;
  c.opcode = o.wr.opcode;
  c.status = status;
  c.byte_len = byte_len;
  cq_post(qp->cq, c);
  o.used = false;
}

int build_request(QpImpl* qp, const SendWr& wr, uint32_t psn, std::vector<uint8_t>& out) {
  MrImpl* mr = find_mr(qp->ctx, wr.sge.lkey);
  uint32_t local_len = wr.sge.length;
  if (wr.opcode == WrOpcode::AtomicCas || wr.opcode == WrOpcode::AtomicFaa) local_len = 8;
  if (local_len > kMaxMsg) return -EINVAL;
  if (!mr_covers(mr, wr.sge.addr, local_len)) return -EINVAL;
  if (wr.opcode == WrOpcode::RdmaRead && local_len == 0) return -EINVAL;

  FrameHdr h;
  h.qpn = qp->remote_qpn;
  h.seq = psn & kPsnMask;
  h.conn = uint16_t(qp->frames_sent >> 20);
  ++qp->frames_sent;

  out.clear();
  switch (wr.opcode) {
    case WrOpcode::Send:
      h.op = uint8_t(Opcode::Send);
      put_hdr(out, h);
      put_bytes(out, reinterpret_cast<const void*>(uintptr_t(wr.sge.addr)), local_len);
      break;
    case WrOpcode::RdmaWrite:
      h.op = uint8_t(Opcode::RdmaWrite);
      put_hdr(out, h);
      put_wext(out, wr.remote_addr, wr.rkey, local_len);
      put_bytes(out, reinterpret_cast<const void*>(uintptr_t(wr.sge.addr)), local_len);
      break;
    case WrOpcode::RdmaRead:
      h.op = uint8_t(Opcode::RdmaReadReq);
      put_hdr(out, h);
      put_wext(out, wr.remote_addr, wr.rkey, local_len);
      break;
    case WrOpcode::AtomicCas:
      h.op = uint8_t(Opcode::AtomicCas);
      put_hdr(out, h);
      put_atom_ext(out, wr.remote_addr, wr.rkey, wr.compare, wr.swap);
      break;
    case WrOpcode::AtomicFaa:
      h.op = uint8_t(Opcode::AtomicFaa);
      put_hdr(out, h);
      put_atom_ext(out, wr.remote_addr, wr.rkey, 0, wr.swap);
      break;
  }
  secure_finish(qp, h, out);
  append_icrc(out);
  return 0;
}

void handle_ack(QpImpl* qp, uint8_t syn, uint32_t msn, uint32_t credit) {
  if (syn == uint8_t(Syndrome::NakRnr)) return;
  bool nak = (syn & 0x80) != 0;
  uint32_t bad_psn = (msn - 1) & kPsnMask;
  qp->peer_credit = credit;
  if (nak) cc_on_loss(qp);

  bool progressed = false;
  // cumulative ack retires fire-and-forget requests (send/write)
  for (uint32_t i = 0; i < kSendWindow; i++) {
    Outstanding& o = qp->out[i];
    if (!o.used) continue;
    if (psn_diff(msn, o.psn) <= 0) continue;
    bool is_bidi = o.wr.opcode == WrOpcode::RdmaRead || o.wr.opcode == WrOpcode::AtomicCas ||
                   o.wr.opcode == WrOpcode::AtomicFaa;
    if (is_bidi) continue;  // wait for its response payload
    WcStatus st = (nak && o.psn == bad_psn) ? WcStatus::RemAccessErr : WcStatus::Success;
    complete_slot(qp, o, st, o.wr.sge.length);
    progressed = true;
  }
  if (nak) {
    for (uint32_t i = 0; i < kSendWindow; i++) {
      Outstanding& o = qp->out[i];
      if (o.used && o.psn == bad_psn) {
        complete_slot(qp, o, WcStatus::RemAccessErr, 0);
      }
    }
  }
  if (progressed) cc_on_progress(qp);
}

void handle_response(QpImpl* qp, uint8_t opcode, uint32_t psn, Reader& r) {
  Outstanding& o = qp->out[psn % kSendWindow];
  if (!o.used || o.psn != (psn & kPsnMask)) return;

  if (opcode == uint8_t(Opcode::RdmaReadResp)) {
    size_t plen = r.remaining();
    MrImpl* mr = find_mr(qp->ctx, o.wr.sge.lkey);
    bool ok = plen == o.wr.sge.length && mr_covers(mr, o.wr.sge.addr, uint32_t(plen));
    WcStatus st = WcStatus::Success;
    if (ok) {
      std::memcpy(reinterpret_cast<void*>(uintptr_t(o.wr.sge.addr)), r.p + r.off, plen);
    } else {
      st = WcStatus::LocLenErr;
    }
    complete_slot(qp, o, st, ok ? uint32_t(plen) : 0);
    return;
  }

  if (opcode == uint8_t(Opcode::AtomicAck)) {
    if (r.remaining() < 8) return;
    uint64_t orig = r.u64();
    MrImpl* mr = find_mr(qp->ctx, o.wr.sge.lkey);
    if (mr_covers(mr, o.wr.sge.addr, 8)) {
      std::memcpy(reinterpret_cast<void*>(uintptr_t(o.wr.sge.addr)), &orig, 8);
      complete_slot(qp, o, WcStatus::Success, 8);
    } else {
      complete_slot(qp, o, WcStatus::LocLenErr, 0);
    }
  }
}

void handle_request(QpImpl* qp, uint8_t opcode, uint32_t psn, Reader& r) {
  int32_t d = psn_diff(psn, qp->expected_psn);

  if (d < 0) {
    CachedResponse& c = qp->resp_cache[psn % kSendWindow];
    if (c.used) chan_xmit(qp->ctx, qp->peer, c.pkt);
    return;
  }
  if (d > 0) {
    // gap: nudge sender back to expected_psn (Go-Back-N)
    chan_xmit(qp->ctx, qp->peer, build_ack(qp, Syndrome::Ack, qp->expected_psn));
    return;
  }

  switch (static_cast<Opcode>(opcode)) {
    case Opcode::Send: {
      if (qp->rq.empty()) {
        chan_xmit(qp->ctx, qp->peer, build_ack(qp, Syndrome::NakRnr, qp->expected_psn));
        return;
      }
      RecvWr rw = qp->rq.front();
      qp->rq.pop_front();
      size_t plen = r.remaining();
      MrImpl* mr = find_mr(qp->ctx, rw.sge.lkey);
      bool ok = plen <= rw.sge.length && mr_covers(mr, rw.sge.addr, uint32_t(plen));
      Cqe c;
      c.wr_id = rw.wr_id;
      c.opcode = WrOpcode::Send;
      if (ok) {
        std::memcpy(reinterpret_cast<void*>(uintptr_t(rw.sge.addr)), r.p + r.off, plen);
        c.status = WcStatus::Success;
        c.byte_len = uint32_t(plen);
      } else {
        c.status = WcStatus::LocLenErr;
      }
      cq_post(qp->cq, c);
      qp->expected_psn = (psn + 1) & kPsnMask;
      cache_and_send(qp, psn,
                     build_ack(qp, ok ? Syndrome::Ack : Syndrome::NakInvalid, qp->expected_psn));
      return;
    }
    case Opcode::RdmaWrite: {
      if (!r.need(kWextLen)) return;
      uint64_t va = r.u64();
      uint32_t rkey = r.u32();
      uint32_t dlen = r.u32();
      size_t plen = r.remaining();
      MrImpl* mr = find_mr(qp->ctx, rkey);
      bool ok = dlen == plen && dlen <= kMaxMsg && (mr && (mr->access & AccessRemoteWrite)) &&
                mr_covers(mr, va, dlen);
      if (ok) {
        std::memcpy(reinterpret_cast<void*>(uintptr_t(va)), r.p + r.off, plen);
      }
      qp->expected_psn = (psn + 1) & kPsnMask;
      cache_and_send(qp, psn,
                     build_ack(qp, ok ? Syndrome::Ack : Syndrome::NakInvalid, qp->expected_psn));
      return;
    }
    case Opcode::RdmaReadReq: {
      if (!r.need(kWextLen)) return;
      uint64_t va = r.u64();
      uint32_t rkey = r.u32();
      uint32_t dlen = r.u32();
      MrImpl* mr = find_mr(qp->ctx, rkey);
      bool ok = dlen > 0 && dlen <= kMaxMsg && (mr && (mr->access & AccessRemoteRead)) &&
                mr_covers(mr, va, dlen);
      qp->expected_psn = (psn + 1) & kPsnMask;
      if (!ok) {
        cache_and_send(qp, psn, build_ack(qp, Syndrome::NakInvalid, qp->expected_psn));
        return;
      }
      std::vector<uint8_t> b;
      FrameHdr h;
      h.op = uint8_t(Opcode::RdmaReadResp);
      h.qpn = qp->remote_qpn;
          h.seq = psn & kPsnMask;
      put_hdr(b, h);
      put_bytes(b, reinterpret_cast<const void*>(uintptr_t(va)), dlen);
      secure_finish(qp, h, b);
      append_icrc(b);
      cache_and_send(qp, psn, std::move(b));
      return;
    }
    case Opcode::AtomicCas:
    case Opcode::AtomicFaa: {
      if (!r.need(kAtomExtLen)) return;
      uint64_t va = r.u64();
      uint32_t rkey = r.u32();
      uint64_t compare = r.u64();
      uint64_t swap = r.u64();
      MrImpl* mr = find_mr(qp->ctx, rkey);
      bool ok = (mr && (mr->access & AccessRemoteAtomic)) && (va % 8 == 0) && mr_covers(mr, va, 8);
      qp->expected_psn = (psn + 1) & kPsnMask;
      if (!ok) {
        cache_and_send(qp, psn, build_ack(qp, Syndrome::NakInvalid, qp->expected_psn));
        return;
      }
      auto* slot = reinterpret_cast<uint64_t*>(uintptr_t(va));
      uint64_t orig = *slot;
      if (static_cast<Opcode>(opcode) == Opcode::AtomicCas) {
        if (orig == compare) *slot = swap;
      } else {
        *slot = orig + swap;
      }
      std::vector<uint8_t> b;
      FrameHdr h;
      h.op = uint8_t(Opcode::AtomicAck);
      h.qpn = qp->remote_qpn;
          h.seq = psn & kPsnMask;
      put_hdr(b, h);
      put_u64(b, orig);
      secure_finish(qp, h, b);
      append_icrc(b);
      cache_and_send(qp, psn, std::move(b));
      return;
    }
    default:
      return;
  }
}

}  // namespace

void handle_rx(ContextImpl* ctx, const uint8_t* data, size_t len) {
  if (len < kHdrLen + kIcrcLen) return;
  size_t body = len - kIcrcLen;
  Reader crc_r {data + body, kIcrcLen};
  if (crc_r.u32() != crc32c(data, body)) { return; }

  Reader hr {data, body};
  FrameHdr h = get_hdr(hr);
  QpImpl* qp = find_qp(ctx, h.qpn);
  if (!qp || qp->state == QpState::Reset || qp->state == QpState::Err) return;

  uint8_t plain[2048];
  const uint8_t* msg;
  size_t msg_len;
  if (ctx->sec) {
    if (body < kHdrLen + aead::kTagLen) return;
    msg_len = body - kHdrLen - aead::kTagLen;
    if (msg_len > sizeof plain) return;
    uint8_t nonce[aead::kNonceLen];
    make_nonce(h, nonce);
    uint8_t key[aead::kKeyLen];
    qp_blk_key(qp, h.conn, key);
    if (!aead::open(key, nonce, data, kHdrLen, data + kHdrLen, msg_len, data + kHdrLen + msg_len,
                    plain)) {
      return;
    }
    msg = plain;
  } else {
    msg = data + kHdrLen;
    msg_len = body - kHdrLen;
  }

  Reader r {msg, msg_len};
  switch (static_cast<Opcode>(h.op)) {
    case Opcode::Ack: {
      if (!r.need(kAextLen)) return;
      uint8_t syn = r.u8();
      uint32_t msn = r.u24();
      uint32_t credit = r.u16();
      handle_ack(qp, syn, msn, credit);
      return;
    }
    case Opcode::RdmaReadResp:
    case Opcode::AtomicAck:
      handle_response(qp, h.op, h.seq, r);
      return;
    default:
      handle_request(qp, h.op, h.seq, r);
      return;
  }
}

int handle_housekeeping(ContextImpl* ctx) {
  int events = 0;
  int64_t now = now_ms();
  for (auto& qpo : ctx->qps) {
    QpImpl* qp = qpo.get();
    if (qp->state != QpState::Rts) continue;

    if (qp->rate_bps > 0) {
      int64_t dt = now - qp->token_ts;
      qp->tokens += dt * qp->rate_bps / 1000;
      if (qp->tokens > 65536) qp->tokens = 65536;
      qp->token_ts = now;
    }
    for (;;) {
      Outstanding* next = nullptr;
      for (uint32_t i = 0; i < kSendWindow; i++) {
        Outstanding& o = qp->out[i];
        if (!o.used || o.sent) continue;
        if (!next || psn_diff(o.psn, next->psn) < 0) next = &o;
      }
      if (!next) break;
      if (next->wr.opcode == WrOpcode::Send && qp->peer_credit == 0) break;
      if (qp->rate_bps > 0) {
        if (qp->tokens < int64_t(next->pkt.size())) break;
        qp->tokens -= int64_t(next->pkt.size());
      }
      if (next->wr.opcode == WrOpcode::Send) --qp->peer_credit;
      next->sent = true;
      next->sent_at_ms = now;
      chan_xmit(qp->ctx, qp->peer, next->pkt);
      ++events;
    }

    for (uint32_t i = 0; i < kSendWindow; i++) {
      Outstanding& o = qp->out[i];
      if (!o.used || !o.sent || now - o.sent_at_ms < qp->rto_ms) continue;
      ++events;
      if (o.retries >= kMaxRetries) {
        // 放弃该帧会造成序号空洞,链路恢复后永远对不齐;
        // 无线语义:指数退避持续重传,信号恢复即补洞完成
        o.retries = 0;
        o.backoff_ms = o.backoff_ms == 0 ? 1000 : std::min<int32_t>(o.backoff_ms * 2, 4000);
        o.sent_at_ms = now + o.backoff_ms;
        ++events;
      } else {
        ++o.retries;
        o.sent_at_ms = now;
        chan_xmit(qp->ctx, qp->peer, o.pkt);
        cc_on_loss(qp);
      }
    }
  }
  return events;
}

void send_credit_update(QpImpl* qp) {
  if (!qp->peer_set) return;
  chan_xmit(qp->ctx, qp->peer, build_ack(qp, Syndrome::Ack, qp->expected_psn));
}

void cc_on_loss(QpImpl* qp) {
  if (qp->rate_max_bps == 0) return;
  qp->rate_bps /= 2;
  if (qp->rate_bps < qp->rate_max_bps / 32) qp->rate_bps = qp->rate_max_bps / 32;
}

void cc_on_progress(QpImpl* qp) {
  if (qp->rate_max_bps == 0) return;
  qp->rate_bps += qp->rate_max_bps / 32;
  if (qp->rate_bps > qp->rate_max_bps) qp->rate_bps = qp->rate_max_bps;
}

int QueuePair::post_send(const SendWr& wr) {
  QpImpl* qp = impl_;
  if (qp->state != QpState::Rts || !qp->peer_set) return -EINVAL;

  int used = 0;
  for (uint32_t i = 0; i < kSendWindow; i++) {
    if (qp->out[i].used) ++used;
  }
  if (used >= int(kSendWindow)) return -EAGAIN;

  uint32_t psn = qp->tx_psn & kPsnMask;
  Outstanding& o = qp->out[psn % kSendWindow];
  std::vector<uint8_t> pkt;
  int rc = build_request(qp, wr, psn, pkt);
  if (rc) return rc;

  o.used = true;
  o.psn = psn;
  o.wr = wr;
  o.pkt = std::move(pkt);
  o.retries = 0;
  o.backoff_ms = 0;
  qp->tx_psn = (psn + 1) & kPsnMask;

  // 限速或 SEND 无 credit 时先挂起,由 housekeeping 按序发出
  if (qp->rate_bps > 0 || (wr.opcode == WrOpcode::Send && qp->peer_credit == 0)) {
    o.sent = false;
    o.sent_at_ms = 0;
  } else {
    if (wr.opcode == WrOpcode::Send) --qp->peer_credit;
    o.sent = true;
    o.sent_at_ms = now_ms();
    chan_xmit(qp->ctx, qp->peer, o.pkt);
  }
  return 0;
}

}  // namespace rdma
