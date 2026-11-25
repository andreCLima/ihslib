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

#include "ihslib/hid.h"
#include "ihs_arraylist.h"
#include "ihs_thread.h"
#include "ihs_timer.h"
#include "report.h"

typedef struct IHS_HIDDevice IHS_HIDDevice;

typedef struct IHS_HIDManagedDevice IHS_HIDManagedDevice;

struct IHS_HIDManager {
    IHS_Session *session;
    /**
     * Stores `IHS_HIDManagedDevice *` (not the struct). Each slot owns one managed device
     * for the lifetime of the manager: once a device is closed its slot's `closed` flag
     * flips true, but the slot stays in place so any concurrent holder of the pointer it
     * returned from a Find* call sees stable memory. All reads/writes of this list are
     * protected by `devicesLock`.
     */
    IHS_ArrayList devices;
    IHS_Mutex *devicesLock;
    IHS_ArrayList providers;
    IHS_ArrayList inputReports;
    uint32_t lastDeviceId;
    /**
     * 125 Hz poll task that drains every device whose class implements `poll`, then calls
     * IHS_SessionHIDSendReport once if anything was added. Created lazily on the first
     * IHS_HIDManagerAddProvider call; destroyed in IHS_HIDManagerDestroy.
     */
    IHS_TimerTask *pollTimer;
    /**
     * Poll ticks since the last periodic full report. Only the poll task touches it.
     */
    uint32_t fullReportTicks;
    /**
     * Worker for HID requests that can block (reads with a timeout, feature reports that retry),
     * so they do not hold up the thread that receives packets. Jobs run in the order they were
     * deferred. Started on the first IHS_HIDManagerDefer; stopped by IHS_HIDManagerStopWorker.
     */
    IHS_Thread *worker;
    IHS_Mutex *workerLock;
    IHS_Cond *workerCond;
    struct IHS_HIDJob *jobsHead;
    struct IHS_HIDJob *jobsTail;
    bool workerStop;
};

struct IHS_HIDManagedDevice {
    IHS_HIDDevice *device;
    IHS_HIDManager *manager;
    uint32_t id;
    IHS_HIDReportHolder reportHolder;
    IHS_Mutex *lock;
    /**
     * Held by anyone using `device` (and by Close while it frees it), so it cannot be freed under
     * them. Take it with IHS_HIDManagerAcquireDevice*; lock order is useLock, then lock.
     */
    IHS_Mutex *useLock;
    /**
     * Set true by IHS_HIDManagerRemoveClosedDevice. Find* skip closed slots; the slot
     * (and this struct) live until IHS_HIDManagerDestroy reclaims them — that keeps every
     * IHS_HIDManagedDevice * stable for the lifetime of the manager.
     */
    bool closed;
    /**
     * Whether the provider implements `poll`, kept here because reading it off `device` needs the
     * device to be alive.
     */
    bool pollable;
    /**
     * A full report is due for this device and has not been produced yet (it was busy). Only the
     * poll task touches it.
     */
    bool resyncPending;
};


/**
 * @param value Value to compare
 * @param device Pointer to the entry in devices list
 * @return 0 if matched
 */
typedef int(*IHS_HIDDeviceComparator)(const void *value, const IHS_HIDDevice **device);

IHS_HIDManager *IHS_HIDManagerCreate();

void IHS_HIDManagerDestroy(IHS_HIDManager *manager);

void IHS_HIDManagerCloseAll(IHS_HIDManager *manager);

IHS_HIDManagedDevice *IHS_HIDManagerOpenDevice(IHS_HIDManager *manager, const char *path);

IHS_HIDManagedDevice *IHS_HIDManagerFindDeviceByID(IHS_HIDManager *manager, uint32_t id);

IHS_HIDManagedDevice *IHS_HIDManagerFindDevice(IHS_HIDManager *manager, IHS_HIDDeviceComparator predicate,
                                               const void *value);

bool IHS_HIDManagerNotifyDeviceClosed(IHS_HIDManager *manager, IHS_HIDManagedDevice *managed);

void IHS_HIDManagerRemoveClosedDevice(IHS_HIDManager *manager, IHS_HIDManagedDevice *managed);

void IHS_HIDManagerAddProvider(IHS_HIDManager *manager, IHS_HIDProvider *provider);

void IHS_HIDManagerRemoveProvider(IHS_HIDManager *manager, IHS_HIDProvider *provider);

/**
 * Walk the device list under `devicesLock` and copy every still-open managed device
 * pointer into a freshly-malloc'd array. Returns NULL with `*count = 0` when there are
 * no open devices. Caller owns the returned array and must `free()` it.
 *
 * This is the only sanctioned way to iterate devices from outside `manager.c` — direct
 * traversal of `manager->devices` would race with concurrent Open/Close on other threads.
 */
IHS_HIDManagedDevice **IHS_HIDManagerSnapshotOpenDevices(IHS_HIDManager *manager, size_t *count);


/**
 * Runs on the HID worker thread. Owns nothing: the manager frees \p job after it returns.
 */
typedef void (IHS_HIDJobFunction)(IHS_HIDManager *manager, void *job);

/**
 * Queue \p job (malloc'd, freed by the manager with free()) to run \p function on the HID worker,
 * after every job deferred before it.
 * @return false if the worker is stopped or could not start; the job is then NOT taken, and the
 * caller frees it.
 */
bool IHS_HIDManagerDefer(IHS_HIDManager *manager, IHS_HIDJobFunction *function, void *job);

/**
 * Stop the worker: the running job finishes, queued ones are dropped, and later Defer calls fail.
 * Must run before the control channel is destroyed, since jobs answer through it. Idempotent.
 */
void IHS_HIDManagerStopWorker(IHS_HIDManager *manager);

/**
 * @return true once StopWorker was called. A job that loops can check it to give up early.
 */
bool IHS_HIDManagerWorkerStopping(IHS_HIDManager *manager);

/**
 * Find an open device by id and hold its useLock, so it cannot be closed (and its IHS_HIDDevice
 * freed) until IHS_HIDManagerReleaseDevice. Blocks while another thread is inside it.
 * @return NULL if there is no such open device; nothing is held then
 */
IHS_HIDManagedDevice *IHS_HIDManagerAcquireDeviceByID(IHS_HIDManager *manager, uint32_t id);

/**
 * Hold the useLock of a device found by other means (a snapshot).
 * @return false if it was closed; nothing is held then
 */
bool IHS_HIDManagerAcquireDevice(IHS_HIDManagedDevice *managed);

void IHS_HIDManagerReleaseDevice(IHS_HIDManagedDevice *managed);

typedef enum IHS_HIDTryAcquire {
    IHS_HIDTryAcquireOK,
    IHS_HIDTryAcquireBusy,
    IHS_HIDTryAcquireClosed,
} IHS_HIDTryAcquire;

/**
 * Like IHS_HIDManagerAcquireDevice but never waits: for the timer thread, which also runs the
 * retransmissions and must not sit behind a read that waits out its timeout. Held only on OK.
 */
IHS_HIDTryAcquire IHS_HIDManagerTryAcquireDevice(IHS_HIDManagedDevice *managed);
