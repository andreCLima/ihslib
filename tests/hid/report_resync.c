/*
 * Delta/full reports and the identical-state dedup. Steam rebuilds the pad from our deltas and
 * checks the CRC we send with each; when it disagrees it asks for a full report. That request (and
 * our own periodic full report) must get through even when the state has not changed since we last
 * sent it, which is exactly the case when a delta was lost on the way.
 */

#include <assert.h>
#include <string.h>

#include "crc32.h"
#include "hid/report.h"

#define STATE_LEN 48

int main(void) {
    uint8_t idle[STATE_LEN] = {0};
    uint8_t pressed[STATE_LEN] = {0};
    pressed[16] = 0x40;

    IHS_HIDReportHolder holder;
    IHS_HIDReportHolderInit(&holder, 7);
    IHS_HIDReportHolderSetReportLength(&holder, 65);

    // First full report: nothing sent before, so it goes out.
    IHS_HIDReportHolderAddFull(&holder, idle, STATE_LEN);
    IHS_HIDDeviceReportMessage *report = IHS_HIDReportHolderGetMessage(&holder);
    assert(report != NULL && report->n_reports == 1);
    assert(report->reports[0]->has_full_report && report->reports[0]->full_report.len == STATE_LEN);
    IHS_HIDReportHolderResetMessage(&holder);

    // A full report of the state we already sent is NOT dropped (periodic resync, RequestFullReport).
    IHS_HIDReportHolderAddFull(&holder, idle, STATE_LEN);
    report = IHS_HIDReportHolderGetMessage(&holder);
    assert(report != NULL && report->n_reports == 1);
    assert(memcmp(report->reports[0]->full_report.data, idle, STATE_LEN) == 0);
    IHS_HIDReportHolderResetMessage(&holder);

    // A delta that changes nothing is dropped.
    IHS_HIDReportHolderAddDelta(&holder, idle, idle, STATE_LEN);
    assert(IHS_HIDReportHolderGetMessage(&holder) == NULL);
    IHS_HIDReportHolderResetMessage(&holder);

    // A real delta carries the CRC of the resulting state and its size.
    IHS_HIDReportHolderAddDelta(&holder, idle, pressed, STATE_LEN);
    report = IHS_HIDReportHolderGetMessage(&holder);
    assert(report != NULL && report->n_reports == 1);
    assert(report->reports[0]->has_delta_report);
    assert(report->reports[0]->delta_report_size == STATE_LEN);
    assert(report->reports[0]->delta_report_crc == IHS_CRC32(pressed, STATE_LEN));
    // 9 bytes of changed-byte bitmap ((65 + 7) / 8) plus the one byte that changed.
    assert(report->reports[0]->delta_report.len == 10);
    IHS_HIDReportHolderResetMessage(&holder);

    // The same delta again is a duplicate of what is on the wire: dropped...
    IHS_HIDReportHolderAddDelta(&holder, idle, pressed, STATE_LEN);
    assert(IHS_HIDReportHolderGetMessage(&holder) == NULL);
    IHS_HIDReportHolderResetMessage(&holder);

    // ...but the full report Steam asks for after a CRC mismatch still goes out, with that state.
    IHS_HIDReportHolderAddFull(&holder, pressed, STATE_LEN);
    report = IHS_HIDReportHolderGetMessage(&holder);
    assert(report != NULL && report->n_reports == 1);
    assert(report->reports[0]->has_full_report);
    assert(memcmp(report->reports[0]->full_report.data, pressed, STATE_LEN) == 0);
    IHS_HIDReportHolderResetMessage(&holder);

    // Delta after that full one still dedups against it: releasing the button is a change.
    IHS_HIDReportHolderAddDelta(&holder, pressed, idle, STATE_LEN);
    report = IHS_HIDReportHolderGetMessage(&holder);
    assert(report != NULL && report->reports[0]->has_delta_report);
    assert(report->reports[0]->delta_report_crc == IHS_CRC32(idle, STATE_LEN));
    IHS_HIDReportHolderResetMessage(&holder);

    IHS_HIDReportHolderDeinit(&holder);
    return 0;
}
