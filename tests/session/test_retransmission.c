/*
 * Retransmission back-off: the schedule that keeps a reliable control packet (and so the pad) alive
 * through a Wi-Fi stall. Steam delivers control messages in order, so a packet given up too early
 * holds back everything behind it.
 */

#include <assert.h>

#include "session/retransmission.h"
#include "session/session_pri.h"

static void test_interval_backs_off_and_caps(void) {
    // The queued copy of a packet sent once has retransmitCount 1.
    assert(IHS_RetransmissionInterval(0) == 10);
    assert(IHS_RetransmissionInterval(1) == 10);
    assert(IHS_RetransmissionInterval(2) == 20);
    assert(IHS_RetransmissionInterval(3) == 40);
    assert(IHS_RetransmissionInterval(4) == 80);
    assert(IHS_RetransmissionInterval(5) == 160);
    // 320 would be next: capped.
    assert(IHS_RetransmissionInterval(6) == IHS_RETRANSMISSION_INTERVAL_MAX);
    assert(IHS_RetransmissionInterval(255) == IHS_RETRANSMISSION_INTERVAL_MAX);

    uint64_t previous = 0;
    for (unsigned count = 0; count <= 255; count++) {
        uint64_t interval = IHS_RetransmissionInterval((uint8_t) count);
        assert(interval >= IHS_RETRANSMISSION_INTERVAL_MIN);
        assert(interval <= IHS_RETRANSMISSION_INTERVAL_MAX);
        assert(interval >= previous);
        previous = interval;
    }
}

static void test_budget_outlasts_a_wifi_stall(void) {
    // Time from the first send to giving up: one wait per queued copy, counts 1 .. ATTEMPTS-1.
    uint64_t total = 0;
    for (unsigned count = 1; count < IHS_RETRANSMISSION_ATTEMPTS; count++) {
        total += IHS_RetransmissionInterval((uint8_t) count);
    }
    // The old 20 attempts 10 ms apart gave up after 200 ms. This must be on the order of 12 s.
    assert(total >= 10000);
    assert(total <= 15000);
}

static void test_exhausted_packet_is_not_queued(void) {
    // Returns before touching the session or the queue, so a bare struct is enough.
    IHS_SessionRetransmission retransmission = {0};
    IHS_SessionPacket packet = {0};
    IHS_SessionPacketBodyInitialize(&packet.body, false);
    packet.header.retransmitCount = IHS_RETRANSMISSION_ATTEMPTS;
    assert(!IHS_RetransmissionQueue(&retransmission, &packet));
    // Not taken: the body is still ours.
    assert(packet.body.data != NULL);
    IHS_SessionPacketClear(&packet, true);
}

static void test_log_limit_is_one_line_per_second(void) {
    IHS_LogLimit limit = {0};
    uint32_t count = 0;
    // The first occurrence logs at once, and stands for itself.
    assert(IHS_LogLimitAllow(&limit, &count));
    assert(count == 1);
    // A burst right after it is held back (the loop is far shorter than a second).
    for (int i = 0; i < 100; i++) {
        assert(!IHS_LogLimitAllow(&limit, &count));
    }
    // A second later the next one logs and reports the 100 held back plus itself.
    limit.lastMs -= 1001;
    assert(IHS_LogLimitAllow(&limit, &count));
    assert(count == 101);
}

int main(void) {
    test_interval_backs_off_and_caps();
    test_budget_outlasts_a_wifi_stall();
    test_exhausted_packet_is_not_queued();
    test_log_limit_is_one_line_per_second();
    return 0;
}
