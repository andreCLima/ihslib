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

#include <stdlib.h>
#include <stdio.h>

#include "ch_control.h"

#include "session/session_pri.h"

#include "protobuf/pb_utils.h"
#include "protobuf/discovery.pb-c.h"

static void OnNegotiationInit(IHS_SessionChannel *channel, const CNegotiationInitMsg *message, uint16_t packetId);

static void OnNegotiationSetConfig(IHS_SessionChannel *channel, const CNegotiationSetConfigMsg *message,
                                   uint16_t packetId);

static void OnConnected(IHS_SessionChannel *channel);

void IHS_SessionChannelControlOnNegotiation(IHS_SessionChannel *channel, EStreamControlMessage type,
                                            IHS_Buffer *payload, const IHS_SessionPacketHeader *header) {
    // Negotiation messages are only valid in the Negotiating phase. Drop replays that
    // arrive before auth succeeds or after we're Connected.
    if (channel->session->state.connectionState != IHS_SessionConnectionStateNegotiating) {
        return;
    }
    switch (type) {
        case k_EStreamControlNegotiationInit: {
            CNegotiationInitMsg *message = IHS_UNPACK_BUFFER(cnegotiation_init_msg__unpack, payload);
            if (message == NULL) {
                IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control", "Malformed CNegotiationInitMsg");
                break;
            }
            OnNegotiationInit(channel, message, header->packetId);
            cnegotiation_init_msg__free_unpacked(message, NULL);
            break;
        }
        case k_EStreamControlNegotiationSetConfig: {
            CNegotiationSetConfigMsg *message = IHS_UNPACK_BUFFER(cnegotiation_set_config_msg__unpack, payload);
            if (message == NULL) {
                IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control", "Malformed CNegotiationSetConfigMsg");
                break;
            }
            OnNegotiationSetConfig(channel, message, header->packetId);
            cnegotiation_set_config_msg__free_unpacked(message, NULL);
            break;
        }
        default: {
            abort();
        }
    }
}

static void OnNegotiationInit(IHS_SessionChannel *channel, const CNegotiationInitMsg *message, uint16_t packetId) {
    IHS_SessionConfig ihsConf = {
            .enableAudio = true,
            .enableHevc = false,
    };

    IHS_Session *session = channel->session;
    if (session->callbacks.session && session->callbacks.session->configuring) {
        session->callbacks.session->configuring(session, &ihsConf, session->callbackContexts.session);
    }

    EStreamAudioCodec audioCodec = k_EStreamAudioCodecNone;
    EStreamVideoCodec videoCodec = k_EStreamVideoCodecNone;
    if (ihsConf.enableAudio) {
        for (int i = 0; i < message->n_supported_audio_codecs; i++) {
            EStreamAudioCodec codec = message->supported_audio_codecs[i];
            if (codec == k_EStreamAudioCodecOpus) {
                audioCodec = codec;
            }
        }
    }
    if (ihsConf.enableHevc) {
        for (int i = 0; i < message->n_supported_video_codecs; i++) {
            EStreamVideoCodec codec = message->supported_video_codecs[i];
            if (codec == k_EStreamVideoCodecHEVC) {
                videoCodec = codec;
                break;
            }
        }
    }
    if (videoCodec == IHS_StreamVideoCodecNone) {
        for (int i = 0; i < message->n_supported_video_codecs; i++) {
            EStreamVideoCodec codec = message->supported_video_codecs[i];
            if (codec == k_EStreamVideoCodecH264) {
                videoCodec = codec;
                break;
            }
        }
    }

    CNegotiatedConfig config = CNEGOTIATED_CONFIG__INIT;
    PROTOBUF_C_SET_VALUE(config, reliable_data, false);

    config.has_selected_audio_codec = audioCodec != k_EStreamAudioCodecNone;
    config.selected_audio_codec = audioCodec;

    config.has_selected_video_codec = videoCodec != k_EStreamVideoCodecNone;
    config.selected_video_codec = videoCodec;

    // 59.94 unless the app asks for more, in 1/100 fps.
    uint32_t framerateNum = ihsConf.maxFramerate > 0 ? (uint32_t) ihsConf.maxFramerate * 100 : 5994;
    CStreamVideoMode availableVideoMode = CSTREAM_VIDEO_MODE__INIT;
    availableVideoMode.width = 1920;
    availableVideoMode.height = 1080;
    PROTOBUF_C_SET_VALUE(availableVideoMode, refresh_rate_numerator, framerateNum);
    PROTOBUF_C_SET_VALUE(availableVideoMode, refresh_rate_denominator, 100);

    CStreamVideoMode *availableVideoModes[] = {&availableVideoMode};
    config.n_available_video_modes = 1;
    config.available_video_modes = availableVideoModes;

    PROTOBUF_C_SET_VALUE(config, enable_remote_hid, 0);

    CStreamingClientConfig clientConfig = CSTREAMING_CLIENT_CONFIG__INIT;

    // Fields 2 and 3 are desired_resolution_x/y in current Steam.
    PROTOBUF_C_SET_VALUE(clientConfig, maximum_resolution_x, ihsConf.maxWidth > 0 ? ihsConf.maxWidth : 0);
    PROTOBUF_C_SET_VALUE(clientConfig, maximum_resolution_y, ihsConf.maxHeight > 0 ? ihsConf.maxHeight : 0);
    PROTOBUF_C_SET_VALUE(clientConfig, enable_hardware_decoding, true);
    PROTOBUF_C_SET_VALUE(clientConfig, enable_performance_overlay, true);
    if (ihsConf.enableAudio) {
        PROTOBUF_C_SET_VALUE(clientConfig, audio_channels, 2);
        PROTOBUF_C_SET_VALUE(clientConfig, enable_audio_streaming, true);
    }
    // Mic streaming is opt-in: only advertise when the user has registered mic callbacks.
    // Steam's server keys off this flag — without it the server never issues Start, so
    // there's no point in advertising mic support we can't actually fulfil. Matches the
    // capability semantics around CStreamingClientConfig.enable_microphone_streaming
    // documented in the mic RE.
    if (session->callbacks.microphone != NULL) {
        PROTOBUF_C_SET_VALUE(clientConfig, enable_microphone_streaming, true);
    }
    PROTOBUF_C_SET_VALUE(clientConfig, enable_video_streaming, true);
    PROTOBUF_C_SET_VALUE(clientConfig, maximum_framerate_numerator, framerateNum);
    PROTOBUF_C_SET_VALUE(clientConfig, maximum_framerate_denominator, 100);
    // Field 6 is desired_bitrate_kbps in current Steam; ihslib's copy still calls it maximum.
    int bitrateKbps = ihsConf.bitrateKbps != 0 ? ihsConf.bitrateKbps : 30000;
    PROTOBUF_C_SET_VALUE(clientConfig, quality,
                         ihsConf.quality != 0 ? (EStreamQualityPreference) ihsConf.quality : k_EStreamQualityBalanced);
    // 0 is what Steam's own Steam Link app sends: no fixed bitrate, so the host's dynamic bitrate
    // control runs. -1 has the host pick a fixed bitrate instead.
    PROTOBUF_C_SET_VALUE(clientConfig, maximum_bitrate_kbps,
                         ihsConf.bitrateKbps == IHS_SESSION_BITRATE_ADAPTIVE ? 0 : bitrateKbps);
    if (ihsConf.enableHevc) {
        PROTOBUF_C_SET_VALUE(clientConfig, enable_video_hevc, true);
    }

    clientConfig.controller_overlay_hotkey = "auto";

    // enable_unreliable_fec (25) is newer than ihslib's copy of the protocol. protobuf-c packs
    // unknown fields as they are, so it goes out that way.
    uint8_t fecOn = 1;
    ProtobufCMessageUnknownField fecField = {25, PROTOBUF_C_WIRE_TYPE_VARINT, 1, &fecOn};
    if (ihsConf.enableFec) {
        clientConfig.base.n_unknown_fields = 1;
        clientConfig.base.unknown_fields = &fecField;
    }

    CStreamingClientCaps clientCaps = CSTREAMING_CLIENT_CAPS__INIT;

    clientCaps.system_info = "\"SystemInfo\"\n{\n\t\"OSType\"\t\t\"-197\"\n\t\"CPUID\"\t\t\"ARM\"\n"
                             "\t\"CPUGhz\"\t\t\"0.000000\"\n\t\"PhysicalCPUCount\"\t\"1\"\n"
                             "\t\"LogicalCPUCount\"\t\"1\"\n\t\"SystemRAM\"\t\t\"263\"\n"
                             "\t\"VideoVendorID\"\t\"0\"\n\t\"VideoDeviceID\"\t\"0\"\n"
                             "\t\"VideoRevision\"\t\"0\"\n\t\"VideoRAM\"\t\t\"0\"\n"
                             "\t\"VideoDisplayX\"\t\"1920\"\n\t\"VideoDisplayY\"\t\"1080\"\n"
                             "\t\"VideoDisplayNameID\"\t\"JN-MD133BFHDR\"\n}\n";
    PROTOBUF_C_SET_VALUE(clientCaps, system_can_suspend, true);
    // Steam keeps the stream under what the client says it can decode.
    PROTOBUF_C_SET_VALUE(clientCaps, maximum_decode_bitrate_kbps, bitrateKbps > 30000 ? bitrateKbps : 30000);
    PROTOBUF_C_SET_VALUE(clientCaps, maximum_burst_bitrate_kbps, bitrateKbps > 30000 ? bitrateKbps * 3 : 90000);
    if (ihsConf.enableHevc) {
        PROTOBUF_C_SET_VALUE(clientCaps, supports_video_hevc, true);
    }
    PROTOBUF_C_SET_VALUE(clientCaps, form_factor, k_EStreamDeviceFormFactorTV);
    // HDR10 (5) is newer than ihslib's copy of EStreamColorspace. Without a list Steam picks an SDR
    // colorspace, so the list is only sent to offer HDR10, with the SDR ones next to it.
    EStreamColorspace colorspaces[] = {k_EStreamColorspace_BT709, k_EStreamColorspace_BT601,
                                       (EStreamColorspace) 5};
    if (ihsConf.enableHdr) {
        clientCaps.n_supported_colorspaces = sizeof(colorspaces) / sizeof(colorspaces[0]);
        clientCaps.supported_colorspaces = colorspaces;
    }

    // supported_video_codecs (15) is newer than ihslib's copy of the protocol, where the caps only
    // have supports_video_hevc, which current Steam marks obsolete. Repeated, unpacked: one
    // unknown field per codec, the preferred one first.
    uint8_t h264 = k_EStreamVideoCodecH264, hevc = k_EStreamVideoCodecHEVC;
    ProtobufCMessageUnknownField codecFields[2] = {
            {15, PROTOBUF_C_WIRE_TYPE_VARINT, 1, &hevc},
            {15, PROTOBUF_C_WIRE_TYPE_VARINT, 1, &h264},
    };
    if (ihsConf.enableHevc) {
        clientCaps.base.n_unknown_fields = 2;
        clientCaps.base.unknown_fields = codecFields;
    } else {
        clientCaps.base.n_unknown_fields = 1;
        clientCaps.base.unknown_fields = &codecFields[1];
    }

    CNegotiationSetConfigMsg response = CNEGOTIATION_SET_CONFIG_MSG__INIT;
    response.config = &config;
    response.streaming_client_config = &clientConfig;
    response.streaming_client_caps = &clientCaps;

    IHS_SessionChannelControlSend(channel, k_EStreamControlNegotiationSetConfig,
                                  (const ProtobufCMessage *) &response, IHS_PACKET_ID_NEXT);
}

static void OnNegotiationSetConfig(IHS_SessionChannel *channel, const CNegotiationSetConfigMsg *message,
                                   uint16_t packetId) {
    CNegotiationCompleteMsg response = CNEGOTIATION_COMPLETE_MSG__INIT;
    IHS_SessionChannelControlSend(channel, k_EStreamControlNegotiationComplete,
                                  (const ProtobufCMessage *) &response, IHS_PACKET_ID_NEXT);
    OnConnected(channel);
}

static void OnConnected(IHS_SessionChannel *channel) {
    IHS_Session *session = channel->session;
    session->state.connectionState = IHS_SessionConnectionStateConnected;
    IHS_SessionChannelControlStartHeartbeat(channel);
    if (session->callbacks.session && session->callbacks.session->connected) {
        session->callbacks.session->connected(session, session->callbackContexts.session);
    }
}
