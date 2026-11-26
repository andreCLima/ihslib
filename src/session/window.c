/*
 *  _____  _   _  _____  _  _  _     
 * |_   _|| | | |/  ___|| |(_)| |     Steam    
 *   | |  | |_| |\ `--. | | _ | |__     In-Home
 *   | |  |  _  | `--. \| || || '_ \      Streaming
 *  _| |_ | | | |/\__/ /| || || |_) |       Library
 *  \___/ \_| |_/\____/ |_||_||_.__/
 *
 * Copyright (c) 2022 Mariotaku <https://github.com/mariotaku>.
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include "window.h"

#include <memory.h>
#include <stdlib.h>
#include <assert.h>

struct IHS_SessionPacketsWindow {
    IHS_SessionWindowItem *data;
    uint16_t capacity;
    /*
     * [+][+][+][-]
     *       ^ head.pos = 2
     */
    struct {
        int pos;
    } head;
    /*
     * [+][+][+][-]
     *         ^ tail.pos = 2
     */
    struct {
        int pos;
        uint16_t id;
    } tail;
};

static inline bool FrameItemIsHead(const IHS_SessionWindowItem *item);

static inline bool FrameItemIsUsed(const IHS_SessionWindowItem *item);

static inline void FrameItemUsePacket(IHS_SessionWindowItem *item, IHS_SessionPacket *packet);

static inline void FrameItemRecycle(IHS_SessionWindowItem *item);

IHS_SessionPacketsWindow *IHS_SessionPacketsWindowCreate(uint16_t capacity) {
    IHS_SessionPacketsWindow *window = calloc(1, sizeof(IHS_SessionPacketsWindow));
    window->capacity = capacity;
    window->data = calloc(capacity, sizeof(IHS_SessionWindowItem));
    window->head.pos = 0;
    window->tail.pos = -1;
    return window;
}

void IHS_SessionPacketsWindowDestroy(IHS_SessionPacketsWindow *window) {
    for (int i = 0, j = window->capacity; i < j; i++) {
        if (!FrameItemIsUsed(&window->data[i])) {
            continue;
        }
        FrameItemRecycle(&window->data[i]);
    }
    free(window->data);
    free(window);
}

bool IHS_SessionPacketsWindowAdd(IHS_SessionPacketsWindow *window, IHS_SessionPacket *packet) {
    /* Calculate distance of 2 items */
    int tailOffset = window->tail.pos < 0 ? 1 : (int16_t) (packet->header.packetId - window->tail.id);
    /* The window holds tail.id - size + 1 to tail.id; anything at or before that start was polled
     * already. The old test let through a resend of the newest packet once everything had been
     * polled (offset 0, size 0), and of the packet just before head (offset -size). Steam resends
     * most control packets once, so this happened all the time: the copy went into a free slot
     * outside the window, and 128 ids later the real packet landed on that slot, was dropped as
     * a duplicate, and the window stopped short of it. The ACK then stayed one behind while Steam
     * resent that packet hundreds of times a second, for seconds or minutes, until a newer packet
     * moved the tail on and the stale copy was delivered in its place. Controller freezes on
     * 26/11 lined up with these gaps. */
    if (tailOffset <= 0 && -tailOffset >= IHS_SessionPacketsWindowSize(window)) {
        return true;
    }
    /* Further ahead than the window holds: the stream went on without us (on Wi-Fi, a gap of a few
     * seconds), so refuse it as an overflow and let the channel start over from it. Ignoring it,
     * as before, ignored every packet after it as well, and the channel stayed dead. */
    if (tailOffset > window->capacity) {
        return false;
    }
    /* Large offset means overflow, abort processing and hangup. One slot always stays free: in a
     * full window whose head isn't at slot 0, tail.pos + 1 == head.pos, which Size reads as empty,
     * and Poll and Discard would never touch it again. */
    if (tailOffset >= (int) IHS_SessionPacketsWindowAvailable(window)) {
        return false;
    }
    const int writePos = (window->tail.pos + tailOffset + window->capacity) % window->capacity;
    IHS_SessionWindowItem *tailPkt = &window->data[writePos];
    if (tailOffset <= 0) {
        /* Within the window, a used slot holds this packet already: a resend. */
        if (FrameItemIsUsed(tailPkt)) {
            return true;
        }
    } else {
        /* Past the tail every slot must be free, the skipped ones included, or a leftover would
         * later be read as the packet that belongs there. */
        for (int i = 1; i <= tailOffset; i++) {
            IHS_SessionWindowItem *item = &window->data[(window->tail.pos + i + window->capacity) % window->capacity];
            if (FrameItemIsUsed(item)) {
                FrameItemRecycle(item);
            }
        }
    }
    FrameItemUsePacket(tailPkt, packet);

    /* Only do incremental update */
    if (tailOffset > 0) {
        window->tail.pos = writePos;
        window->tail.id = packet->header.packetId;
    }
    return true;
}

bool IHS_SessionPacketsWindowPoll(IHS_SessionPacketsWindow *window, IHS_SessionFrame *frame) {
    uint16_t size = IHS_SessionPacketsWindowSize(window);
    if (size == 0) {
        return false;
    }
    assert(window->head.pos >= 0);
    IHS_SessionWindowItem *head = &window->data[window->head.pos % window->capacity];

    /* Must start from packet head */
    if (!FrameItemIsHead(head)) {
        return false;
    }

    /* Must have size enough for all fragments */
    int packetsCount = 1 + head->header.fragmentId;
    if (size < packetsCount) {
        return false;
    }

    size_t frameBodyLen = 0;
    for (int i = window->head.pos, j = window->head.pos + packetsCount; i < j; i++) {
        /* The array is sparse, must collect all fragments */
        const IHS_SessionWindowItem *item = &window->data[i % window->capacity];
        if (!FrameItemIsUsed(item)) {
            return false;
        }
        frameBodyLen += item->body.size;
    }
    frame->header = head->header;
    IHS_BufferEnsureMaxSize(&frame->body, frameBodyLen);

    for (int i = window->head.pos, j = window->head.pos + packetsCount; i < j; i++) {
        IHS_SessionWindowItem *item = &window->data[i % window->capacity];
        IHS_BufferAppend(&frame->body, &item->body);

        /* This item is used, recycle it */
        FrameItemRecycle(item);
    }
    assert(frame->body.size == frameBodyLen);

    window->head.pos = window->head.pos + packetsCount;
    if (window->head.pos > window->capacity) {
        window->head.pos = window->head.pos % window->capacity;
    }
    return true;
}

uint16_t IHS_SessionPacketsWindowDiscard(IHS_SessionPacketsWindow *window, uint32_t diff) {
    uint16_t size = IHS_SessionPacketsWindowSize(window);
    if (!size) return 0;
    IHS_SessionWindowItem *tailPkt = &window->data[window->tail.pos];
    if (tailPkt->header.sendTimestamp < diff) return 0;
    /* Should discard all frames older than discardBefore */
    uint32_t discardBefore = tailPkt->header.sendTimestamp - diff;
    /* Find reset valid index after discardBefore */
    int firstValid = -1;
    for (int i = window->head.pos, j = window->head.pos + size; i < j; i++) {
        const IHS_SessionWindowItem *item = &window->data[i % window->capacity];
        if (!FrameItemIsUsed(item) || !FrameItemIsHead(item)) {
            continue;
        }
        if (item->header.sendTimestamp >= discardBefore) {
            firstValid = i;
            break;
        }
    }
    if (firstValid < 0) return 0;
    uint16_t discarded = 0;
    for (int i = window->head.pos; i < firstValid; i++) {
        IHS_SessionWindowItem *item = &window->data[i % window->capacity];
        discarded++;
        if (!FrameItemIsUsed(item)) {
            continue;
        }
        /* This packet is used, recycle it */
        FrameItemRecycle(item);
    }
    window->head.pos = firstValid;
    if (window->head.pos > window->capacity) {
        window->head.pos = window->head.pos % window->capacity;
    }
    return discarded;
}

bool IHS_SessionPacketsWindowContiguousId(const IHS_SessionPacketsWindow *window, uint16_t *id) {
    if (window->tail.pos < 0) {
        return false;
    }
    /* Everything before head has been polled; the run of used slots from head on decides. */
    uint16_t size = IHS_SessionPacketsWindowSize(window);
    uint16_t headId = (uint16_t) (window->tail.id - size + 1);
    for (uint16_t i = 0; i < size; i++) {
        if (!FrameItemIsUsed(&window->data[(window->head.pos + i) % window->capacity])) {
            *id = (uint16_t) (headId + i - 1);
            return true;
        }
    }
    *id = window->tail.id;
    return true;
}

void IHS_SessionPacketsWindowReset(IHS_SessionPacketsWindow *window) {
    for (int i = 0; i < window->capacity; i++) {
        if (FrameItemIsUsed(&window->data[i])) {
            FrameItemRecycle(&window->data[i]);
        }
    }
    window->head.pos = 0;
    window->tail.pos = -1;
    window->tail.id = 0;
}

/* True if the frame starting at pos (unwrapped) has all its packets within the next `size` slots. */
static bool FrameCompleteAt(const IHS_SessionPacketsWindow *window, int pos, uint16_t size) {
    const IHS_SessionWindowItem *head = &window->data[pos % window->capacity];
    if (!FrameItemIsUsed(head) || !FrameItemIsHead(head)) {
        return false;
    }
    int packetsCount = 1 + head->header.fragmentId;
    if (packetsCount > size) {
        return false;
    }
    for (int i = 1; i < packetsCount; i++) {
        if (!FrameItemIsUsed(&window->data[(pos + i) % window->capacity])) {
            return false;
        }
    }
    return true;
}

uint16_t IHS_SessionPacketsWindowSkipLost(IHS_SessionPacketsWindow *window) {
    uint16_t size = IHS_SessionPacketsWindowSize(window);
    if (size == 0 || FrameCompleteAt(window, window->head.pos, size)) {
        return 0;
    }
    for (uint16_t offset = 1; offset < size; offset++) {
        if (!FrameCompleteAt(window, window->head.pos + offset, (uint16_t) (size - offset))) {
            continue;
        }
        for (uint16_t i = 0; i < offset; i++) {
            IHS_SessionWindowItem *item = &window->data[(window->head.pos + i) % window->capacity];
            if (FrameItemIsUsed(item)) {
                FrameItemRecycle(item);
            }
        }
        window->head.pos = window->head.pos + offset;
        if (window->head.pos > window->capacity) {
            window->head.pos = window->head.pos % window->capacity;
        }
        return offset;
    }
    return 0;
}

void IHS_SessionPacketsWindowReleaseFrame(IHS_SessionFrame *frame) {
    IHS_SessionFrameClear(frame, false);
}

uint16_t IHS_SessionPacketsWindowAvailable(const IHS_SessionPacketsWindow *window) {
    return window->capacity - IHS_SessionPacketsWindowSize(window);
}

uint16_t IHS_SessionPacketsWindowSize(const IHS_SessionPacketsWindow *window) {
    if (window->tail.pos < 0) {
        return 0;
    } else if (window->tail.pos + 1 >= window->head.pos) {
        /*
         * Head is before tail.pos, just calculate their distance
         *
         * |[+][+][+][+][+][-][-][-]| capacity = 8
         *  ^ head.pos = 0    ^ tail.pos = 4
         * distance = tail.pos + 1 - head.pos = 5
         *
         * |[-][-][-][-][-][-][-][-]| capacity = 8
         *                         ^ head.pos = 8, tail.pos = 7
         * distance = tail.pos + 1 - head.pos = 0
         */
        return window->tail.pos + 1 - window->head.pos;
    } else {
        /* Head is after tail.pos
         *
         * |[+][+][-][-][-][-][+][+]| capacity = 8
         *       ^ tail.pos = 1   ^ head.pos = 6
         * distance = capacity - head.pos + tail.pos + 1 = 4
         *
         * |[+][+][-][-][-][-][-][-]| capacity = 8
         *       ^ tail.pos = 1        ^ head.pos = 8
         * distance = capacity - head.pos + tail.pos + 1 = 2
         */
        return window->capacity - window->head.pos + window->tail.pos + 1;
    }
}

static inline bool FrameItemIsHead(const IHS_SessionWindowItem *item) {
    return item->header.type == IHS_SessionPacketTypeReliable || item->header.type == IHS_SessionPacketTypeUnreliable;
}

static inline bool FrameItemIsUsed(const IHS_SessionWindowItem *item) {
    return !IHS_BufferIsNull(&item->body);
}

static inline void FrameItemUsePacket(IHS_SessionWindowItem *item, IHS_SessionPacket *packet) {
    item->header = packet->header;
    IHS_BufferTransferOwnership(&packet->body, &item->body);
}

static inline void FrameItemRecycle(IHS_SessionWindowItem *item) {
    IHS_BufferClear(&item->body, true);
    memset(item, 0, sizeof(IHS_SessionWindowItem));
}