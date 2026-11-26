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

#include "retransmission.h"
#include "session_pri.h"

/*
 * Steam delivers reliable control messages in order, so one we stop resending holds back every
 * later one: controller reports included, which left the game deaf to the pad (sometimes with a
 * button held) while video and audio went on. 20 attempts 10 ms apart gave up after 200 ms, less
 * than a Wi-Fi stall lasts. Back off from 10 ms to 250 ms and keep trying for about 12 s, as
 * reliable transports do (ENet, which Moonlight uses for input, backs off and never drops one).
 */

uint64_t IHS_RetransmissionInterval(uint8_t retransmitCount) {
    uint64_t interval = IHS_RETRANSMISSION_INTERVAL_MIN;
    for (uint8_t i = 1; i < retransmitCount && interval < IHS_RETRANSMISSION_INTERVAL_MAX; i++) {
        interval *= 2;
    }
    return interval < IHS_RETRANSMISSION_INTERVAL_MAX ? interval : IHS_RETRANSMISSION_INTERVAL_MAX;
}

/* One warning per packet once it has been sent this many times (it has then waited ~150 ms): a
 * Wi-Fi hiccup shows up here long before the packet is given up. */
#define RETRANSMISSION_WARN_ATTEMPTS 5

/* How long a packet sent `sends` times has been waiting since its first send, from the back-off
 * schedule (the send timestamp in the header is that of the latest send only). */
static uint64_t RetransmissionWaited(uint8_t sends) {
    uint64_t total = 0;
    for (uint8_t i = 1; i < sends; i++) {
        total += IHS_RetransmissionInterval(i);
    }
    return total;
}

/*
 * A pending packet belongs to its timer task: the task's end callback takes it out of the queue
 * and frees it, on the timer thread. An ACK only marks it, under the lock, and the task then
 * skips the resend. (Cancelling used to take it out and stop the task from the receive thread;
 * when the ACK came as the timer fired, which on Wi-Fi it often does, both threads freed it.)
 */
typedef struct IHS_QueueItem {
    IHS_SessionPacket packet;
    IHS_SessionRetransmission *retransmission;
    bool acked;
} PendingRetransmission;

typedef struct RetransmissionQuery {
    IHS_SessionChannelId channelId;
    uint16_t packetId;
    uint16_t fragmentId;
} RetransmissionQuery;

static void RetransmissionQueueItemDestroy(PendingRetransmission *item, void *context);

static uint64_t RetransmissionTimerRun(int runCount, void *context);

static void RetransmissionTimerEnd(void *context);

static bool RetransmissionIdenticalPredicate(PendingRetransmission *item, void *context);

static bool RetransmissionPacketPredicate(PendingRetransmission *item, void *context);

void IHS_RetransmissionInit(IHS_SessionRetransmission *retransmission, IHS_Session *session) {
    retransmission->session = session;
    retransmission->lock = IHS_MutexCreate();
    retransmission->queue = IHS_QueueCreate(sizeof(PendingRetransmission));
}

void IHS_RetransmissionDeinit(IHS_SessionRetransmission *retransmission) {
    IHS_MutexLock(retransmission->lock);
    IHS_QueueDestroy(retransmission->queue, RetransmissionQueueItemDestroy, retransmission);
    IHS_MutexUnlock(retransmission->lock);
    IHS_MutexDestroy(retransmission->lock);
}

bool IHS_RetransmissionQueue(IHS_SessionRetransmission *retransmission, IHS_SessionPacket *packet) {
    assert(packet->body.data != NULL);
    assert(packet->body.offset == IHS_PACKET_HEADER_SIZE);
    if (packet->header.retransmitCount >= IHS_RETRANSMISSION_ATTEMPTS) {
        return false;
    }
    PendingRetransmission *pending = IHS_QueueItemObtain(retransmission->queue);
    pending->packet = *packet;
    pending->retransmission = retransmission;
    pending->acked = false;
    pending->packet.header.retransmitCount++;
    IHS_BufferTransferOwnership(&packet->body, &pending->packet.body);
    uint8_t sends = pending->packet.header.retransmitCount;
    if (sends > 1) {
        IHS_SessionCounterAdd(retransmission->session->counters.retransmissions, 1);
    }
    if (sends == RETRANSMISSION_WARN_ATTEMPTS) {
        // Once per packet: only the send that reaches the count logs it.
        IHS_SessionLogLimited(retransmission->session, IHS_LogLevelWarn, "Retransmission",
                       "Reliable packet (channel %u, id %u, fragment %d) not acknowledged after %u sends, "
                       "~%u ms; still resending",
                       pending->packet.header.channelId, pending->packet.header.packetId,
                       pending->packet.header.fragmentId, sends, (unsigned) RetransmissionWaited(sends));
    }
    IHS_SessionLog(retransmission->session, IHS_LogLevelVerbose, "Retransmission",
                   "Queued Packet(channelId=%u, packetId=%u, fragmentId=%u), retransmitCount=%u",
                   pending->packet.header.channelId, pending->packet.header.packetId,
                   pending->packet.header.fragmentId, pending->packet.header.retransmitCount);
    // In the queue before its task starts, so the end callback always finds it there. If the
    // timer is already shutting down, no task starts and Deinit frees the item.
    IHS_MutexLock(retransmission->lock);
    IHS_QueueAppend(retransmission->queue, pending);
    IHS_MutexUnlock(retransmission->lock);
    IHS_TimerTaskStart(retransmission->session->timers, RetransmissionTimerRun, RetransmissionTimerEnd,
                       IHS_RetransmissionInterval(pending->packet.header.retransmitCount), pending);
    return true;
}

bool IHS_RetransmissionCancel(IHS_SessionRetransmission *retransmission, IHS_SessionChannelId channelId,
                              uint16_t packetId, uint16_t fragmentId) {
    RetransmissionQuery query = {
            .channelId = channelId,
            .packetId = packetId,
            .fragmentId = fragmentId,
    };
    IHS_MutexLock(retransmission->lock);
    PendingRetransmission *match = IHS_QueueFindBy(retransmission->queue, RetransmissionPacketPredicate, &query);
    uint8_t retransmitCount = 0;
    uint32_t sendTimestamp = 0;
    if (match != NULL) {
        match->acked = true;
        retransmitCount = match->packet.header.retransmitCount;
        sendTimestamp = match->packet.header.sendTimestamp;
    }
    IHS_MutexUnlock(retransmission->lock);
    if (match == NULL) {
        return false;
    }
    int32_t sample = (int32_t) (IHS_SessionPacketTimestamp() - sendTimestamp);
    if (retransmitCount == 1 && sample >= 0) {
        // Sent once (the queued copy counts 1), so the ACK can only be for that send. Smoothed
        // like TCP's SRTT, by 1/8 per sample. A negative sample is skipped: as a uint32_t, one of
        // about -4 ms read as 65535996 ms on a Q74T, and the smoothing took about a hundred more to undo it.
        IHS_Session *session = retransmission->session;
        if (sample == 0) {
            sample = 1;  // 0 means not measured
        }
        uint32_t rtt = IHS_SessionCounterGet(session->counters.roundTrip);
        IHS_SessionCounterSet(session->counters.roundTrip,
                              rtt == 0 ? (uint32_t) sample
                                       : (uint32_t) ((int64_t) rtt + ((int64_t) sample - rtt) / 8));
    }
    if (retransmitCount >= RETRANSMISSION_WARN_ATTEMPTS) {
        // Pairs with the warning above: the packet got through after all.
        uint32_t waited = (uint32_t) RetransmissionWaited(retransmitCount);
        IHS_SessionCounterMax(&retransmission->session->counters.retransmitMaxDelayMs, waited);
        IHS_SessionLogLimited(retransmission->session, IHS_LogLevelInfo, "Retransmission",
                       "Reliable packet (channel %u, id %u, fragment %d) acknowledged after %u sends, ~%u ms",
                       channelId, packetId, (int) (int16_t) fragmentId, retransmitCount, waited);
    }
    IHS_SessionLog(retransmission->session, IHS_LogLevelVerbose, "Retransmission",
                   "Cancelling Packet(channelId=%u, packetId=%u, fragmentId=%u), retransmitCount=%u",
                   channelId, packetId, fragmentId, retransmitCount);
    return true;
}

static void RetransmissionQueueItemDestroy(PendingRetransmission *item, void *context) {
    (void) context;
    // The session destroys its timer first, and every task's end callback takes its item out,
    // so what is left here never had a task.
    IHS_SessionPacketClear(&item->packet, true);
}

static uint64_t RetransmissionTimerRun(int runCount, void *context) {
    (void) runCount;
    PendingRetransmission *pending = context;
    IHS_SessionRetransmission *retransmission = pending->retransmission;
    IHS_MutexLock(retransmission->lock);
    bool acked = pending->acked;
    IHS_MutexUnlock(retransmission->lock);
    if (!acked) {
        IHS_SessionPacket *packet = &pending->packet;
        IHS_SessionQueuePacket(retransmission->session, packet,
                               packet->header.retransmitCount < IHS_RETRANSMISSION_ATTEMPTS);
        assert(packet->body.data == NULL);
        if (packet->header.retransmitCount >= IHS_RETRANSMISSION_ATTEMPTS) {
            // That was the last send: nothing queues it again.
            IHS_SessionCounterAdd(retransmission->session->counters.retransmitGaveUp, 1);
            IHS_SessionLogLimited(retransmission->session, IHS_LogLevelError, "Retransmission",
                           "Gave up on reliable packet (channel %u, id %u, fragment %d) after %u sends, ~%u ms; "
                           "Steam delivers control messages in order, so later ones may be stuck behind it",
                           packet->header.channelId, packet->header.packetId, packet->header.fragmentId,
                           packet->header.retransmitCount,
                           (unsigned) RetransmissionWaited(packet->header.retransmitCount));
        }
    }
    return 0;
}

static void RetransmissionTimerEnd(void *context) {
    PendingRetransmission *pending = context;
    IHS_SessionRetransmission *retransmission = pending->retransmission;
    IHS_SessionLog(retransmission->session, IHS_LogLevelVerbose, "Retransmission",
                   "Timer ended for Packet(channelId=%u, packetId=%u, fragmentId=%u), retransmitCount=%u",
                   pending->packet.header.channelId, pending->packet.header.packetId, pending->packet.header.fragmentId,
                   pending->packet.header.retransmitCount);
    IHS_MutexLock(retransmission->lock);
    IHS_QueuePollBy(retransmission->queue, RetransmissionIdenticalPredicate, pending);
    IHS_MutexUnlock(retransmission->lock);
    // Out of the queue, so no ACK can reach it now. Its body is still here if it was acknowledged.
    IHS_SessionPacketClear(&pending->packet, true);
    IHS_QueueItemFree(pending);
}

static bool RetransmissionIdenticalPredicate(PendingRetransmission *item, void *context) {
    return item == context;
}

static bool RetransmissionPacketPredicate(PendingRetransmission *item, void *context) {
    RetransmissionQuery *query = context;
    return !item->acked && item->packet.header.channelId == query->channelId &&
           item->packet.header.packetId == query->packetId &&
           item->packet.header.fragmentId == query->fragmentId;
}