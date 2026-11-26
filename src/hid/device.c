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
#include <assert.h>
#include "device.h"
#include "manager.h"
#include "session/session_pri.h"

IHS_HIDDevice *IHS_HIDDeviceCreate(const IHS_HIDDeviceClass *cls) {
    IHS_HIDDevice *device = cls->alloc(cls);
    assert(device->cls == cls);
    return device;
}

void IHS_HIDManagedDeviceOpened(IHS_HIDManagedDevice *managed) {
    assert(managed->manager != NULL);
    IHS_HIDDevice *device = managed->device;
    if (device->cls->opened != NULL) {
        device->cls->opened(device);
    }
}

void IHS_HIDManagedDeviceClose(IHS_HIDManagedDevice *managed) {
    assert(managed != NULL);
    assert(managed->manager != NULL);
    // useLock is held by whoever is inside the device (request worker, poll task, resync), so
    // the device is never freed under them. `closed` is only changed with it held, which makes
    // a second Close (event thread racing a Close request from the host) a no-op.
    IHS_MutexLock(managed->useLock);
    if (managed->closed) {
        IHS_MutexUnlock(managed->useLock);
        return;
    }
    IHS_HIDDevice *device = managed->device;
    device->cls->close(device);
    IHS_HIDManagerNotifyDeviceClosed(managed->manager, managed);
    IHS_HIDManagerRemoveClosedDevice(managed->manager, managed);
    device->managed = NULL;
    device->cls->free(device);
    IHS_MutexUnlock(managed->useLock);
}

int IHS_HIDDeviceWrite(IHS_HIDDevice *device, const uint8_t *data, size_t dataLen) {
    assert(device->cls->write != NULL);
    return device->cls->write(device, data, dataLen);
}

int IHS_HIDDeviceRead(IHS_HIDDevice *device, IHS_Buffer *dest, size_t length, uint32_t timeoutMs) {
    assert(device->cls->read != NULL);
    return device->cls->read(device, dest, length, timeoutMs);
}

int IHS_HIDDeviceSendFeatureReport(IHS_HIDDevice *device, const uint8_t *data, size_t dataLen) {
    return device->cls->sendFeatureReport(device, data, dataLen);
}

int IHS_HIDDeviceGetFeatureReport(IHS_HIDDevice *device, const uint8_t *reportNumber, size_t reportNumberLen,
                                  IHS_Buffer *dest, size_t length) {
    return device->cls->getFeatureReport(device, reportNumber, reportNumberLen, dest, length);
}

int IHS_HIDDeviceGetVendorString(IHS_HIDDevice *device, IHS_Buffer *out) {
    return device->cls->getVendorString(device, out);
}

int IHS_HIDDeviceGetProductString(IHS_HIDDevice *device, IHS_Buffer *out) {
    return device->cls->getProductString(device, out);
}

int IHS_HIDDeviceGetSerialNumberString(IHS_HIDDevice *device, IHS_Buffer *out) {
    return device->cls->getSerialNumberString(device, out);
}

int IHS_HIDDeviceStartInputReports(IHS_HIDDevice *device, size_t length) {
    return device->cls->startInputReports(device, length);
}

int IHS_HIDDeviceRequestFullReport(IHS_HIDDevice *device) {
    // Optional: a provider that cannot produce one has nothing to resync.
    if (device->cls->requestFullReport == NULL) return -1;
    return device->cls->requestFullReport(device);
}

int IHS_HIDDeviceRequestDisconnect(IHS_HIDDevice *device, int method, const uint8_t *data, size_t dataLen) {
    return device->cls->requestDisconnect(device, method, data, dataLen);
}

/* A report was produced for this device and waits for the next send. Counted for the stats, and the
 * time of the first one still waiting is kept so a send that never comes can be told apart from
 * input that never happened. (A report the dedup dropped is not counted: it never needed sending.) */
static void HIDReportQueued(IHS_HIDDevice *device) {
    IHS_HIDManager *manager = device->managed->manager;
    if (manager == NULL || manager->session == NULL) return;
    IHS_SessionCounterAdd(manager->session->counters.hidReportsGenerated, 1);
    uint32_t expected = 0;
    __atomic_compare_exchange_n(&manager->firstUnsentMs, &expected, (uint32_t) IHS_TimerNow() | 1u, false,
                                __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

void IHS_HIDDeviceReportAddFull(IHS_HIDDevice *device, const uint8_t *current, size_t len) {
    size_t before = device->managed->reportHolder.reportItems.size;
    IHS_HIDReportHolderAddFull(&device->managed->reportHolder, current, len);
    if (device->managed->reportHolder.reportItems.size > before) HIDReportQueued(device);
}

void IHS_HIDDeviceReportAddDelta(IHS_HIDDevice *device, const uint8_t *previous, const uint8_t *current, size_t len) {
    size_t before = device->managed->reportHolder.reportItems.size;
    IHS_HIDReportHolderAddDelta(&device->managed->reportHolder, previous, current, len);
    if (device->managed->reportHolder.reportItems.size > before) HIDReportQueued(device);
}

void IHS_HIDDeviceLock(IHS_HIDDevice *device) {
    assert(device->managed != NULL);
    IHS_MutexLock(device->managed->lock);
}

void IHS_HIDDeviceUnlock(IHS_HIDDevice *device) {
    assert(device->managed != NULL);
    IHS_MutexUnlock(device->managed->lock);
}

IHS_Session *IHS_HIDDeviceGetSession(IHS_HIDDevice *device) {
    return device->managed->manager->session;
}