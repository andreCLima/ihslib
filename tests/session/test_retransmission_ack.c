/*
 * Cancelling retransmissions by ACK. Steam sends one ACK for the newest packet when several
 * arrive together, so an ACK must also stop the pending packets of its channel with a lower id,
 * across the 16-bit wrap and for any fragment, and nothing else. The checks go through the return
 * value and the session counters, so none of them waits for a timer.
 */

#include <assert.h>

#include "test_session.h"
#include "session/retransmission.h"
#include "session/session_pri.h"

static void pend(IHS_Session *session, IHS_SessionChannelId channel, uint16_t id, int16_t fragment) {
    IHS_SessionPacket packet = {0};
    IHS_SessionPacketBodyInitialize(&packet.body, false);
    packet.header.type = IHS_SessionPacketTypeReliable;
    packet.header.channelId = channel;
    packet.header.packetId = id;
    packet.header.fragmentId = fragment;
    assert(IHS_RetransmissionQueue(&session->retransmission, &packet));
}

static uint32_t covered(IHS_Session *session) {
    return IHS_SessionCounterGet(session->counters.ackCumulativeCancels);
}

int main(void) {
    IHS_Session *session = IHS_TestSessionCreate();
    IHS_SessionRetransmission *r = &session->retransmission;

    // Ids straddling the wrap, plus one still ahead of the ACK.
    pend(session, IHS_SessionChannelIdControl, 65534, 0);
    pend(session, IHS_SessionChannelIdControl, 65535, 0);
    pend(session, IHS_SessionChannelIdControl, 0, 0);
    pend(session, IHS_SessionChannelIdControl, 1, 0);
    pend(session, IHS_SessionChannelIdControl, 2, 0);
    // Same ids on another channel must not be touched by control ACKs.
    pend(session, IHS_SessionChannelIdStats, 65534, 0);

    // ACK for id 0 covers 65534 and 65535 (a smaller id after the wrap), not 1 or 2.
    assert(IHS_RetransmissionAck(r, IHS_SessionChannelIdControl, 0, 0, true));
    assert(covered(session) == 2);
    // Repeating it finds nothing pending.
    assert(!IHS_RetransmissionAck(r, IHS_SessionChannelIdControl, 0, 0, true));
    assert(covered(session) == 2);
    // ACK for 2 covers 1 and is exact for 2.
    assert(IHS_RetransmissionAck(r, IHS_SessionChannelIdControl, 2, 0, true));
    assert(covered(session) == 3);
    // Nothing on the control channel is left; the stats channel still has its packet.
    assert(!IHS_RetransmissionAck(r, IHS_SessionChannelIdControl, 100, 0, true));
    assert(IHS_RetransmissionAck(r, IHS_SessionChannelIdStats, 65534, 0, false));
    assert(covered(session) == 3);

    // An ACK for a lower id does not reach a higher pending one.
    pend(session, IHS_SessionChannelIdControl, 5, 0);
    pend(session, IHS_SessionChannelIdControl, 9, 0);
    assert(!IHS_RetransmissionAck(r, IHS_SessionChannelIdControl, 3, 0, true));
    assert(covered(session) == 3);
    // 7 covers 5 and finds no packet 7 of its own; 9 is still pending.
    assert(IHS_RetransmissionAck(r, IHS_SessionChannelIdControl, 7, 0, true));
    assert(covered(session) == 4);
    assert(IHS_RetransmissionAck(r, IHS_SessionChannelIdControl, 9, 0, true));
    assert(covered(session) == 4);

    // Fragments of one id: an ACK for one fragment of the newest id covers the others of it.
    pend(session, IHS_SessionChannelIdControl, 7, 3);
    pend(session, IHS_SessionChannelIdControl, 7, 1);
    pend(session, IHS_SessionChannelIdControl, 6, 2);
    assert(IHS_RetransmissionAck(r, IHS_SessionChannelIdControl, 7, 3, true));
    assert(covered(session) == 4 + 2);

    // Not cumulative (NACK, discovery): exact packet only.
    pend(session, IHS_SessionChannelIdControl, 10, 0);
    pend(session, IHS_SessionChannelIdControl, 11, 0);
    assert(IHS_RetransmissionAck(r, IHS_SessionChannelIdControl, 11, 0, false));
    assert(covered(session) == 6);
    assert(IHS_RetransmissionAck(r, IHS_SessionChannelIdControl, 10, 0, false));
    assert(!IHS_RetransmissionAck(r, IHS_SessionChannelIdControl, 12, 0, false));

    // The acknowledgements above are in the stats once, and the interval starts again.
    IHS_SessionStreamStats stats;
    IHS_SessionGetStreamStats(session, &stats);
    assert(stats.inputEnabled == 1);
    assert(stats.controlAckCount > 0);
    assert(stats.ackCumulativeCancels == 6);
    assert(stats.controlOldestPendingMs == 0);  // everything on the control channel is acknowledged
    IHS_SessionGetStreamStats(session, &stats);
    assert(stats.controlAckCount == 0);
    assert(stats.controlAckMaxMs == 0);

    IHS_SessionDestroy(session);
    return 0;
}
