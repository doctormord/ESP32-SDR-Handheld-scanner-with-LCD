/* SPDX-License-Identifier: GPL-3.0-or-later
 * GFSK / BLE packet catcher, taken unchanged from the ESPsoup firmware
 * (pit711/espsoup-firmware, main/families/c5_c6_c61/demod_dsp.h and
 * demod_tab.h at commit 0a7a405; Copyright (C) 2026 ESPsoup contributors /
 * Peter Holzhauser, GPL-3.0-or-later). Only the GFSK part; the VFM video
 * demodulator is left out. Added here: gf_sign (Q orientation).
 *
 * Input: raw dump words, I = bits 0-9, Q = bits 10-19 (as on the ESP32 and
 * the S3). Boxcar decimation by R, quadrature discriminator, per-phase bit
 * slicer, access-address correlation, BLE de-whitening and CRC-24. Integer
 * arithmetic only. */
#pragma once
#include <stdint.h>
#include <string.h>
#ifndef DM_ATTR
#define DM_ATTR
#endif
#ifndef DM_CYC
#define DM_CYC() 0u
#endif
#define DM_INLINE static inline __attribute__((always_inline))
static const int16_t dm_lt[65] = {0,673,1336,1989,2633,3267,3892,4508,5115,5714,6305,6888,7463,8031,8591,9145,9691,10231,10763,11290,11810,12324,12832,13334,13830,14321,14806,15286,15761,16230,16695,17154,17609,18059,18505,18946,19382,19814,20242,20666,21085,21501,21913,22320,22724,23125,23521,23914,24304,24690,25072,25452,25828,26201,26570,26937,27300,27661,28018,28373,28724,29073,29419,29762,30103};

/* 10*log10(v) in 0.01 dB for v > 0 (same method as SPEC, constant table) */
static int32_t dm_db100(uint64_t v) {
    if (!v) return -10000;
    int e = 63 - __builtin_clzll(v);
    unsigned frac = e >= 6 ? (unsigned)((v >> (e - 6)) & 63) : (unsigned)((v << (6 - e)) & 63);
    return (int32_t)((e * 30103 + dm_lt[frac]) / 100);
}
static unsigned dm_popc(uint32_t x) { unsigned c = 0; while (x) { x &= x - 1; c++; } return c; }

/* ================================================================== GFSK */
#define GF_F_CRC 1u         /* BLE: CRC-24 ok */
#define GF_F_TRUNC 2u       /* cut at the block end */
#define GF_F_BLE 4u         /* payload = de-whitened BLE PDU + CRC (LSB-first bytes) */
#define GF_F_LEN 8u         /* BLE: length field above the limit (header only) */
#define GF_MAXBITS 2112u
#define GF_SBUF 560u        /* >= 31*16 + 2*16 + 3 */
typedef struct { uint32_t pos; uint16_t n, off; uint8_t flags, errs; int16_t rssi10; int32_t cfo_hz, dev_hz; } gf_pkt_t;
static int32_t gf_sign = 1;   /* -1: our Q is the other way round (TDBLESDR) */
typedef struct {
    /* configuration */
    unsigned fin, R, sps, mode, nbits, maxerr, len, okonly; int chan;
    uint32_t sync;                                   /* BLE: access address; mode 1: sync word (air order, MSB = first bit) */
    /* derived */
    uint32_t pat, mask, amask;
    uint8_t *bits;                                   /* GF_MAXBITS scratch (caller) */
    int32_t *sbuf;                                   /* GF_SBUF scratch (caller): S of the timing-candidate window */
    /* statistics */
    uint32_t blocks, hits, hit_ovf, pkts, crc_ok, trunc, rejected, pool_ovf;
    uint64_t cyc_scan, cyc_dec, samples;
} gf_t;

static uint32_t gf_rev32(uint32_t x) { uint32_t r = 0; for (int i = 0; i < 32; i++) r |= ((x >> i) & 1u) << (31 - i); return r; }
static int gf_init(gf_t *g) {
    if (g->R != 1 && g->R != 2 && g->R != 4) return 0;
    if (g->sps != 2 && g->sps != 4 && g->sps != 8 && g->sps != 16) return 0;
    if (g->mode > 2 || g->maxerr > 2) return 0;
    if (g->mode == 0) { g->nbits = 32; g->pat = gf_rev32(g->sync); g->mask = 0xffffffffu; g->amask = 0; }
    else if (g->mode == 1) {
        if (g->nbits < 8 || g->nbits > 32) return 0;
        g->mask = g->nbits == 32 ? 0xffffffffu : (1u << g->nbits) - 1; g->pat = g->sync & g->mask; g->amask = 0;
    } else {
        if (g->nbits < 8 || g->nbits > 32) return 0;
        g->mask = (1u << (g->nbits - 1)) - 1; g->pat = g->mask; g->amask = 0xffffffffu;   /* transitions: r ^ (r >> 1) */
    }
    if (g->len < 1 || g->len > 255) return 0;
    return 1;
}
DM_INLINE int32_t dm_I(uint32_t x) { return ((int32_t)(x << 22)) >> 22; }
DM_INLINE int32_t dm_Q(uint32_t x) { return ((int32_t)(x << 12)) >> 22; }

/* Hot loop: y[k] = c[k] + c[k+1] - dc (c = sum of R words; 2 sps: y[k] = c[k+1] - dc), d[k] = Im(y[k] conj y[k-1]),
 * S[k] = sum of the last SPS d's, M = slow mean of S, bit = S > M shifted into one
 * register per sample phase; a hit is a register within 2 bit errors of the pattern. */
DM_INLINE unsigned gf_scan_t(const gf_t *g, const uint32_t *restrict w, unsigned nout, int32_t dci, int32_t dcq, uint32_t *restrict hits, unsigned maxh,
                             uint32_t *ovf, const unsigned R, const unsigned SPS) {
    const uint32_t pat = g->pat, mask = g->mask, am = g->amask;
    uint32_t regs[SPS]; int32_t dpv[SPS];
    for (unsigned j = 0; j < SPS; j++) { regs[j] = 0; dpv[j] = 0; }
    int32_t pi = 0, pq = 0, S = 0, M = 0, ci = 0, cq = 0; unsigned nh = 0;
    for (unsigned j = 0; j < R; j++) { ci += dm_I(w[j]); cq += dm_Q(w[j]); }
    const uint32_t *p = w + R;
    unsigned k = 0;
    for (; k + SPS <= nout;) {
#pragma GCC unroll 16
        for (unsigned ph = 0; ph < SPS; ph++, k++) {
            int32_t ni = 0, nq = 0;
            for (unsigned j = 0; j < R; j++) { uint32_t x = p[j]; ni += dm_I(x); nq += dm_Q(x); }
            p += R;
            int32_t yi, yq;
            if (SPS >= 4) { yi = ci + ni - dci; yq = cq + nq - dcq; ci = ni; cq = nq; }   /* boxcar 2R */
            else { yi = ni - dci; yq = nq - dcq; }                                          /* 2 sps: boxcar R */
            int32_t d = (yi * pq - yq * pi) * gf_sign; pi = yi; pq = yq;          /* host orientation (device Q negated) */
            S += d - dpv[ph]; dpv[ph] = d;
            M += (S - M) >> 5;
            uint32_t r = (regs[ph] << 1) | (uint32_t)(S > M); regs[ph] = r;
            uint32_t x = ((r ^ ((r >> 1) & am)) ^ pat) & mask;
            x &= x - 1; x &= x - 1;
            if (__builtin_expect(!x, 0)) { if (nh < maxh) hits[nh++] = k; else (*ovf)++; }
        }
    }
    return nh;
}
#define GF_SCAN(R, S) static unsigned __attribute__((noinline)) DM_ATTR gf_scan_##R##_##S(const gf_t *g, const uint32_t *w, unsigned n, int32_t a, int32_t b, uint32_t *h, unsigned m, uint32_t *o) { return gf_scan_t(g, w, n, a, b, h, m, o, R, S); }
GF_SCAN(1, 2) GF_SCAN(1, 4) GF_SCAN(1, 8) GF_SCAN(1, 16)
GF_SCAN(2, 2) GF_SCAN(2, 4) GF_SCAN(2, 8) GF_SCAN(2, 16)
GF_SCAN(4, 2) GF_SCAN(4, 4) GF_SCAN(4, 8) GF_SCAN(4, 16)
static unsigned gf_scan(const gf_t *g, const uint32_t *w, unsigned nout, int32_t dci, int32_t dcq, uint32_t *h, unsigned m, uint32_t *o) {
#define GF_CASE(R, S) case R * 32 + S: return gf_scan_##R##_##S(g, w, nout, dci, dcq, h, m, o);
    switch (g->R * 32 + g->sps) {
    GF_CASE(1, 2) GF_CASE(1, 4) GF_CASE(1, 8) GF_CASE(1, 16)
    GF_CASE(2, 2) GF_CASE(2, 4) GF_CASE(2, 8) GF_CASE(2, 16)
    GF_CASE(4, 2) GF_CASE(4, 4) GF_CASE(4, 8) GF_CASE(4, 16)
    }
#undef GF_CASE
    return 0;
}

/* Slow-path iterator reproducing y/d/S of the hot loop from out-sample k0 on. */
typedef struct { const uint32_t *w; unsigned R, sps, k, nout, ph; int32_t dci, dcq, pi, pq, ci, cq, S; int32_t dpv[16]; uint64_t pow; } gf_it_t;
static void gf_it_start(gf_it_t *it, const uint32_t *w, unsigned R, unsigned sps, unsigned nout, int32_t dci, int32_t dcq, unsigned k0) {
    memset(it, 0, sizeof(*it));
    it->w = w; it->R = R; it->sps = sps; it->nout = nout; it->dci = dci; it->dcq = dcq; it->k = k0; it->ph = k0 % sps;
    for (unsigned j = 0; j < R; j++) { it->ci += dm_I(w[k0 * R + j]); it->cq += dm_Q(w[k0 * R + j]); }
    if (k0) {   /* y[k0-1] for d[k0] */
        int32_t ai = 0, aq = 0;
        for (unsigned j = 0; j < R; j++) { ai += dm_I(w[(k0 - 1) * R + j]); aq += dm_Q(w[(k0 - 1) * R + j]); }
        if (sps >= 4) { it->pi = ai + it->ci - dci; it->pq = aq + it->cq - dcq; }
        else { it->pi = it->ci - dci; it->pq = it->cq - dcq; }
    }
}
/* Advance the iterator (identical arithmetic to the hot loop), specialised per (R, SPS):
 * every stride-th out-sample S is written to out, until nreq values or the block end.
 * Returns the number of values written. */
DM_INLINE unsigned gf_adv_t(gf_it_t *restrict it, int32_t *restrict out, unsigned nreq, unsigned stride, const unsigned R, const unsigned SPS) {
    unsigned k = it->k, ph = it->ph, c = 0, o = 0; const unsigned nout = it->nout;
    int32_t ci = it->ci, cq = it->cq, pi = it->pi, pq = it->pq, S = it->S; const int32_t dci = it->dci, dcq = it->dcq;
    uint64_t pow = it->pow; const uint32_t *p = it->w + (k + 1) * R; int32_t *dpv = it->dpv;
    while (o < nreq && k < nout) {
        int32_t ni = 0, nq = 0;
        for (unsigned j = 0; j < R; j++) { uint32_t x = p[j]; ni += dm_I(x); nq += dm_Q(x); }
        p += R;
        int32_t yi, yq;
        if (SPS >= 4) { yi = ci + ni - dci; yq = cq + nq - dcq; ci = ni; cq = nq; }
        else { yi = ni - dci; yq = nq - dcq; }
        int32_t d = (yi * pq - yq * pi) * gf_sign; pi = yi; pq = yq;
        S += d - dpv[ph]; dpv[ph] = d; ph = ph + 1 == SPS ? 0 : ph + 1;
        pow += (uint32_t)(yi * yi + yq * yq);                     /* |y| <= 4096: < 2^25 */
        k++;
        if (++c == stride) { out[o++] = S; c = 0; }
    }
    it->k = k; it->ph = ph; it->ci = ci; it->cq = cq; it->pi = pi; it->pq = pq; it->S = S; it->pow = pow;
    return o;
}
#define GF_ADV(R, S) static unsigned __attribute__((noinline)) DM_ATTR gf_adv_##R##_##S(gf_it_t *it, int32_t *o, unsigned n, unsigned st) { return gf_adv_t(it, o, n, st, R, S); }
GF_ADV(1, 2) GF_ADV(1, 4) GF_ADV(1, 8) GF_ADV(1, 16)
GF_ADV(2, 2) GF_ADV(2, 4) GF_ADV(2, 8) GF_ADV(2, 16)
GF_ADV(4, 2) GF_ADV(4, 4) GF_ADV(4, 8) GF_ADV(4, 16)
static unsigned gf_adv(gf_it_t *it, int32_t *o, unsigned n, unsigned st) {
#define GF_ACASE(R, S) case R * 32 + S: return gf_adv_##R##_##S(it, o, n, st);
    switch (it->R * 32 + it->sps) {
    GF_ACASE(1, 2) GF_ACASE(1, 4) GF_ACASE(1, 8) GF_ACASE(1, 16)
    GF_ACASE(2, 2) GF_ACASE(2, 4) GF_ACASE(2, 8) GF_ACASE(2, 16)
    GF_ACASE(4, 2) GF_ACASE(4, 4) GF_ACASE(4, 8) GF_ACASE(4, 16)
    }
#undef GF_ACASE
    return 0;
}

static const uint32_t GF_CRC_INIT = 0x555555u;
/* BLE whitening (x^7 + x^4 + 1, position 0 = 1, 1..6 = channel MSB first) */
static unsigned gf_whiten_init(int ch) {
    unsigned r = 1;
    for (int k = 1; k <= 6; k++) r |= (unsigned)((ch >> (6 - k)) & 1) << k;
    return r;
}

/* Sync-word statistics of one timing candidate: class means of the expected bits, threshold,
 * bit errors and summed eye margin.  Returns 0 if the candidate does not fit. */
static int gf_sync_stats(const gf_t *g, const int32_t *Ssync, int64_t *thr_, int64_t *m1_, int64_t *m0_, uint32_t *exp_, unsigned *errs_, int64_t *eye) {
    const unsigned nb = g->nbits;
    int64_t s1 = 0, s0 = 0; unsigned n1 = 0, n0 = 0;
    int64_t mean = 0; for (unsigned j = 0; j < nb; j++) mean += Ssync[j]; mean /= (int64_t)nb;
    uint32_t got = 0, exp = 0;
    for (unsigned j = 0; j < nb; j++) {
        unsigned e = g->mode == 2 ? (Ssync[j] > mean) : (g->pat >> (nb - 1 - j)) & 1u;
        exp = (exp << 1) | e;
        if (e) { s1 += Ssync[j]; n1++; } else { s0 += Ssync[j]; n0++; }
    }
    if (!n1 || !n0) return 0;
    int64_t m1 = s1 / n1, m0 = s0 / n0;
    if (m1 <= m0) return 0;
    int64_t thr = (m1 + m0) / 2;
    for (unsigned j = 0; j < nb; j++) got = (got << 1) | (uint32_t)(Ssync[j] > thr);
    unsigned errs = g->mode == 2 ? dm_popc(~(got ^ (got >> 1)) & g->mask) : dm_popc((got ^ exp) & (nb == 32 ? 0xffffffffu : (1u << nb) - 1));
    if (errs > g->maxerr) return 0;
    int64_t mg = 0;   /* summed margin of the sync bits on their expected side */
    for (unsigned j = 0; j < nb; j++) { int64_t d = Ssync[j] - thr; mg += ((exp >> (nb - 1 - j)) & 1u) ? d : -d; }
    *eye = mg - (int64_t)errs * (m1 - m0);
    *thr_ = thr; *m1_ = m1; *m0_ = m0; *exp_ = exp; *errs_ = errs;
    return 1;
}

/* Decode the packet whose sync word ends at out-sample k (S integrates its last
 * bit there).  Returns the out-sample of the last decoded bit (0 = rejected). */
static unsigned gf_decode(gf_t *g, const uint32_t *w, unsigned nout, int32_t dci, int32_t dcq, unsigned k, gf_pkt_t *pk, uint8_t *data, unsigned room, int64_t *eye) {
    const unsigned sps = g->sps, nb = g->nbits;
    if (k < (nb - 1) * sps) return 0;
    unsigned kfirst = k - (nb - 1) * sps;                       /* S of the first sync bit */
    unsigned ks = kfirst + 1 > sps ? kfirst + 1 - sps : 0;
    gf_it_t it; gf_it_start(&it, w, g->R, sps, nout, dci, dcq, ks);
    int32_t Ssync[32];
    if (gf_adv(&it, Ssync, 1, kfirst - ks + 1) != 1 || gf_adv(&it, Ssync + 1, nb - 1, sps) != nb - 1) return 0;
    int64_t thr, m1, m0; uint32_t exp; unsigned errs; int64_t ey;
    if (!gf_sync_stats(g, Ssync, &thr, &m1, &m0, &exp, &errs, &ey)) return 0;
    if (eye) { *eye = ey; return 1; }
    /* payload bits (S at the bit ends k + j*sps, in chunks) */
    unsigned want = g->mode == 0 ? 16 : g->len * 8, nbits = 0, flags = 0, kl = k;
    unsigned wr = g->mode == 0 ? gf_whiten_init(g->chan) : 0;
    int32_t chunk[64];
    while (nbits < want) {
        unsigned req = want - nbits; if (req > 64) req = 64;
        if (g->mode == 0 && nbits < 16 && req > 16 - nbits) req = 16 - nbits;
        unsigned got = gf_adv(&it, chunk, req, sps);
        for (unsigned j = 0; j < got; j++) {
            unsigned b = chunk[j] > thr;
#ifdef GF_DEBUG
            GF_DEBUG(nbits, chunk[j], thr, m1, m0, k);
#endif
            if (g->mode == 0) { unsigned o = (wr >> 6) & 1u; wr = ((wr << 1) & 0x7fu) | o; if (o) wr ^= 0x10u; b ^= o; }
            g->bits[nbits++] = (uint8_t)b; kl = k + nbits * sps;
        }
        if (got < req) { flags |= GF_F_TRUNC; break; }
        if (g->mode == 0 && nbits == 16) {
            unsigned L = 0; for (unsigned j = 0; j < 8; j++) L |= (unsigned)g->bits[8 + j] << j;
            if (L > g->len) flags |= GF_F_LEN; else want = 16 + 8 * L + 24;
        }
    }
    unsigned nbytes = (nbits + 7) / 8;
    if (nbytes > room) return 0xffffffffu;                    /* pool full */
    memset(data, 0, nbytes);
    if (g->mode == 0) {
        flags |= GF_F_BLE;
        for (unsigned j = 0; j < nbits; j++) data[j >> 3] |= (uint8_t)(g->bits[j] << (j & 7));
        if (!(flags & (GF_F_TRUNC | GF_F_LEN)) && nbits >= 40) {
            uint32_t c = GF_CRC_INIT; unsigned nb_pdu = nbits - 24, ok = 1;
            for (unsigned j = 0; j < nb_pdu; j++) { unsigned fb = ((c >> 23) & 1u) ^ g->bits[j]; c = (c << 1) & 0xffffffu; if (fb) c ^= 0x65bu; }
            for (unsigned j = 0; j < 24; j++) if (((c >> (23 - j)) & 1u) != g->bits[nb_pdu + j]) { ok = 0; break; }
            if (ok) flags |= GF_F_CRC;
        }
    } else for (unsigned j = 0; j < nbits; j++) data[j >> 3] |= (uint8_t)(g->bits[j] << (7 - (j & 7)));
    /* power over sync + payload, carrier offset and deviation (small-angle, |y|^2 normalised) */
    uint64_t pw = it.pow, cnt = it.k - ks;
    uint64_t P = cnt ? pw / cnt : 0; if (!P) P = 1;
    uint32_t fout = g->fin / g->R; unsigned L = g->sps >= 4 ? 2 * g->R : g->R; uint64_t fs2 = (uint64_t)(L * 512) * (L * 512);
    int64_t q = thr * (int64_t)fout / ((int64_t)sps * (int64_t)P);
    int64_t qd = (m1 - m0) / 2 * (int64_t)fout / ((int64_t)sps * (int64_t)P);
    pk->cfo_hz = (int32_t)(q * 159155 / 1000000);             /* host orientation (d is computed with Q negated) */
    pk->dev_hz = (int32_t)(qd * 159155 / 1000000);
    pk->rssi10 = (int16_t)((dm_db100(P) - dm_db100(fs2)) / 10);
    pk->pos = k + 1 >= nb * sps ? (k + 1 - nb * sps) * g->R : 0;
    pk->n = (uint16_t)nbytes; pk->flags = (uint8_t)flags; pk->errs = (uint8_t)errs;
    return kl;
}

/* One block: returns the number of packet records; payload bytes go to pool
 * (pk[i].off / pk[i].n). */
static unsigned DM_ATTR gf_block(gf_t *g, const uint32_t *w, unsigned n, uint32_t *hits, unsigned maxh, gf_pkt_t *pk, unsigned maxpk, uint8_t *pool, unsigned poolsz) {
    uint32_t c0 = DM_CYC();
    g->blocks++; g->samples += n;
    unsigned nout = n / g->R - 1;
    /* DC of this block (every 8th sample), in y units (2R samples) */
    int32_t si = 0, sq = 0; unsigned cnt = 0;
    for (unsigned j = 0; j < n; j += 8, cnt++) { si += dm_I(w[j]); sq += dm_Q(w[j]); }
    const int32_t L = (int32_t)(g->sps >= 4 ? 2 * g->R : g->R);          /* boxcar length (input samples per y) */
    int32_t dci = (int32_t)((int64_t)si * L / (int32_t)cnt), dcq = (int32_t)((int64_t)sq * L / (int32_t)cnt);
    uint32_t ovf = 0;
    unsigned nh = gf_scan(g, w, nout, dci, dcq, hits, maxh, &ovf);
    g->hit_ovf += ovf; g->hits += nh;
    uint32_t c1 = DM_CYC(); g->cyc_scan += c1 - c0;
    unsigned np = 0, used = 0, busy_until = 0;
    for (unsigned a = 0; a < nh;) {
        unsigned b = a + 1;
        while (b < nh && hits[b] - hits[a] < g->sps) b++;          /* one group = one alignment */
        /* timing: best sync-word eye opening among the group and its neighbours */
        unsigned ka = hits[a] ? hits[a] - 1 : 0, kb = hits[b - 1] + 1, k = hits[a];
        if (kb < hits[a] + g->sps) kb = hits[a] + g->sps;          /* at least one bit period of alignments */
        if (kb >= nout) kb = nout - 1;
        a = b;
        if (k <= busy_until && busy_until) continue;
        int64_t best = INT64_MIN;
        /* S of all out-samples the candidates can need, computed once (identical arithmetic) */
        const unsigned span = (g->nbits - 1) * g->sps, kmax = ka + 2 * g->sps + 2 < nout ? ka + 2 * g->sps + 2 : nout - 1;
        unsigned kA = ka >= span ? ka - span : 0, nS = 0;
        {
            gf_it_t it; unsigned ks = kA + 1 > g->sps ? kA + 1 - g->sps : 0;
            gf_it_start(&it, w, g->R, g->sps, nout, dci, dcq, ks);
            unsigned want = kmax - kA + 1; if (want > GF_SBUF) want = GF_SBUF;
            if (gf_adv(&it, g->sbuf, 1, kA - ks + 1) == 1) nS = 1 + gf_adv(&it, g->sbuf + 1, want - 1, 1);
        }
        for (unsigned c = ka; c <= kb; c++) {
            int64_t e; int okc = 0;
            if (c >= span && c - span >= kA && c - kA < nS) {
                int32_t Ss[32]; unsigned b0 = c - span - kA;
                for (unsigned j = 0; j < g->nbits; j++) Ss[j] = g->sbuf[b0 + j * g->sps];
                int64_t t1, t2, t3; uint32_t t4; unsigned t5;
                okc = gf_sync_stats(g, Ss, &t1, &t2, &t3, &t4, &t5, &e);
            }
#ifdef GF_DEBUG_EYE
            GF_DEBUG_EYE(c, okc, e);
#endif
            if (okc && best == INT64_MIN && kb < c + g->sps && c + g->sps < nout) kb = c + g->sps;   /* a full bit after the first fit */
            if (okc && e > best) { best = e; k = c; }
        }
        if (best == INT64_MIN) { g->rejected++; continue; }
        if (np >= maxpk) { g->pool_ovf++; break; }
        unsigned kl = gf_decode(g, w, nout, dci, dcq, k, &pk[np], pool + used, poolsz - used, NULL);
        if (kl == 0xffffffffu) { g->pool_ovf++; break; }
        if (!kl) { g->rejected++; continue; }
        busy_until = kl;
        g->pkts++;
        if (pk[np].flags & GF_F_TRUNC) g->trunc++;
        if (pk[np].flags & GF_F_CRC) g->crc_ok++;
        if (g->okonly && g->mode == 0 && !(pk[np].flags & GF_F_CRC)) continue;
        pk[np].off = (uint16_t)used; used += pk[np].n; np++;
    }
    g->cyc_dec += DM_CYC() - c1;
    return np;
}
