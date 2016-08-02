/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cpu.h>
#include <heap.h>
#include <sensors.h>
#include <sensors_priv.h>
#include <seos.h>
#include <seos_priv.h>
#include <syscall.h>
#include <timer.h>
#include <util.h>

#include <chre.h>
#include <chreApi.h>

/*
 * This is to ensure that message size and some extra headers will stay representable with 1 byte
 * Code relies on that in many places.
 */
C_STATIC_ASSERT(max_chre_msg_size, CHRE_MESSAGE_TO_HOST_MAX_SIZE <= 240);

/*
 * Many syscalls rely on the property that uintptr_t can hold uint32_t without data loss
 * This is enforced by static assertion in chreApi.h
 * None of the methods returning uint32_t are cast to uintptr_t
 * This is done in order to let compiler warn us if our assumption is not safe for some reason
 */
void osChreTaskHandle(struct Task *task, uint32_t evtType, const void *evtData)
{
    uint16_t evt = evtType;
    if (evt < EVT_FIRST_CHRE_USER_EVENT && evt >= EVT_FIRST_CHRE_SYS_EVENT) {
        evt -= EVT_FIRST_CHRE_SYS_EVENT;
    } else if (evt >= EVT_FIRST_CHRE_USER_EVENT) {
        evt = evt - EVT_FIRST_CHRE_USER_EVENT + CHRE_EVENT_FIRST_USER_VALUE;
    }
    evtType = evt | (task->tid << 16);
    cpuAppHandle(task->app, &task->platInfo, evtType, evtData);
}

static inline uint64_t osChreGetAppId(void)
{
    struct Task *task = osGetCurrentTask();
    const struct AppHdr *app = task ? task->app : NULL;

    return app ? app->hdr.appId : 0;
}

static void osChreApiGetAppId(uintptr_t *retValP, va_list args)
{
    uint64_t *appId = va_arg(args, uint64_t *);
    if (appId)
        *appId = osChreGetAppId();
}

static void osChreApiGetInstanceId(uintptr_t *retValP, va_list args)
{
    *retValP = osGetCurrentTid();
}

static void osChreApiLogLogv(uintptr_t *retValP, va_list args)
{
    va_list innerArgs;
    enum chreLogLevel level = va_arg(args, int /* enums promoted to ints in va_args in C */);
    const static char levels[] = "EWIDV";
    char clevel = level > CHRE_LOG_DEBUG || level < 0 ? 'V' : levels[level];
    const char *str = va_arg(args, const char*);
    uintptr_t inner = va_arg(args, uintptr_t);

    va_copy(innerArgs, INTEGER_TO_VA_LIST(inner));
    osLogv(clevel, str, innerArgs);
    va_end(innerArgs);
}

static void osChreApiGetTime(uintptr_t *retValP, va_list args)
{
    uint64_t *timeNanos = va_arg(args, uint64_t *);
    if (timeNanos)
        *timeNanos = timGetTime();
}

static inline uint32_t osChreTimerSet(uint64_t duration, const void* cookie, bool oneShot)
{
    uint32_t timId = timTimerSetNew(duration, cookie, oneShot);

    return timId == 0 ? CHRE_TIMER_INVALID : timId;
}

static void osChreApiTimerSet(uintptr_t *retValP, va_list args)
{
    uint32_t length_lo = va_arg(args, uint32_t);
    uint32_t length_hi = va_arg(args, uint32_t);
    void *cookie = va_arg(args, void *);
    bool oneshot = va_arg(args, int);
    uint64_t length = (((uint64_t)length_hi) << 32) | length_lo;

    *retValP = osChreTimerSet(length, cookie, oneshot);
}

static void osChreApiTimerCancel(uintptr_t *retValP, va_list args)
{
    uint32_t timerId = va_arg(args, uint32_t);
    *retValP = timTimerCancelEx(timerId, true);
}

static inline void osChreAbort(uint32_t abortCode)
{
    struct Task *task = osGetCurrentTask();
    osLog(LOG_ERROR, "APP ID=0x%" PRIX64 "TID=0x%" PRIX16 " aborted [code 0x%" PRIX32 "]",
          task->app->hdr.appId, task->tid, abortCode);
    osTaskAbort(task);
}

static void osChreApiAbort(uintptr_t *retValP, va_list args)
{
    uint32_t code = va_arg(args, uint32_t);
    osChreAbort(code);
}

static void osChreApiHeapAlloc(uintptr_t *retValP, va_list args)
{
    uint32_t size = va_arg(args, uint32_t);
    *retValP = (uintptr_t)heapAlloc(size);
}

static void osChreApiHeapFree(uintptr_t *retValP, va_list args)
{
    void *ptr = va_arg(args, void *);
    heapFree(ptr);
}

static bool osChreSendEvent(uint16_t evtType, void *evtData,
                            chreEventCompleteFunction *evtFreeCallback,
                            uint32_t toTid)
{
    uint32_t evt;
    if (evtType >= CHRE_EVENT_FIRST_USER_VALUE) {
        evt = evtType - CHRE_EVENT_FIRST_USER_VALUE + EVT_FIRST_CHRE_USER_EVENT;
        if (evt >= EVT_DEBUG_LOG) {
            osLog(LOG_INFO, "%s: CHRE User Event %04" PRIX16 " is not compatible with nanohub", __func__, evtType);
            return false;
        }
    } else if (evtType < MAX_CHRE_SYS_EVENTS) {
        evt = evtType + EVT_FIRST_CHRE_SYS_EVENT;
    } else {
        osLog(LOG_INFO, "%s: CHRE System Event %04" PRIX16 " is not compatible with nanohub", __func__, evtType);
        return false;
    }
    return osEnqueuePrivateEvtNew(evt, evtData, evtFreeCallback, toTid);
}

static void osChreApiSendEvent(uintptr_t *retValP, va_list args)
{
    uint16_t evtType = va_arg(args, uint32_t); // stored as 32-bit
    void *evtData = va_arg(args, void *);
    chreEventCompleteFunction *freeCallback = va_arg(args, chreEventCompleteFunction *);
    uint32_t toTid = va_arg(args, uint32_t);
    *retValP = osChreSendEvent(evtType, evtData, freeCallback, toTid);
}

static bool osChreSendMessageToHost(void *message, uint32_t messageSize,
                           uint32_t reservedMessageType,
                           chreMessageFreeFunction *freeCallback)
{
    bool result = false;
    struct HostHubRawPacket *hostMsg = NULL;

    if (messageSize > CHRE_MESSAGE_TO_HOST_MAX_SIZE || (messageSize && !message))
        goto out;

    hostMsg = heapAlloc(sizeof(*hostMsg) + messageSize);
    if (!hostMsg)
        goto out;

    if (messageSize)
        memcpy(hostMsg+1, message, messageSize);

    hostMsg->appId = osChreGetAppId();
    hostMsg->dataLen = messageSize;
    result = osEnqueueEvtOrFree(EVT_APP_TO_HOST, hostMsg, heapFree);

out:
    if (freeCallback && message && messageSize)
        osTaskInvokeMessageFreeCallback(osGetCurrentTask(), freeCallback, message, messageSize);
    return result;
}

static void osChreApiSendMessageToHost(uintptr_t *retValP, va_list args)
{
    void *message = va_arg(args, void *);
    uint32_t messageSize = va_arg(args, uint32_t);
    uint32_t reservedMessageType = va_arg(args, uint32_t);
    chreMessageFreeFunction *freeCallback = va_arg(args, chreMessageFreeFunction *);

    *retValP = osChreSendMessageToHost(message, messageSize, reservedMessageType, freeCallback);
}

static bool osChreSensorFindDefault(uint8_t sensorType, uint32_t *pHandle)
{
    if (!pHandle)
        return false;

    const struct SensorInfo *info = sensorFind(sensorType, 0, pHandle);

    return info != NULL;
}

static void osChreApiSensorFindDefault(uintptr_t *retValP, va_list args)
{
    uint8_t sensorType = va_arg(args, uint32_t);
    uint32_t *pHandle = va_arg(args, uint32_t *);
    *retValP = osChreSensorFindDefault(sensorType, pHandle);
}

static bool osChreSensorGetInfo(uint32_t sensorHandle, struct chreSensorInfo *info)
{
    struct Sensor *s = sensorFindByHandle(sensorHandle);
    if (!s || !info)
        return false;
    const struct SensorInfo *si = s->si;
    info->sensorName = si->sensorName;
    info->sensorType = si->sensorType;
    info->unusedFlags = 0;

    // TODO: check if the two lines below deliver intended semantics
    info->isOnChange = s->hasOnchange;
    info->isOneShot = s->hasOndemand;

    return true;
}

static void osChreApiSensorGetInfo(uintptr_t *retValP, va_list args)
{
    uint32_t sensorHandle = va_arg(args, uint32_t);
    struct chreSensorInfo *info = va_arg(args, struct chreSensorInfo *);
    *retValP = osChreSensorGetInfo(sensorHandle, info);
}

static bool osChreSensorGetSamplingStatus(uint32_t sensorHandle,
                                 struct chreSensorSamplingStatus *status)
{
    struct Sensor *s = sensorFindByHandle(sensorHandle);
    if (!s || !status)
        return false;

    // TODO
    status->interval = 0;
    status->latency = 0;
    status->enabled = 0;
    return true;
}

static void osChreApiSensorGetStatus(uintptr_t *retValP, va_list args)
{
    uint32_t sensorHandle = va_arg(args, uint32_t);
    struct chreSensorSamplingStatus *status = va_arg(args, struct chreSensorSamplingStatus *);
    *retValP = osChreSensorGetSamplingStatus(sensorHandle, status);
}

static bool osChreSensorConfigure(uint32_t sensorHandle,
                         enum chreSensorConfigureMode mode,
                         uint64_t interval, uint64_t latency)
{
    // TODO
    osLog(LOG_ERROR, "%s: not implemented\n", __func__);
    return false;
}

static void osChreApiSensorConfig(uintptr_t *retValP, va_list args)
{
    uint32_t sensorHandle = va_arg(args, uint32_t);
    enum chreSensorConfigureMode mode = va_arg(args, int);
    uint64_t interval = va_arg(args, uint32_t);
    uint32_t interval_hi = va_arg(args, uint32_t);
    uint64_t latency = va_arg(args, uint32_t);
    uint32_t latency_hi = va_arg(args, uint32_t);

    interval |= ((uint64_t)interval_hi) << 32;
    latency  |= ((uint64_t)latency_hi) << 32;

    *retValP = osChreSensorConfigure(sensorHandle, mode, interval, latency);
}

static uint32_t osChreGetApiVersion(void)
{
    return CHRE_API_VERSION;
}

static void osChreApiChreApiVersion(uintptr_t *retValP, va_list args)
{
    *retValP = osChreGetApiVersion();
}

static uint32_t osChreGetVersion(void)
{
    return CHRE_API_VERSION | NANOHUB_OS_PATCH_LEVEL;
}

static void osChreApiChreOsVersion(uintptr_t *retValP, va_list args)
{
    *retValP = (uintptr_t)osChreGetVersion();
}

static uint64_t osChreGetPlatformId(void)
{
    return HW_ID_MAKE(NANOHUB_VENDOR_GOOGLE, 0);
}

static void osChreApiPlatformId(uintptr_t *retValP, va_list args)
{
    uint64_t *pHwId = va_arg(args, uint64_t*);
    if (pHwId)
        *pHwId = osChreGetPlatformId();
}

static const struct SyscallTable chreMainApiTable = {
    .numEntries = SYSCALL_CHRE_MAIN_API_LAST,
    .entry = {
        [SYSCALL_CHRE_MAIN_API_LOG]                     = { .func = osChreApiLogLogv },
        [SYSCALL_CHRE_MAIN_API_GET_APP_ID]              = { .func = osChreApiGetAppId },
        [SYSCALL_CHRE_MAIN_API_GET_INST_ID]             = { .func = osChreApiGetInstanceId },
        [SYSCALL_CHRE_MAIN_API_GET_TIME]                = { .func = osChreApiGetTime },
        [SYSCALL_CHRE_MAIN_API_TIMER_SET]               = { .func = osChreApiTimerSet },
        [SYSCALL_CHRE_MAIN_API_TIMER_CANCEL]            = { .func = osChreApiTimerCancel },
        [SYSCALL_CHRE_MAIN_API_ABORT]                   = { .func = osChreApiAbort },
        [SYSCALL_CHRE_MAIN_API_HEAP_ALLOC]              = { .func = osChreApiHeapAlloc },
        [SYSCALL_CHRE_MAIN_API_HEAP_FREE]               = { .func = osChreApiHeapFree },
        [SYSCALL_CHRE_MAIN_API_SEND_EVENT]              = { .func = osChreApiSendEvent },
        [SYSCALL_CHRE_MAIN_API_SEND_MSG]                = { .func = osChreApiSendMessageToHost },
        [SYSCALL_CHRE_MAIN_API_SENSOR_FIND_DEFAULT]     = { .func = osChreApiSensorFindDefault },
        [SYSCALL_CHRE_MAIN_API_SENSOR_GET_INFO]         = { .func = osChreApiSensorGetInfo },
        [SYSCALL_CHRE_MAIN_API_SENSOR_GET_STATUS]       = { .func = osChreApiSensorGetStatus },
        [SYSCALL_CHRE_MAIN_API_SENSOR_CONFIG]           = { .func = osChreApiSensorConfig },
        [SYSCALL_CHRE_MAIN_API_GET_OS_API_VERSION]      = { .func = osChreApiChreApiVersion },
        [SYSCALL_CHRE_MAIN_API_GET_OS_VERSION]          = { .func = osChreApiChreOsVersion },
        [SYSCALL_CHRE_MAIN_API_GET_PLATFORM_ID]         = { .func = osChreApiPlatformId },
    },
};

static const struct SyscallTable chreMainTable = {
    .numEntries = SYSCALL_CHRE_MAIN_LAST,
    .entry = {
        [SYSCALL_CHRE_MAIN_API]     = { .subtable = (struct SyscallTable*)&chreMainApiTable,     },
    },
};

static const struct SyscallTable chreTable = {
    .numEntries = SYSCALL_CHRE_LAST,
    .entry = {
        [SYSCALL_CHRE_MAIN]    = { .subtable = (struct SyscallTable*)&chreMainTable,    },
    },
};

void osChreApiExport()
{
    if (!syscallAddTable(SYSCALL_NO(SYSCALL_DOMAIN_CHRE,0,0,0), 1, (struct SyscallTable*)&chreTable))
            osLog(LOG_ERROR, "Failed to export CHRE OS API");
}