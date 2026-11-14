// SPDX-License-Identifier: ISC
// Startup for an kaiHEN plugin (include/kaihen/plugin.h).
//
// The plugin registers the temporary service mig:usr. The bootstrap connects
// to it and sends AdoptHandles with copies of its SM session and FS root, and
// of a HEN session that only plugins receive (the FS hook interface) when HEN
// provided one. The reply has the runtime version (PluginRuntimeVersion) and
// three handles: a release event, a ready event and the client end of the
// plugin's control session, or INVALID_HANDLE for a plugin without a service.
//
// The plugin also creates a callback session pair. Its server end serves FS
// hook events; the bootstrap asks for a copy of the client end with
// GetHookSession and gives it to HEN. The bootstrap signals the release event
// only after that, which is why the plugin keeps serving mig:usr until then.
#include <switch.h>

#include "../include/kaihen/plugin.h"
#include "plugin_hook.hpp"
#include "plugin_startup.h"
#include "cmif_server.hpp"

extern "C" {

// A plugin that initializes FS starts with one session. Its own strong
// definition can select a larger pool when it needs concurrent FS calls.
__attribute__((weak)) u32 __nx_fs_num_sessions = 1;

}

namespace {

using ounce::closeHandle;

constexpr u32 MigrationServiceSpace = 1;
constexpr u64 ReleaseTimeoutNs = 5000000000ull;
constexpr u32 MaxInstalledHooks = HEN_MAX_PLUGINS;

/// The handles that the plugin keeps from the bootstrap's AdoptHandles.
struct ReceivedHandles {
    Handle sm_session = INVALID_HANDLE;
    Handle fs_root = INVALID_HANDLE;
    Handle hook_session = INVALID_HANDLE;    ///< HEN's plugin-only FS hook interface.
    Handle callback_server = INVALID_HANDLE; ///< The plugin's end of the handler callback pair.
    Handle ready_event = INVALID_HANDLE;     ///< Writable end; the plugin signals it.
    Handle control_session = INVALID_HANDLE; ///< Server end, when the plugin publishes a service.
};

/// The bootstrap's ends of the objects that the AdoptHandles reply creates.
/// The reply copies them, so the plugin closes these after sending it.
struct AdoptReply {
    Handle release_event = INVALID_HANDLE;  ///< Writable end.
    Handle ready_event = INVALID_HANDLE;    ///< Readable end.
    Handle control_client = INVALID_HANDLE; ///< When the plugin publishes a service.

    void close() {
        closeHandle(&release_event);
        closeHandle(&ready_event);
        closeHandle(&control_client);
    }
};

struct InstalledHook {
    u32 id = 0;
    u32 handler_id = 0;
};

ReceivedHandles g_received;
Handle g_hook_client = INVALID_HANDLE;
InstalledHook g_installed[MaxInstalledHooks] = {};
u32 g_installed_count = 0;
bool g_publish_service = false;
bool g_initialized = false;

void closeReceivedHandles() {
    closeHandle(&g_received.control_session);
    closeHandle(&g_received.hook_session);
    closeHandle(&g_received.callback_server);
    closeHandle(&g_hook_client);
    closeHandle(&g_received.ready_event);
    closeHandle(&g_received.fs_root);
    closeHandle(&g_received.sm_session);
}

bool isAdoptHandles(const ounce::CmifRequest& request) {
    const HipcMetadata& meta = request.hipc.meta;
    return request.valid && meta.type == CmifCommandType_Request &&
        request.command_id == PluginStartupCommand_AdoptHandles &&
        (meta.num_copy_handles == 2 || meta.num_copy_handles == 3) &&
        !meta.num_move_handles;
}

bool isGetHookSession(const ounce::CmifRequest& request) {
    return request.valid && request.hipc.meta.type == CmifCommandType_Request &&
        request.command_id == PluginStartupCommand_GetHookSession &&
        ounce::hasOnlyInlineData(request);
}

/// Creates the events, the control session, and the callback session pair
/// that the handoff needs. `release_event` receives the readable end; the
/// plugin keeps the server end of the callback pair in g_received.
Result createAdoptObjects(AdoptReply* reply, Handle* release_event) {
    Result rc = svcCreateEvent(&reply->release_event, release_event);
    if (R_FAILED(rc))
        return rc;

    rc = svcCreateEvent(&g_received.ready_event, &reply->ready_event);
    if (R_FAILED(rc))
        goto fail_release_event;

    if (g_publish_service) {
        rc = svcCreateSession(&g_received.control_session, &reply->control_client, false, 0);
        if (R_FAILED(rc))
            goto fail_ready_event;
    }

    rc = svcCreateSession(&g_received.callback_server, &g_hook_client, false, 0);
    if (R_FAILED(rc))
        goto fail_control_session;
    return 0;

fail_control_session:
    closeHandle(&g_received.control_session);
    closeHandle(&reply->control_client);
fail_ready_event:
    closeHandle(&g_received.ready_event);
    closeHandle(&reply->ready_event);
fail_release_event:
    closeHandle(release_event);
    closeHandle(&reply->release_event);
    return rc;
}

/// Adopts the SM session and FS root of an AdoptHandles request and writes
/// the reply. On success, `reply` holds the bootstrap's ends to close after
/// sending it.
Result adoptHandles(void* message, const ounce::CmifRequest& request,
                    AdoptReply* reply, Handle* release_event) {
    Result rc = MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    if (g_received.sm_session == INVALID_HANDLE)
        rc = createAdoptObjects(reply, release_event);

    if (R_FAILED(rc)) {
        ounce::closeRequestHandles(request);
        ounce::writeReply(message, rc, nullptr, 0);
        return rc;
    }

    g_received.sm_session = request.hipc.data.copy_handles[0];
    g_received.fs_root = request.hipc.data.copy_handles[1];
    if (request.hipc.meta.num_copy_handles == 3)
        g_received.hook_session = request.hipc.data.copy_handles[2];

    // The kernel passes an INVALID_HANDLE in the copy list through unchanged,
    // so a plugin without a service sends one in the third place.
    const u32 runtime_version = PluginRuntimeVersion;
    const Handle handles[] = {reply->release_event, reply->ready_event, reply->control_client};
    ounce::writeReply(message, 0, &runtime_version, sizeof(runtime_version), handles, 3);
    return 0;
}

/// Serves one request on the mig:usr session. After AdoptHandles, the
/// bootstrap asks for the hook callback client; the runtime keeps serving
/// until the bootstrap signals release.
Result serveBootstrapRequest(Handle session, Handle* release_event) {
    void* message = armGetTls();
    const ounce::CmifRequest request = ounce::parseRequest(message);
    AdoptReply reply;
    Result rc;
    bool sent_hook_client = false;

    if (ounce::isQueryPointerBufferSize(request)) {
        ounce::writePointerBufferSizeReply(message);
        rc = 0;
    } else if (isAdoptHandles(request)) {
        rc = adoptHandles(message, request, &reply, release_event);
    } else if (isGetHookSession(request)) {
        if (g_hook_client == INVALID_HANDLE) {
            rc = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
            ounce::closeRequestHandles(request);
            ounce::writeReply(message, rc, nullptr, 0);
        } else {
            rc = 0;
            ounce::writeReply(message, 0, nullptr, 0, &g_hook_client, 1);
            sent_hook_client = true;
        }
    } else {
        rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        ounce::closeRequestHandles(request);
        ounce::writeReply(message, rc, nullptr, 0);
    }

    const Result send_rc = ounce::sendReply(session);
    reply.close();
    if (sent_hook_client)
        closeHandle(&g_hook_client);
    return R_FAILED(send_rc) ? send_rc : rc;
}

/// Accepts the bootstrap's connection to mig:usr and serves it until the
/// bootstrap has AdoptHandles, asks for the hook callback client, and
/// signals release. The caller closes the session and the release event.
Result receiveBootstrapHandles(Handle port, Handle* out_session,
                               Handle* out_release_event) {
    Handle session = INVALID_HANDLE;
    Handle release = INVALID_HANDLE;
    for (;;) {
        Handle handles[3] = {port, INVALID_HANDLE, INVALID_HANDLE};
        s32 session_index = -1;
        s32 release_index = -1;
        s32 count = 1;
        if (session != INVALID_HANDLE) {
            session_index = count;
            handles[count++] = session;
        }
        if (release != INVALID_HANDLE) {
            release_index = count;
            handles[count++] = release;
        }

        s32 index = -1;
        const u64 timeout = release != INVALID_HANDLE ? ReleaseTimeoutNs : UINT64_MAX;
        Result rc = svcReplyAndReceive(&index, handles, count, INVALID_HANDLE, timeout);
        if (R_FAILED(rc)) {
            // The bootstrap signals release and then closes its end. If the
            // kernel reports the close first, the handoff still succeeded.
            if (rc == KERNELRESULT(ConnectionClosed) && release != INVALID_HANDLE &&
                R_SUCCEEDED(svcWaitSynchronizationSingle(release, ReleaseTimeoutNs))) {
                *out_session = session;
                *out_release_event = release;
                return 0;
            }
            *out_session = session;
            *out_release_event = release;
            return rc;
        }

        if (index == 0) {
            rc = svcAcceptSession(&session, port);
            if (R_FAILED(rc)) {
                *out_session = session;
                *out_release_event = release;
                return rc;
            }
            continue;
        }

        if (index == release_index) {
            *out_session = session;
            *out_release_event = release;
            return 0;
        }

        if (index == session_index) {
            rc = serveBootstrapRequest(session, &release);
            if (R_FAILED(rc)) {
                *out_session = session;
                *out_release_event = release;
                return rc;
            }
        }
    }
}

/// Registers mig:usr, receives the bootstrap's handles, and unregisters
/// mig:usr once the bootstrap signals that it has them.
Result receiveThroughMigrationService() {
    Handle port = INVALID_HANDLE;
    Result rc = smRegisterServiceInSpace(&port, MigrationServiceSpace,
                                         smEncodeName("mig:usr"), false, 1);
    if (R_FAILED(rc))
        return rc;

    Handle session = INVALID_HANDLE;
    Handle release_event = INVALID_HANDLE;
    rc = receiveBootstrapHandles(port, &session, &release_event);
    closeHandle(&session);
    closeHandle(&release_event);

    if (R_SUCCEEDED(rc))
        rc = smUnregisterServiceInSpace(MigrationServiceSpace, smEncodeName("mig:usr"));
    svcCloseHandle(port);
    return rc;
}

/// Installs the bootstrap's SM session and FS root for later service use.
/// The hook callback thread is optional: a plugin without a working thread
/// still runs, but its hook install API returns NotInitialized.
Result installReceivedServices() {
    smAddOverrideHandle(smEncodeName("sm:"), g_received.sm_session);
    smAddOverrideHandle(smEncodeName("fsp-srv"), g_received.fs_root);
    Result rc = smInitialize();
    if (R_FAILED(rc))
        return rc;

    if (R_FAILED(kaihen::StartHookServer(g_received.callback_server))) {
        closeHandle(&g_received.callback_server);
        closeHandle(&g_hook_client);
    }
    return 0;
}

InstalledHook* findInstalledHook(u32 id) {
    for (u32 index = 0; index < g_installed_count; index++) {
        if (g_installed[index].id == id)
            return &g_installed[index];
    }
    return nullptr;
}

} // namespace

Result pluginRuntimeInitialize(Handle* out_ready_event) {
    if (!out_ready_event)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Result rc = smInitialize();
    if (R_FAILED(rc))
        return rc;

    rc = receiveThroughMigrationService();
    // mig:usr was registered with the plugin's own SM session. Close it
    // before the bootstrap's session replaces it.
    smExit();

    if (R_SUCCEEDED(rc))
        rc = installReceivedServices();
    if (R_FAILED(rc)) {
        closeReceivedHandles();
        return rc;
    }

    g_initialized = true;
    *out_ready_event = g_received.ready_event;
    return 0;
}

namespace {

/// A service on HEN's plugin-only session. The caller does not close it: the
/// runtime owns the handle from the handoff.
Result openHookService(Service* out) {
    if (g_received.hook_session == INVALID_HANDLE)
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    serviceCreate(out, g_received.hook_session);
    out->own_handle = false;
    return 0;
}

} // namespace

Result pluginFsHookInstall(u64 target_offset, HenFsHookHandler handler,
                           void* user, HenFsHookInfo* out) {
    if (!out || !handler)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (!kaihen::HookServerRunning())
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (g_installed_count == MaxInstalledHooks)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    u32 handler_id = 0;
    Result rc = kaihen::RegisterHookHandler(handler, user, &handler_id);
    if (R_FAILED(rc))
        return rc;

    Service hook = {};
    rc = openHookService(&hook);
    if (R_SUCCEEDED(rc)) {
        const HenFsHookRequest request = {target_offset, handler_id, 0, 0};
        rc = serviceDispatchInOut(&hook, HenPluginCommand_InstallFsHook, request, *out);
    }
    if (R_FAILED(rc)) {
        kaihen::UnregisterHookHandler(handler_id);
        return rc;
    }

    g_installed[g_installed_count++] = {out->id, handler_id};
    return 0;
}

Result pluginFsHookRemove(u32 id) {
    InstalledHook* installed = findInstalledHook(id);
    if (!installed)
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    Service hook = {};
    Result rc = openHookService(&hook);
    if (R_SUCCEEDED(rc))
        rc = serviceDispatchIn(&hook, HenPluginCommand_RemoveFsHook, id);
    if (R_FAILED(rc))
        return rc;

    const u32 index = static_cast<u32>(installed - g_installed);
    kaihen::UnregisterHookHandler(installed->handler_id);
    for (u32 next = index; next + 1 < g_installed_count; next++)
        g_installed[next] = g_installed[next + 1];
    g_installed[--g_installed_count] = {};
    return 0;
}

Result pluginFsHookCount(u32* out_count) {
    if (!out_count)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    Service hook = {};
    Result rc = openHookService(&hook);
    if (R_SUCCEEDED(rc))
        rc = serviceDispatchOut(&hook, HenPluginCommand_GetFsHookCount, *out_count);
    return rc;
}

Result pluginFsHookInfo(u32 index, HenFsHookInfo* out) {
    if (!out)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    Service hook = {};
    Result rc = openHookService(&hook);
    if (R_SUCCEEDED(rc))
        rc = serviceDispatchInOut(&hook, HenPluginCommand_GetFsHookInfo, index, *out);
    return rc;
}

Result pluginRuntimeInitializeWithService(Handle* out_ready_event,
                                          Handle* out_control_session) {
    if (!out_control_session)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    g_publish_service = true;
    Result rc = pluginRuntimeInitialize(out_ready_event);
    if (R_SUCCEEDED(rc))
        *out_control_session = g_received.control_session;
    else
        g_publish_service = false;
    return rc;
}

void pluginRuntimeExit(void) {
    kaihen::StopHookServer();
    if (g_initialized) {
        smExit();
        g_initialized = false;
    }
    closeReceivedHandles();
    g_publish_service = false;
}
