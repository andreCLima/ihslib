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

/*
 * The control channel's receive window, fed the way Steam feeds it on Wi-Fi: most packets come
 * again after we have polled them, and some arrive out of order. Each id must come out once, in
 * order, with its own payload, and the ACK id must reach the newest packet.
 */

#include <assert.h>
#include <string.h>

#include "session/window.h"

#define CAPACITY 128
#define PACKETS 5000

static bool Add(IHS_SessionPacketsWindow *window, uint16_t id) {
    IHS_SessionPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.header.type = IHS_SessionPacketTypeReliable;
    packet.header.packetId = id;
    IHS_BufferInit(&packet.body, 8, 8);
    IHS_BufferAppendMem(&packet.body, (const uint8_t *) &id, sizeof(id));
    bool ok = IHS_SessionPacketsWindowAdd(window, &packet);
    /* Ignored packets keep their body. */
    IHS_BufferClear(&packet.body, true);
    return ok;
}

static uint16_t Contiguous(const IHS_SessionPacketsWindow *window) {
    uint16_t id = 0;
    assert(IHS_SessionPacketsWindowContiguousId(window, &id));
    return id;
}

/* Polls everything ready; checks it continues from *next and advances it. */
static void Drain(IHS_SessionPacketsWindow *window, uint16_t *next) {
    IHS_SessionFrame frame;
    IHS_BufferInit(&frame.body, 16, 1024);
    for (; IHS_SessionPacketsWindowPoll(window, &frame); IHS_SessionPacketsWindowReleaseFrame(&frame)) {
        uint16_t payload;
        assert(frame.body.size == sizeof(payload));
        memcpy(&payload, IHS_BufferPointer(&frame.body), sizeof(payload));
        assert(frame.header.packetId == *next);
        assert(payload == *next);
        (*next)++;
    }
    IHS_BufferClear(&frame.body, true);
}

/* Every packet comes twice, the second time after it was polled. */
static void ResendAfterPoll(void) {
    IHS_SessionPacketsWindow *window = IHS_SessionPacketsWindowCreate(CAPACITY);
    uint16_t next = 0;
    for (uint16_t id = 0; id < PACKETS; id++) {
        assert(Add(window, id));
        assert(Contiguous(window) == id);
        Drain(window, &next);
        assert(Add(window, id));
        assert(Contiguous(window) == id);
        if (id > 0) {
            assert(Add(window, (uint16_t) (id - 1)));
        }
        Drain(window, &next);
        assert(next == (uint16_t) (id + 1));
    }
    IHS_SessionPacketsWindowDestroy(window);
}

/* Pairs arrive swapped, and the early one comes again while the hole is open. */
static void OutOfOrder(void) {
    IHS_SessionPacketsWindow *window = IHS_SessionPacketsWindowCreate(CAPACITY);
    uint16_t next = 0;
    /* The first packet sets where the window starts. */
    assert(Add(window, 0));
    Drain(window, &next);
    for (uint16_t id = 1; id < PACKETS; id += 2) {
        assert(Add(window, (uint16_t) (id + 1)));
        assert(Contiguous(window) == (uint16_t) (id - 1));
        assert(Add(window, (uint16_t) (id + 1)));
        /* The packet just before the hole, polled already. */
        assert(Add(window, (uint16_t) (id - 1)));
        Drain(window, &next);
        assert(next == id);
        assert(Add(window, id));
        assert(Contiguous(window) == (uint16_t) (id + 1));
        Drain(window, &next);
        assert(next == (uint16_t) (id + 2));
        assert(Add(window, id));
        assert(Add(window, (uint16_t) (id + 1)));
        Drain(window, &next);
        assert(next == (uint16_t) (id + 2));
    }
    IHS_SessionPacketsWindowDestroy(window);
}

int main(void) {
    ResendAfterPoll();
    OutOfOrder();
    return 0;
}
