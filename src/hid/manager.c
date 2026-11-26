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
#include <assert.h>
#include "manager.h"
#include "device.h"
#include "provider.h"
#include "session/channels/ch_control.h"
#include "protobuf/pb_utils.h"
#include "session/session_pri.h"

/* 250 Hz. Every poll sends only what changed, so a faster poll adds little traffic, and a
 * button press waits 2 ms on average for it instead of 4. */
#define HID_POLL_INTERVAL_MS 4u

/* A full report this often. The delta reports are reliable, but the host can still end up with a
 * wrong state (a retransmission given up on, a delta whose CRC the host rejected, a stall that
 * ended with a button release lost), and nothing else tells us. A full report is 48 bytes plus
 * framing, so once a second is free, and it caps how long a stuck button can last. Counted in
 * poll ticks, not read from a clock: the timer thread is the only user. */
#define HID_FULL_REPORT_TICKS (1000u / HID_POLL_INTERVAL_MS)

static uint64_t HIDPollTick(int runCount, void *context);

IHS_HIDManager *IHS_HIDManagerCreate() {
    IHS_HIDManager *manager = calloc(1, sizeof(IHS_HIDManager));
    IHS_ArrayListInit(&manager->providers, sizeof(IHS_HIDProvider *));
    IHS_ArrayListInit(&manager->devices, sizeof(IHS_HIDManagedDevice *));
    IHS_ArrayListInit(&manager->inputReports, sizeof(IHS_HIDDeviceReportMessage *));
    manager->devicesLock = IHS_MutexCreate();
    manager->workerLock = IHS_MutexCreate();
    manager->workerCond = IHS_CondCreate();
    return manager;
}

void IHS_HIDManagerDestroy(IHS_HIDManager *manager) {
    // Jobs look devices up and answer through the session, so they must be gone before either.
    IHS_HIDManagerStopWorker(manager);
    if (manager->pollTimer != NULL) {
        IHS_TimerTaskStop(manager->pollTimer);
        manager->pollTimer = NULL;
    }
    // Defensively close anything still open. Discovery deinit normally runs CloseAll, but
    // tests / abnormal teardown paths may skip that; the second CloseAll is a no-op once
    // every device is marked closed.
    IHS_HIDManagerCloseAll(manager);
    for (size_t i = 0, j = manager->providers.size; i < j; ++i) {
        IHS_HIDProvider *provider = *((IHS_HIDProvider **) IHS_ArrayListGet(&manager->providers, i));
        assert(provider->manager == manager);
        provider->manager = NULL;
    }
    // Every slot holds a managed device that's already been through ManagedDeviceClose
    // (so the inner IHS_HIDDevice is gone). Reclaim the outer struct + its lock + holder.
    for (size_t i = 0, j = manager->devices.size; i < j; ++i) {
        IHS_HIDManagedDevice *managed = *((IHS_HIDManagedDevice **) IHS_ArrayListGet(&manager->devices, i));
        assert(managed->closed);
        IHS_HIDReportHolderDeinit(&managed->reportHolder);
        IHS_MutexDestroy(managed->lock);
        IHS_MutexDestroy(managed->useLock);
        free(managed);
    }
    IHS_ArrayListDeinit(&manager->devices);
    IHS_ArrayListDeinit(&manager->providers);
    IHS_ArrayListDeinit(&manager->inputReports);
    IHS_MutexDestroy(manager->devicesLock);
    IHS_CondDestroy(manager->workerCond);
    IHS_MutexDestroy(manager->workerLock);
    free(manager);
}

void IHS_HIDManagerCloseAll(IHS_HIDManager *manager) {
    // ManagedDeviceClose grabs per-device locks and re-enters the manager via
    // RemoveClosedDevice (which itself takes devicesLock). Snapshot under the list lock,
    // release it, then close each device — keeps the critical section tiny and avoids
    // lock-order entanglement with the per-device lock.
    size_t count;
    IHS_HIDManagedDevice **snapshot = IHS_HIDManagerSnapshotOpenDevices(manager, &count);
    for (size_t i = 0; i < count; ++i) {
        IHS_HIDManagedDeviceClose(snapshot[i]);
    }
    free(snapshot);
}

IHS_HIDManagedDevice *IHS_HIDManagerOpenDevice(IHS_HIDManager *manager, const char *path) {
    // Provider open callbacks can block on OS calls, so they run without devicesLock held;
    // only the list append at the end is locked.
    IHS_HIDDevice *device = NULL;
    for (size_t i = 0, j = manager->providers.size; i < j; ++i) {
        IHS_HIDProvider *provider = *((IHS_HIDProvider **) IHS_ArrayListGet(&manager->providers, i));
        if (!IHS_HIDProviderSupportsDevice(provider, path)) continue;
        device = IHS_HIDProviderOpenDevice(provider, path);
        if (device != NULL) break;
    }
    if (device == NULL) return NULL;
    IHS_HIDManagedDevice *managed = calloc(1, sizeof(IHS_HIDManagedDevice));
    managed->manager = manager;
    managed->device = device;
    managed->lock = IHS_MutexCreate();
    managed->useLock = IHS_MutexCreate();
    managed->pollable = device->cls->poll != NULL;
    IHS_MutexLock(manager->devicesLock);
    managed->id = ++manager->lastDeviceId;
    IHS_HIDReportHolderInit(&managed->reportHolder, managed->id);
    IHS_ArrayListAppend(&manager->devices, &managed);
    IHS_MutexUnlock(manager->devicesLock);
    device->managed = managed;
    IHS_HIDManagedDeviceOpened(managed);
    return managed;
}

IHS_HIDManagedDevice *IHS_HIDManagerFindDeviceByID(IHS_HIDManager *manager, uint32_t id) {
    // IDs are assigned monotonically under devicesLock, so the list stays sorted — but
    // the closed flag means we can't use a plain binary search (a closed slot with the
    // right ID would match and we'd hand back a dead pointer). Linear scan from the end
    // finds recent devices fastest and naturally skips closed entries.
    IHS_MutexLock(manager->devicesLock);
    IHS_HIDManagedDevice *result = NULL;
    for (int i = (int) manager->devices.size - 1; i >= 0; --i) {
        IHS_HIDManagedDevice *managed = *((IHS_HIDManagedDevice **) IHS_ArrayListGet(&manager->devices, i));
        if (managed->closed) continue;
        if (managed->id == id) {
            result = managed;
            break;
        }
    }
    IHS_MutexUnlock(manager->devicesLock);
    return result;
}

IHS_HIDManagedDevice *IHS_HIDManagerFindDevice(IHS_HIDManager *manager, IHS_HIDDeviceComparator predicate,
                                               const void *value) {
    IHS_MutexLock(manager->devicesLock);
    IHS_HIDManagedDevice *result = NULL;
    for (size_t i = 0, j = manager->devices.size; i < j; ++i) {
        IHS_HIDManagedDevice *managed = *((IHS_HIDManagedDevice **) IHS_ArrayListGet(&manager->devices, i));
        if (managed->closed) continue;
        if (predicate(value, (const IHS_HIDDevice **) &managed->device) == 0) {
            result = managed;
            break;
        }
    }
    IHS_MutexUnlock(manager->devicesLock);
    return result;
}

IHS_HIDManagedDevice **IHS_HIDManagerSnapshotOpenDevices(IHS_HIDManager *manager, size_t *count) {
    IHS_MutexLock(manager->devicesLock);
    size_t total = manager->devices.size;
    IHS_HIDManagedDevice **out = total > 0 ? calloc(total, sizeof(IHS_HIDManagedDevice *)) : NULL;
    size_t n = 0;
    for (size_t i = 0; i < total; ++i) {
        IHS_HIDManagedDevice *managed = *((IHS_HIDManagedDevice **) IHS_ArrayListGet(&manager->devices, i));
        if (!managed->closed) out[n++] = managed;
    }
    IHS_MutexUnlock(manager->devicesLock);
    *count = n;
    return out;
}

bool IHS_HIDManagerNotifyDeviceClosed(IHS_HIDManager *manager, IHS_HIDManagedDevice *managed) {
    IHS_SessionChannel *channel = IHS_SessionChannelForType(manager->session, IHS_SessionChannelTypeControl);
    CHIDMessageFromRemote message = CHIDMESSAGE_FROM_REMOTE__INIT;
    CHIDMessageFromRemote__CloseDevice closeDevice = CHIDMESSAGE_FROM_REMOTE__CLOSE_DEVICE__INIT;
    PROTOBUF_C_SET_VALUE(closeDevice, device, managed->id);
    message.command_case = CHIDMESSAGE_FROM_REMOTE__COMMAND_CLOSE_DEVICE;
    message.close_device = &closeDevice;
    IHS_SessionLog(manager->session, IHS_LogLevelDebug, "HID", "Close device, id=%u", managed->id);
    return IHS_SessionChannelControlSendHIDMsg(channel, &message);
}

void IHS_HIDManagerRemoveClosedDevice(IHS_HIDManager *manager, IHS_HIDManagedDevice *managed) {
    // Deferred-free model: flip `closed` under the list lock so concurrent Find* callers
    // stop returning this slot, but leave the IHS_HIDManagedDevice * itself valid. Any
    // thread that already obtained the pointer before the flag flipped continues to see
    // live memory; the slot is reclaimed in IHS_HIDManagerDestroy. The inner IHS_HIDDevice
    // is freed by the caller (IHS_HIDManagedDeviceClose) right after this returns —
    // callers using `managed->device` racing against close is a separate, pre-existing
    // hazard that this change does not address.
    IHS_SessionLog(manager->session, IHS_LogLevelDebug, "HID", "MarkDeviceClosed, id=%u", managed->id);
    IHS_MutexLock(manager->devicesLock);
    managed->closed = true;
    IHS_MutexUnlock(manager->devicesLock);
}

void IHS_HIDManagerAddProvider(IHS_HIDManager *manager, IHS_HIDProvider *provider) {
    provider->manager = manager;
    IHS_ArrayListAppend(&manager->providers, &provider);
    if (manager->pollTimer == NULL && manager->session != NULL) {
        // Lazy-start the periodic poll task on the first provider so sessions that never
        // use HID pay no wakeup cost.
        manager->pollTimer = IHS_TimerTaskStart(manager->session->timers, HIDPollTick, NULL,
                                                HID_POLL_INTERVAL_MS, manager);
    }
}

void IHS_HIDManagerRemoveProvider(IHS_HIDManager *manager, IHS_HIDProvider *provider) {
    bool removed = IHS_ArrayListRemoveFirst(&manager->providers, &provider);
    provider->manager = NULL;
    assert(removed);
}

// Mirrors CHIDDeviceReportThread::Run (0x20c8d0) → RunHIDDeviceReportThread (0x21d4c8):
// every tick, poll each device that implements `poll`. If anything returned data, batch
// into one SendReport call. Devices whose poll returns negative are dead — close them.
//
// Snapshot the device list under devicesLock and iterate the snapshot, so concurrent
// Open/Close on the SDL event thread or session worker can't race with the poll walk
// and so the slow poll callback runs without devicesLock held.
static uint64_t HIDPollTick(int runCount, void *context) {
    (void) runCount;
    IHS_HIDManager *manager = context;
    // If input streaming is disabled, skip the device drain entirely. Otherwise the holders
    // would accumulate raw reports that IHS_SessionHIDSendReport refuses to flush, growing
    // the per-device dataBuffer without bound. The timer keeps ticking so we resume
    // immediately when the server flips streamingInput back on.
    if (!IHS_SessionInputEnabled(manager->session)) {
        return HID_POLL_INTERVAL_MS;
    }
    size_t count;
    IHS_HIDManagedDevice **snapshot = IHS_HIDManagerSnapshotOpenDevices(manager, &count);
    bool anyData = false;
    // Once a second every device becomes due for a full report. A device that is busy (a request
    // in flight) stays due and is retried on the next tick, on its own: the others must not get
    // another report every 4 ms meanwhile.
    if (++manager->fullReportTicks >= HID_FULL_REPORT_TICKS) {
        manager->fullReportTicks = 0;
        for (size_t i = 0; i < count; ++i) {
            snapshot[i]->resyncPending = true;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        IHS_HIDManagedDevice *managed = snapshot[i];
        bool wantsResync = managed->resyncPending;
        if (!managed->pollable && !wantsResync) continue;
        // useLock keeps the SDL event thread (device removed) and the HID worker (Close request)
        // from freeing the device while we use it. Try only: this is the timer thread.
        IHS_HIDTryAcquire acquired = IHS_HIDManagerTryAcquireDevice(managed);
        if (acquired == IHS_HIDTryAcquireBusy) continue;
        if (acquired == IHS_HIDTryAcquireClosed) {
            managed->resyncPending = false;
            continue;
        }
        managed->resyncPending = false;
        int result = 0;
        if (managed->pollable) {
            IHS_HIDDeviceLock(managed->device);
            result = managed->device->cls->poll(managed->device);
            IHS_HIDDeviceUnlock(managed->device);
        }
        if (result > 0) {
            anyData = true;
        }
        if (result >= 0 && wantsResync) {
            IHS_MutexLock(managed->lock);
            // Only after the host sent StartInputReports (it sets the length): before that it is
            // not reading this device, and AddFull asserts the length covers the 48-byte state.
            bool started = managed->reportHolder.reportLength >= 48;
            IHS_MutexUnlock(managed->lock);
            if (started && IHS_HIDDeviceRequestFullReport(managed->device) == 0) {
                IHS_SessionCounterAdd(manager->session->counters.hidResyncReports, 1);
                anyData = true;
            }
        }
        IHS_HIDManagerReleaseDevice(managed);
        if (result < 0) {
            IHS_SessionLog(manager->session, IHS_LogLevelInfo, "HID",
                           "Device id=%u poll failed (%d), closing", managed->id, result);
            // After the release: Close takes the lock itself.
            IHS_HIDManagedDeviceClose(managed);
        }
    }
    free(snapshot);
    if (anyData) {
        IHS_SessionHIDSendReport(manager->session);
    }
    return HID_POLL_INTERVAL_MS;
}

typedef struct IHS_HIDJob {
    struct IHS_HIDJob *next;
    IHS_HIDJobFunction *function;
    void *job;
} IHS_HIDJob;

static void HIDWorkerRun(void *context) {
    IHS_HIDManager *manager = context;
    IHS_MutexLock(manager->workerLock);
    while (!manager->workerStop) {
        IHS_HIDJob *node = manager->jobsHead;
        if (node == NULL) {
            IHS_CondWait(manager->workerCond, manager->workerLock);
            continue;
        }
        manager->jobsHead = node->next;
        if (manager->jobsHead == NULL) {
            manager->jobsTail = NULL;
        }
        // Unlocked while it runs: a read may wait its whole timeout, and Defer must not wait for it.
        IHS_MutexUnlock(manager->workerLock);
        node->function(manager, node->job);
        free(node->job);
        free(node);
        IHS_MutexLock(manager->workerLock);
    }
    IHS_MutexUnlock(manager->workerLock);
}

bool IHS_HIDManagerDefer(IHS_HIDManager *manager, IHS_HIDJobFunction *function, void *job) {
    IHS_HIDJob *node = calloc(1, sizeof(IHS_HIDJob));
    if (node == NULL) {
        return false;
    }
    node->function = function;
    node->job = job;
    IHS_MutexLock(manager->workerLock);
    // Lazily started, like the poll timer: sessions without HID requests pay for no thread.
    if (manager->workerStop || (manager->worker == NULL &&
                                (manager->worker = IHS_ThreadCreate(HIDWorkerRun, "IHSHid", manager)) == NULL)) {
        IHS_MutexUnlock(manager->workerLock);
        free(node);
        return false;
    }
    if (manager->jobsTail != NULL) {
        manager->jobsTail->next = node;
    } else {
        manager->jobsHead = node;
    }
    manager->jobsTail = node;
    IHS_CondSignal(manager->workerCond);
    IHS_MutexUnlock(manager->workerLock);
    return true;
}

void IHS_HIDManagerStopWorker(IHS_HIDManager *manager) {
    IHS_MutexLock(manager->workerLock);
    manager->workerStop = true;
    IHS_Thread *worker = manager->worker;
    manager->worker = NULL;
    IHS_CondSignal(manager->workerCond);
    IHS_MutexUnlock(manager->workerLock);
    // Outside the lock: the running job may still be going through it. A second caller finds
    // worker == NULL and returns at once, which is fine because Destroy only follows a first one
    // that already joined.
    if (worker != NULL) {
        IHS_ThreadJoin(worker);
    }
    IHS_MutexLock(manager->workerLock);
    IHS_HIDJob *node = manager->jobsHead;
    manager->jobsHead = manager->jobsTail = NULL;
    IHS_MutexUnlock(manager->workerLock);
    while (node != NULL) {
        IHS_HIDJob *next = node->next;
        free(node->job);
        free(node);
        node = next;
    }
}

bool IHS_HIDManagerWorkerStopping(IHS_HIDManager *manager) {
    IHS_MutexLock(manager->workerLock);
    bool stopping = manager->workerStop;
    IHS_MutexUnlock(manager->workerLock);
    return stopping;
}

bool IHS_HIDManagerAcquireDevice(IHS_HIDManagedDevice *managed) {
    IHS_MutexLock(managed->useLock);
    // Read under useLock: Close sets it with useLock held, so it cannot flip while we hold it.
    if (managed->closed) {
        IHS_MutexUnlock(managed->useLock);
        return false;
    }
    return true;
}

IHS_HIDManagedDevice *IHS_HIDManagerAcquireDeviceByID(IHS_HIDManager *manager, uint32_t id) {
    IHS_HIDManagedDevice *managed = IHS_HIDManagerFindDeviceByID(manager, id);
    // The struct outlives the device (slots are reclaimed in Destroy), so waiting on its lock is safe.
    if (managed == NULL || !IHS_HIDManagerAcquireDevice(managed)) {
        return NULL;
    }
    return managed;
}

void IHS_HIDManagerReleaseDevice(IHS_HIDManagedDevice *managed) {
    IHS_MutexUnlock(managed->useLock);
}

IHS_HIDTryAcquire IHS_HIDManagerTryAcquireDevice(IHS_HIDManagedDevice *managed) {
    if (!IHS_MutexTryLock(managed->useLock)) {
        return IHS_HIDTryAcquireBusy;
    }
    if (managed->closed) {
        IHS_MutexUnlock(managed->useLock);
        return IHS_HIDTryAcquireClosed;
    }
    return IHS_HIDTryAcquireOK;
}
