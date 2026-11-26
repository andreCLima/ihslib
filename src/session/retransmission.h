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

#pragma once

#include "ihs_thread.h"
#include "ihs_queue.h"
#include "ihs_timer.h"
#include "packet.h"

typedef struct IHS_Session IHS_Session;

#define IHS_RETRANSMISSION_ACK_CHANNELS 16

/* ACKs seen on one channel since the last Debug line; guarded by the retransmission lock. */
typedef struct IHS_SessionRetransmissionAckStats {
    uint32_t received;
    uint32_t covered;
    uint32_t noPending;
    uint32_t lastLogMs;
} IHS_SessionRetransmissionAckStats;

typedef struct IHS_SessionRetransmission {
    IHS_Session *session;
    IHS_Mutex *lock;
    IHS_Queue *queue;
    IHS_SessionRetransmissionAckStats ackStats[IHS_RETRANSMISSION_ACK_CHANNELS];
} IHS_SessionRetransmission;

void IHS_RetransmissionInit(IHS_SessionRetransmission *retransmission, IHS_Session *session);

void IHS_RetransmissionDeinit(IHS_SessionRetransmission *retransmission);

bool IHS_RetransmissionQueue(IHS_SessionRetransmission *retransmission, IHS_SessionPacket *packet);

bool IHS_RetransmissionCancel(IHS_SessionRetransmission *retransmission, IHS_SessionChannelId channelId,
                              uint16_t packetId, uint16_t fragmentId);


/** Delay before the first resend, in ms. Doubles with each attempt up to the maximum. */
#define IHS_RETRANSMISSION_INTERVAL_MIN 10
#define IHS_RETRANSMISSION_INTERVAL_MAX 250
/** A packet whose retransmitCount reaches this is not queued again and is given up on. */
#define IHS_RETRANSMISSION_ATTEMPTS 52

/**
 * Delay in ms before the next send of a packet that has been sent \p retransmitCount times
 * (the queued copy counts 1). Exposed so tests can check the back-off.
 */
uint64_t IHS_RetransmissionInterval(uint8_t retransmitCount);


/**
 * Stop resending what an ACK acknowledges. With \p cumulative the ACK also covers every pending
 * packet of the channel with an id up to \p packetId (16-bit wrap-around, any fragment), as Steam
 * sends them: one ACK for the newest packet when several arrive together. The round trip is
 * sampled only from the exact packet.
 * @return true if anything was cancelled
 */
bool IHS_RetransmissionAck(IHS_SessionRetransmission *retransmission, IHS_SessionChannelId channelId,
                           uint16_t packetId, uint16_t fragmentId, bool cumulative);


/**
 * How long the oldest packet of \p channelId still waiting for its ACK has waited since its first
 * send, in ms; 0 if none is pending. Walks the queue under the lock.
 */
uint32_t IHS_RetransmissionOldestPendingMs(IHS_SessionRetransmission *retransmission, IHS_SessionChannelId channelId);
