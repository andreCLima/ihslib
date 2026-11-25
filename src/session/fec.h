/* SPDX-License-Identifier: LGPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "packet.h"

/*
 * Forward error correction for unreliable data. Steam sends it when the client sets
 * CStreamingClientConfig.enable_unreliable_fec (field 25).
 *
 * Each data message then goes out under one packet id, as packets of type UnreliableFEC: k data
 * shards, then m parity shards, the fragment id being the shard index. Every shard body starts
 * with an 8-byte header, little endian: u32 message length, u16 shard size, u16 m. The data
 * shards hold the message cut into pieces of the shard size, the last one padded. Parity shard
 * r is the sum over i of d_i / (r + m + i) in GF(2^8), polynomial 0x11d, where + is XOR: a
 * Cauchy matrix with x_r = r and y_i = m + i. So a message can lose any m shards and still be
 * rebuilt.
 *
 * Worked out from captured traffic: video only (audio stays plain), m = 1 for most messages
 * and 2 for large ones (above about 30 KB). All 6074 parity rows in two captures matched.
 */
typedef struct IHS_SessionFEC IHS_SessionFEC;

typedef struct IHS_SessionFECStats {
    /** Shards received. */
    uint32_t shards;
    /** Shards that never arrived, counted when their message leaves the reorder window. */
    uint32_t shardsLost;
    /** Messages rebuilt from parity. */
    uint32_t recovered;
    /** Messages that lost more shards than the parity can make up for. */
    uint32_t unrecoverable;
} IHS_SessionFECStats;

IHS_SessionFEC *IHS_SessionFECCreate(void);

void IHS_SessionFECDestroy(IHS_SessionFEC *fec);

/**
 * Takes one UnreliableFEC packet. Returns true when it completes its message, or makes it
 * rebuildable, and fills message with a plain Unreliable packet (fragment id 0, header of the
 * message's first shard) holding the whole message. The caller owns message->body.
 * Late, duplicate and malformed shards return false.
 */
bool IHS_SessionFECAdd(IHS_SessionFEC *fec, const IHS_SessionPacket *shard, IHS_SessionPacket *message);

/** Not thread safe: call it from the thread that adds shards. */
void IHS_SessionFECGetStats(const IHS_SessionFEC *fec, IHS_SessionFECStats *stats);
