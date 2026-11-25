/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "fec.h"

#include <stdlib.h>
#include <string.h>

#include "endianness.h"
#include "ihs_buffer.h"

#define FEC_HEADER_SIZE 8
/* Messages in flight. A shard more than this many messages late is dropped. */
#define FEC_GROUPS 32
/* Shard index is an int16 fragment id, and c_i = 1 / (i + 1) needs i + 1 <= 255. */
#define FEC_MAX_SHARDS 255
/* Datagrams are at most about 1500 bytes. */
#define FEC_MAX_SHARD_SIZE 2048
/* Size of the matrix to invert when rebuilding. Steam used 1 or 2. */
#define FEC_MAX_PARITY 16

typedef struct FECGroup {
    bool active;
    bool done;
    uint16_t packetId;
    IHS_SessionPacketHeader header;
    uint32_t length;
    uint16_t shardSize;
    uint16_t dataShards;
    uint16_t parityShards;
    uint16_t received;
    uint16_t dataReceived;
    uint16_t parityReceived;
    uint8_t present[(FEC_MAX_SHARDS + 7) / 8];
    /* All shards in index order, data then parity; freed once the message is out. */
    uint8_t *shards;
} FECGroup;

struct IHS_SessionFEC {
    FECGroup groups[FEC_GROUPS];
    bool started;
    uint16_t newest;
    uint32_t advanced;
    uint8_t exp[512];
    uint8_t log[256];
    IHS_SessionFECStats stats;
};

static void GroupRetire(IHS_SessionFEC *fec, FECGroup *group);

static bool GroupRebuild(const IHS_SessionFEC *fec, FECGroup *group);

static inline bool GroupHas(const FECGroup *group, int index) {
    return group->present[index / 8] & (1u << (index % 8));
}

IHS_SessionFEC *IHS_SessionFECCreate(void) {
    IHS_SessionFEC *fec = calloc(1, sizeof(IHS_SessionFEC));
    unsigned x = 1;
    for (int i = 0; i < 255; i++) {
        fec->exp[i] = (uint8_t) x;
        fec->log[x] = (uint8_t) i;
        x <<= 1;
        if (x & 0x100) {
            x ^= 0x11d;
        }
    }
    for (int i = 255; i < 512; i++) {
        fec->exp[i] = fec->exp[i - 255];
    }
    return fec;
}

void IHS_SessionFECDestroy(IHS_SessionFEC *fec) {
    for (int i = 0; i < FEC_GROUPS; i++) {
        free(fec->groups[i].shards);
    }
    free(fec);
}

bool IHS_SessionFECAdd(IHS_SessionFEC *fec, const IHS_SessionPacket *shard, IHS_SessionPacket *message) {
    const uint8_t *body = IHS_BufferPointer(&shard->body);
    size_t bodySize = shard->body.size;
    if (bodySize <= FEC_HEADER_SIZE) {
        return false;
    }
    uint32_t length;
    uint16_t shardSize, parityShards;
    IHS_ReadUInt32LE(body, &length);
    IHS_ReadUInt16LE(body + 4, &shardSize);
    IHS_ReadUInt16LE(body + 6, &parityShards);
    if (length == 0 || shardSize == 0 || shardSize > FEC_MAX_SHARD_SIZE || parityShards == 0 ||
        parityShards > FEC_MAX_PARITY) {
        return false;
    }
    uint32_t dataShards = (length + shardSize - 1) / shardSize;
    int index = shard->header.fragmentId;
    if (dataShards + parityShards > FEC_MAX_SHARDS || index < 0 || (uint32_t) index >= dataShards + parityShards) {
        return false;
    }
    fec->stats.shards++;

    uint16_t id = shard->header.packetId;
    if (!fec->started) {
        fec->started = true;
        fec->newest = (uint16_t) (id - 1);
    }
    int16_t ahead = (int16_t) (id - fec->newest);
    if (ahead > FEC_GROUPS) {
        // A jump past the whole window: the messages skipped over it are gone without a trace.
        if (fec->advanced >= FEC_GROUPS) {
            fec->stats.shardsLost += (uint32_t) (ahead - FEC_GROUPS);
            fec->stats.unrecoverable += (uint32_t) (ahead - FEC_GROUPS);
        }
        fec->advanced += (uint32_t) (ahead - FEC_GROUPS);
        fec->newest = (uint16_t) (id - FEC_GROUPS);
        ahead = FEC_GROUPS;
    }
    if (ahead > 0) {
        // Each id that becomes current takes the slot of the id FEC_GROUPS before it.
        for (int16_t step = 0; step < ahead; step++) {
            uint16_t current = (uint16_t) (fec->newest + 1 + step);
            FECGroup *slot = &fec->groups[current % FEC_GROUPS];
            if (slot->active) {
                GroupRetire(fec, slot);
            } else if (fec->advanced >= FEC_GROUPS) {
                // Not a single shard of that message arrived.
                fec->stats.shardsLost++;
                fec->stats.unrecoverable++;
            }
            fec->advanced++;
        }
        fec->newest = id;
    } else if (ahead <= -FEC_GROUPS) {
        return false;
    }

    FECGroup *group = &fec->groups[id % FEC_GROUPS];
    if (!group->active) {
        memset(group, 0, sizeof(FECGroup));
        group->active = true;
        group->packetId = id;
        group->header = shard->header;
        group->length = length;
        group->shardSize = shardSize;
        group->dataShards = (uint16_t) dataShards;
        group->parityShards = parityShards;
        group->shards = calloc(dataShards + parityShards, shardSize);
    } else if (group->packetId != id || group->length != length || group->shardSize != shardSize ||
               group->parityShards != parityShards) {
        return false;
    }
    if (GroupHas(group, index)) {
        return false;
    }
    group->present[index / 8] |= (uint8_t) (1u << (index % 8));
    group->received++;
    if (group->done) {
        return false;
    }
    size_t copy = bodySize - FEC_HEADER_SIZE < shardSize ? bodySize - FEC_HEADER_SIZE : shardSize;
    memcpy(group->shards + (size_t) index * shardSize, body + FEC_HEADER_SIZE, copy);
    if (index < group->dataShards) {
        group->dataReceived++;
    } else {
        group->parityReceived++;
    }

    if (group->dataReceived < group->dataShards) {
        if (group->dataReceived + group->parityReceived < group->dataShards || !GroupRebuild(fec, group)) {
            return false;
        }
        fec->stats.recovered++;
    }
    group->done = true;
    message->header = group->header;
    message->header.type = IHS_SessionPacketTypeUnreliable;
    message->header.fragmentId = 0;
    message->crc = 0;
    IHS_BufferInit(&message->body, group->length, group->length);
    IHS_BufferAppendMem(&message->body, group->shards, group->length);
    free(group->shards);
    group->shards = NULL;
    return true;
}

void IHS_SessionFECGetStats(const IHS_SessionFEC *fec, IHS_SessionFECStats *stats) {
    *stats = fec->stats;
}

static void GroupRetire(IHS_SessionFEC *fec, FECGroup *group) {
    fec->stats.shardsLost += (uint32_t) (group->dataShards + group->parityShards - group->received);
    if (!group->done) {
        fec->stats.unrecoverable++;
    }
    free(group->shards);
    memset(group, 0, sizeof(FECGroup));
}

static inline uint8_t GfMul(const IHS_SessionFEC *fec, uint8_t a, uint8_t b) {
    return a != 0 && b != 0 ? fec->exp[fec->log[a] + fec->log[b]] : 0;
}

static inline uint8_t GfInv(const IHS_SessionFEC *fec, uint8_t a) {
    return fec->exp[255 - fec->log[a]];
}

/* Weight of data shard i in parity row r of m: 1 / (r + m + i), never 1 / 0 since r < m. */
static inline uint8_t Coefficient(const IHS_SessionFEC *fec, int m, int r, int i) {
    return GfInv(fec, (uint8_t) (r ^ (m + i)));
}

/* target += c * source */
static void AddScaled(const IHS_SessionFEC *fec, uint8_t *target, const uint8_t *source, uint8_t c, size_t size) {
    if (c == 0) {
        return;
    }
    int logC = fec->log[c];
    for (size_t j = 0; j < size; j++) {
        if (source[j] != 0) {
            target[j] ^= fec->exp[fec->log[source[j]] + logC];
        }
    }
}

/*
 * Rebuilds the e missing data shards from e received parity rows. For each of those rows, the
 * parity minus the known shards' part is what the missing shards add up to; inverting the e x e
 * Cauchy submatrix (missing shards by rows) turns that into the shards.
 */
static bool GroupRebuild(const IHS_SessionFEC *fec, FECGroup *group) {
    int k = group->dataShards, m = group->parityShards;
    size_t size = group->shardSize;
    int missing[FEC_MAX_PARITY], rows[FEC_MAX_PARITY];
    int e = 0, p = 0;
    for (int i = 0; i < k; i++) {
        if (!GroupHas(group, i)) {
            if (e == FEC_MAX_PARITY) {
                return false;
            }
            missing[e++] = i;
        }
    }
    for (int r = 0; r < m && p < e; r++) {
        if (GroupHas(group, k + r)) {
            rows[p++] = r;
        }
    }
    if (e == 0 || p < e) {
        return false;
    }

    // Gauss-Jordan on [M | I], M[a][b] = weight of missing shard b in row a.
    uint8_t mat[FEC_MAX_PARITY][FEC_MAX_PARITY], inv[FEC_MAX_PARITY][FEC_MAX_PARITY];
    for (int a = 0; a < e; a++) {
        for (int b = 0; b < e; b++) {
            mat[a][b] = Coefficient(fec, m, rows[a], missing[b]);
            inv[a][b] = a == b;
        }
    }
    for (int col = 0; col < e; col++) {
        int pivot = col;
        while (pivot < e && mat[pivot][col] == 0) {
            pivot++;
        }
        if (pivot == e) {
            return false;
        }
        for (int b = 0; b < e; b++) {
            uint8_t t = mat[col][b];
            mat[col][b] = mat[pivot][b];
            mat[pivot][b] = t;
            t = inv[col][b];
            inv[col][b] = inv[pivot][b];
            inv[pivot][b] = t;
        }
        uint8_t scale = GfInv(fec, mat[col][col]);
        for (int b = 0; b < e; b++) {
            mat[col][b] = GfMul(fec, mat[col][b], scale);
            inv[col][b] = GfMul(fec, inv[col][b], scale);
        }
        for (int a = 0; a < e; a++) {
            uint8_t factor = mat[a][col];
            if (a == col || factor == 0) {
                continue;
            }
            for (int b = 0; b < e; b++) {
                mat[a][b] ^= GfMul(fec, factor, mat[col][b]);
                inv[a][b] ^= GfMul(fec, factor, inv[col][b]);
            }
        }
    }

    uint8_t *rhs = malloc((size_t) e * size);
    if (rhs == NULL) {
        return false;
    }
    for (int a = 0; a < e; a++) {
        uint8_t *row = rhs + (size_t) a * size;
        memcpy(row, group->shards + (size_t) (k + rows[a]) * size, size);
        for (int i = 0; i < k; i++) {
            if (GroupHas(group, i)) {
                AddScaled(fec, row, group->shards + (size_t) i * size, Coefficient(fec, m, rows[a], i), size);
            }
        }
    }
    for (int b = 0; b < e; b++) {
        uint8_t *target = group->shards + (size_t) missing[b] * size;
        memset(target, 0, size);
        for (int a = 0; a < e; a++) {
            AddScaled(fec, target, rhs + (size_t) a * size, inv[b][a], size);
        }
    }
    free(rhs);
    return true;
}
