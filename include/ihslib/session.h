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

#include "common.h"
#include "audio.h"
#include "video.h"
#include "input.h"
#include "hid.h"

typedef struct IHS_Session IHS_Session;

typedef struct IHS_SessionInfo {
    IHS_SocketAddress address;
    uint8_t sessionKey[32];
    size_t sessionKeyLen;
    uint64_t steamId;
} IHS_SessionInfo;

/**
 * bitrateKbps for an adaptive bitrate. Steam then lowers the bitrate when the client reports lost
 * data and raises it again while the stream keeps up, up to maximum_decode_bitrate_kbps (30000).
 */
#define IHS_SESSION_BITRATE_ADAPTIVE (-2)

typedef struct IHS_SessionConfig {
    bool enableAudio;
    bool enableHevc;
    /** Ask the host for forward error correction on video, so a lost packet can be rebuilt. */
    bool enableFec;
    /**
     * Steam's quality preference (EStreamQualityPreference): -1 automatic, 1 fast, 2 balanced,
     * 3 beautiful. 0 keeps balanced.
     */
    int quality;
    /**
     * Bitrate to ask for in kbit/s. 0 keeps 30000; -1 lets Steam decide; IHS_SESSION_BITRATE_ADAPTIVE
     * asks for no fixed bitrate, which turns on Steam's own bitrate control.
     */
    int bitrateKbps;
    /**
     * Largest picture the client wants. 0 leaves it to Steam, which then stays at the 1920x1080
     * display ihslib describes; the maximum resolution in the streaming request doesn't lift that.
     */
    int maxWidth;
    int maxHeight;
    /** Frames per second the client can show; 0 keeps 59.94. */
    int maxFramerate;
    /**
     * The display can show HDR: HDR10 joins the colorspaces the client supports. Steam then sends
     * HDR10 (10-bit HEVC) when the host's display is in HDR, and IHS_StreamVideoConfig.colorspace
     * says which colorspace it chose. Set the streaming request's displayHdr too.
     */
    bool enableHdr;
    /**
     * After a lost video frame, report the loss and go on decoding the frames that follow, instead
     * of dropping them until a keyframe comes. The picture may smear until the keyframe lands.
     */
    bool keepPlayingAfterLoss;
} IHS_SessionConfig;

typedef struct IHS_StreamSessionCallbacks {
    void (*initialized)(IHS_Session *session, void *context);

    void (*connecting)(IHS_Session *session, void *context);

    void (*configuring)(IHS_Session *session, IHS_SessionConfig *config, void *context);

    void (*connected)(IHS_Session *session, void *context);

    void (*disconnected)(IHS_Session *session, void *context);

    /**
     * All the resources and states has been destroyed. Nothing can be used beyond this call.
     * @param session Session pointer
     * @param context Callback context
     */
    void (*finalized)(IHS_Session *session, void *context);
} IHS_StreamSessionCallbacks;

/*
 * Lifecycle functions
 */

/**
 * Create session instance
 * @param clientConfig Client info
 * @param sessionInfo Session info
 * @return Session instance
 */
IHS_Session *IHS_SessionCreate(const IHS_ClientConfig *clientConfig, const IHS_SessionInfo *sessionInfo);

/**
 * Start receive and send thread, and send connect request
 * @param session Session instance
 * @return
 */
bool IHS_SessionConnect(IHS_Session *session);

/**
 * Send disconnect request
 * @param session Session instance
 */
void IHS_SessionDisconnect(IHS_Session *session);

/**
 * Wait for all threads to finish
 * @param session Session instance
 */
void IHS_SessionThreadedJoin(IHS_Session *session);

/**
 * Release all resources of the session and free the pointer
 * @param session Session instance
 */
void IHS_SessionDestroy(IHS_Session *session);

/*
 * Callback related functions
 */

void IHS_SessionSetSessionCallbacks(IHS_Session *session, const IHS_StreamSessionCallbacks *callbacks, void *context);

void IHS_SessionSetAudioCallbacks(IHS_Session *session, const IHS_StreamAudioCallbacks *callbacks, void *context);

void IHS_SessionSetVideoCallbacks(IHS_Session *session, const IHS_StreamVideoCallbacks *callbacks, void *context);

void IHS_SessionSetInputCallbacks(IHS_Session *session, const IHS_StreamInputCallbacks *callbacks, void *context);

/**
 * Register microphone (client → server) callbacks. Setting non-NULL callbacks here
 * advertises the `enable_microphone_streaming` capability to the server during
 * negotiation; the server may then issue Start/Stop microphone control messages.
 *
 * Must be called before IHS_SessionConnect — capability negotiation happens once and
 * cannot be re-advertised mid-session.
 */
void IHS_SessionSetMicrophoneCallbacks(IHS_Session *session, const IHS_StreamMicrophoneCallbacks *callbacks,
                                       void *context);

/**
 * Submit one encoded microphone frame for transmission. Call from the capture / encode
 * thread, once per Opus packet (or once per PCM chunk when codec=Raw). The payload is
 * wrapped in a data-frame header and queued for unreliable send on the mic data channel.
 *
 * Returns false if no microphone channel is currently active (the server hasn't issued
 * Start, or has issued Stop, or the session isn't connected); the caller can use this
 * as the cue to drop the frame.
 */
bool IHS_SessionSendMicrophoneData(IHS_Session *session, const uint8_t *data, size_t len);

/**
 * One of the four client-owned stages of a video frame's lifecycle that the
 * application reports back to ihslib via IHS_SessionReportVideoFrameStage.
 * Values match Steam's EStreamFrameEvent so the wire encoding is a direct
 * cast — do not renumber.
 */
typedef enum IHS_VideoFrameStage {
    IHS_VideoFrameStageDecodeBegin = 14,
    IHS_VideoFrameStageDecodeEnd = 15,
    IHS_VideoFrameStageUploadBegin = 16,
    IHS_VideoFrameStageUploadEnd = 17,
} IHS_VideoFrameStage;

/**
 * Per-frame outcome reported by the application. Values match Steam's
 * EStreamFrameResult — do not renumber.
 */
typedef enum IHS_VideoFrameResult {
    IHS_VideoFrameResultPending = 0,
    IHS_VideoFrameResultDisplayed = 1,
    IHS_VideoFrameResultDroppedNetworkSlow = 2,
    IHS_VideoFrameResultDroppedNetworkLost = 3,
    IHS_VideoFrameResultDroppedDecodeSlow = 4,
    IHS_VideoFrameResultDroppedDecodeCorrupt = 5,
    IHS_VideoFrameResultDroppedLate = 6,
    IHS_VideoFrameResultDroppedReset = 7,
} IHS_VideoFrameResult;

/**
 * Report a per-frame decode/upload milestone. ihslib accumulates these into a
 * 1 Hz CFrameStatsListMsg that drives the host's adaptive-bitrate / framerate
 * loop. `timestamp` is in IHS_SessionPacketTimestamp units (1/65536 second);
 * pass `0` to use the current session clock.
 *
 * Safe to call from any thread.
 */
void IHS_SessionReportVideoFrameStage(IHS_Session *session, uint16_t frameId,
                                      IHS_VideoFrameStage stage, uint32_t timestamp);

/**
 * Report the final outcome of a video frame (displayed or one of the drop
 * categories), AFTER any IHS_SessionReportVideoFrameStage calls for it. ihslib
 * already reports each frame it hands to the decoder, as displayed when the
 * decoder takes it; call this to report a different outcome.
 *
 * Safe to call from any thread.
 */
void IHS_SessionReportVideoFrameComplete(IHS_Session *session, uint16_t frameId,
                                         IHS_VideoFrameResult result);

/**
 * Debug toggle: when true, each flush packet carries per-frame CFrameStats
 * rows in addition to the aggregated stats. Default false (aggregated only,
 * matching Steam's normal operating mode). Useful for capturing wire traces.
 */
void IHS_SessionStatsSetFullReporting(IHS_Session *session, bool enabled);

void IHS_SessionSetLogFunction(IHS_Session *session, IHS_LogFunction *logFunction);

const IHS_SessionInfo *IHS_SessionGetInfo(const IHS_Session *session);

/**
 * Counters since the session started, for display. Safe to call from any thread while the session
 * exists, also after it has disconnected.
 */
typedef struct IHS_SessionStreamStats {
    /** Packets received on the video channel. */
    uint32_t videoPackets;
    /** Video packets that never arrived (from gaps in the packet ids). */
    uint32_t videoPacketsLost;
    /** Data lost notifications sent for video. Steam answers each with a keyframe. */
    uint32_t lostReports;
    /** Smoothed round trip of reliable packets in milliseconds; 0 until measured. */
    float roundTripMs;
    /** The host sends video with forward error correction. Packets above count its shards. */
    bool fec;
    /** Video messages rebuilt from FEC parity. */
    uint32_t fecRecovered;
    /** Video messages lost despite FEC (more shards missing than the parity covers). */
    uint32_t fecUnrecoverable;
    /** The session ended because the host went silent (no packets for 10 s), not by request. */
    bool timedOut;
    /** Reliable (control) packets that had to be sent again. */
    uint32_t retransmissions;
    /** Reliable packets given up on after the last attempt (about 12 s). */
    uint32_t retransmitGaveUp;
    /** Longest a reliable packet waited after its first send, acknowledged or given up, in ms (from the back-off schedule),
     *  since the previous call (each call clears it). */
    uint32_t retransmitMaxDelayMs;
    /** Times Steam asked for a full controller report (a delta was lost or its CRC did not match). */
    uint32_t hidFullReportRequests;
    /** Full controller reports sent on our own, once a second per controller. */
    uint32_t hidResyncReports;
    /** Controller requests that took the HID worker more than 50 ms. */
    uint32_t hidSlowJobs;
    /** Reliable packets stopped by an ACK for a later id (Steam acknowledges the newest of a burst). */
    uint32_t ackCumulativeCancels;
    /*
     * Where the controller path can stop. "Interval" fields cover the time since the previous call
     * of IHS_SessionGetStreamStats and start again from zero on each call, so call it from one
     * place only (the app's periodic stats line); the others are totals since the session began.
     */
    /** The host currently lets input through (1) or has turned it off (0). */
    uint32_t inputEnabled;
    /** Times the host switched input streaming on or off. */
    uint32_t inputTransitions;
    /** Controller messages dropped because the host had input off. */
    uint32_t inputDisabledDrops;
    /** Interval: most packets waiting in the send queue at once. */
    uint32_t sendQueueDepthMax;
    /** Interval: longest a packet waited in the send queue before it was sent, in ms. */
    uint32_t sendQueueLatencyMaxMs;
    /** Control channel packets sent, resends included. */
    uint32_t controlPacketsSent;
    /** Interval: latest a timer task (controller poll, resends) started after it was due, in ms. */
    uint32_t timerLagMaxMs;
    /** Interval: control packets acknowledged, and the longest and mean wait from first send, in ms. */
    uint32_t controlAckCount;
    uint32_t controlAckMaxMs;
    uint32_t controlAckAvgMs;
    /** How long the oldest unacknowledged control packet has waited at the time of the call, in ms. */
    uint32_t controlOldestPendingMs;
    /** Times the host went over 500 ms without acknowledging control packets while video kept coming. */
    uint32_t controlAckStalls;
    /** Controller reports produced (changes and full reports) and put on the wire. */
    uint32_t hidReportsGenerated;
    uint32_t hidReportsSent;
    /** Interval: longest a controller event or request waited for a controller in use, in ms. */
    uint32_t hidUseLockMaxWaitMs;
    /** Times controller input waited over 200 ms to be sent. */
    uint32_t hidInputStalls;
    /** Reliable control packets from the host: all, those it marked as resent, those we already had. */
    uint32_t ctrlRxPackets;
    uint32_t ctrlRxResent;
    uint32_t ctrlRxDuplicate;
    /** ACKs we sent short of the newest id received, because an earlier packet was missing. */
    uint32_t ctrlAckHoles;
    /** Interval: longest a missing host control packet held back the later ones, in ms. */
    uint32_t ctrlHoleMaxMs;
    /** Control messages from the host delivered in order. */
    uint32_t ctrlRxMessages;
    /** Controller requests from the host run on the HID worker. */
    uint32_t hidJobs;
    /** Interval: longest such request waited in the worker's queue, and longest one ran, in ms. */
    uint32_t hidJobWaitMaxMs;
    uint32_t hidJobRunMaxMs;
} IHS_SessionStreamStats;

void IHS_SessionGetStreamStats(const IHS_Session *session, IHS_SessionStreamStats *stats);
