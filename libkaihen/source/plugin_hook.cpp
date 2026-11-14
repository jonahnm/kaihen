// SPDX-License-Identifier: ISC
// HEN sends one HenFsHookEvent per phase on the plugin's callback session.
// The handler runs on this thread, in the plugin process, so it can use the
// plugin's services. A handler that asks for the original returns to HEN,
// which lets the FS bridge stub call the trampoline and then send the AFTER
// event.
#include "plugin_hook.hpp"
#include "cmif_server.hpp"

#include <cstring>

namespace kaihen {
namespace {

using ounce::closeHandle;

constexpr u32 MaxHandlers = HEN_MAX_PLUGINS;
constexpr size_t ThreadStackSize = 0x4000;
constexpr int ThreadPriority = 0x2c;

constexpr s32 Wait_Stop = 0;
constexpr s32 Wait_Session = 1;

struct HandlerEntry {
    bool used = false;
    u32 id = 0;
    HenFsHookHandler handler = nullptr;
    void* user = nullptr;
};

HandlerEntry g_handlers[MaxHandlers] = {};
Mutex g_handler_lock = {};
u32 g_next_handler_id = 1;

Handle g_session = INVALID_HANDLE;
Handle g_stop_write = INVALID_HANDLE;
Handle g_stop_read = INVALID_HANDLE;
Thread g_thread = {};
bool g_started = false;

bool isHookCall(const ounce::CmifRequest& request) {
    const u32 type = request.hipc.meta.type;
    return request.valid && ounce::hasOnlyInlineData(request) &&
        (type == CmifCommandType_Request || type == CmifCommandType_RequestWithContext) &&
        request.command_id == HEN_PLUGIN_HOOK_CALL &&
        request.data_size >= sizeof(HenFsHookEvent);
}

Result findHandler(u32 id, HenFsHookHandler* out_handler, void** out_user) {
    mutexLock(&g_handler_lock);
    Result rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    for (const HandlerEntry& entry : g_handlers) {
        if (entry.used && entry.id == id) {
            *out_handler = entry.handler;
            *out_user = entry.user;
            rc = 0;
            break;
        }
    }
    mutexUnlock(&g_handler_lock);
    return rc;
}

/// Serves a request on the callback session. Returns a failure only when the
/// session itself failed.
Result serveHookRequest() {
    void* message = armGetTls();
    const ounce::CmifRequest request = ounce::parseRequest(message);

    if (ounce::isQueryPointerBufferSize(request)) {
        ounce::writePointerBufferSizeReply(message);
        return ounce::sendReply(g_session);
    }

    if (!isHookCall(request)) {
        ounce::closeRequestHandles(request);
        ounce::writeReply(message,
                          MAKERESULT(Module_Libnx, LibnxError_BadInput), nullptr, 0);
        return ounce::sendReply(g_session);
    }

    HenFsHookEvent event = {};
    std::memcpy(&event, request.data, sizeof(event));
    ounce::closeRequestHandles(request);

    HenFsHookReply reply = {};
    HenFsHookHandler handler = nullptr;
    void* user = nullptr;
    const Result lookup = findHandler(event.handler_id, &handler, &user);
    if (R_SUCCEEDED(lookup) && (event.phase == HEN_FS_HOOK_BEFORE ||
                                event.phase == HEN_FS_HOOK_AFTER)) {
        HenFsHookContext context = {};
        context.hook_id = event.hook_id;
        context.handler_id = event.handler_id;
        context.phase = event.phase;
        context.action = HEN_FS_HOOK_RETURN;
        context.user = user;
        std::memcpy(context.regs, event.regs, sizeof(context.regs));

        const Result rc = handler(&context);
        reply.result = rc;
        if (R_SUCCEEDED(rc)) {
            std::memcpy(reply.regs, context.regs, sizeof(reply.regs));
            reply.action = event.phase == HEN_FS_HOOK_BEFORE &&
                    context.action == HEN_FS_HOOK_CALL_ORIGINAL
                ? HEN_FS_HOOK_CALL_ORIGINAL
                : HEN_FS_HOOK_RETURN;
        } else {
            // A failed handler must leave the original call intact.
            std::memcpy(reply.regs, event.regs, sizeof(reply.regs));
            reply.action = event.phase == HEN_FS_HOOK_BEFORE
                ? HEN_FS_HOOK_CALL_ORIGINAL
                : HEN_FS_HOOK_RETURN;
        }
    } else {
        reply.result = R_SUCCEEDED(lookup)
            ? MAKERESULT(Module_Libnx, LibnxError_BadInput)
            : lookup;
        std::memcpy(reply.regs, event.regs, sizeof(reply.regs));
        reply.action = event.phase == HEN_FS_HOOK_BEFORE
            ? HEN_FS_HOOK_CALL_ORIGINAL
            : HEN_FS_HOOK_RETURN;
    }

    ounce::writeReply(message, 0, &reply, sizeof(reply));
    return ounce::sendReply(g_session);
}

Result runHookServer() {
    for (;;) {
        if (g_session == INVALID_HANDLE)
            return 0;
        const Handle handles[] = {g_stop_read, g_session};
        s32 index = -1;
        Result rc = svcReplyAndReceive(&index, handles, 2, INVALID_HANDLE, UINT64_MAX);
        if (index == Wait_Stop && R_SUCCEEDED(rc))
            return 0;
        if (rc == KERNELRESULT(ConnectionClosed)) {
            g_session = INVALID_HANDLE;
            return 0;
        }
        if (R_FAILED(rc))
            return rc;
        if (index != Wait_Session)
            continue;

        if (hipcParseRequest(armGetTls()).meta.type == CmifCommandType_Close) {
            g_session = INVALID_HANDLE;
            return 0;
        }
        rc = serveHookRequest();
        if (rc == KERNELRESULT(ConnectionClosed)) {
            g_session = INVALID_HANDLE;
            return 0;
        }
        if (R_FAILED(rc))
            return rc;
    }
}

void hookThread(void*) {
    runHookServer();
}

} // namespace

Result RegisterHookHandler(HenFsHookHandler handler, void* user, u32* out_id) {
    if (!handler || !out_id || !g_started)
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    Result rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    mutexLock(&g_handler_lock);
    for (HandlerEntry& entry : g_handlers) {
        if (entry.used)
            continue;
        entry.used = true;
        entry.id = g_next_handler_id++;
        if (!g_next_handler_id)
            g_next_handler_id = 1;
        entry.handler = handler;
        entry.user = user;
        *out_id = entry.id;
        rc = 0;
        break;
    }
    mutexUnlock(&g_handler_lock);
    return rc;
}

void UnregisterHookHandler(u32 id) {
    mutexLock(&g_handler_lock);
    for (HandlerEntry& entry : g_handlers) {
        if (entry.used && entry.id == id) {
            entry = {};
            break;
        }
    }
    mutexUnlock(&g_handler_lock);
}

Result StartHookServer(Handle session) {
    if (!session || session == INVALID_HANDLE)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (g_started)
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);

    g_session = session;
    mutexInit(&g_handler_lock);
    Result rc = svcCreateEvent(&g_stop_write, &g_stop_read);
    if (R_FAILED(rc))
        return rc;

    rc = threadCreate(&g_thread, hookThread, nullptr, nullptr, ThreadStackSize,
                      ThreadPriority, -2);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&g_thread);
        if (R_FAILED(rc))
            threadClose(&g_thread);
    }
    if (R_FAILED(rc)) {
        closeHandle(&g_stop_write);
        closeHandle(&g_stop_read);
        return rc;
    }

    g_started = true;
    return 0;
}

void StopHookServer() {
    if (!g_started)
        return;
    svcSignalEvent(g_stop_write);
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    closeHandle(&g_stop_write);
    closeHandle(&g_stop_read);
    g_started = false;
    g_session = INVALID_HANDLE;
}

bool HookServerRunning() {
    return g_started && g_session != INVALID_HANDLE;
}

} // namespace kaihen
