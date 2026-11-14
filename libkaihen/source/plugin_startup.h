// SPDX-License-Identifier: ISC
// The bootstrap's side of the plugin handoff (plugin_runtime.cpp has
// the plugin's side).
#pragma once

#include <switch.h>

enum PluginStartupCommand : u32 {
    PluginStartupCommand_AdoptHandles = 1,
    /// Returns a COPY of the plugin's hook callback client handle. The
    /// plugin keeps serving the server end.
    PluginStartupCommand_GetHookSession = 2,
};

/// The version of the plugin runtime's AdoptHandles reply. A later version
/// may add data or handles after the current ones, but keeps these.
/// Versions 0 and 1 did not exist as such: the first runtime replied with a
/// "publishes a service" flag in this word, and sent the third handle only
/// when the flag was 1. Version 3's handoff may also carry the plugin's HEN
/// hook session. Version 4 adds the GetHookSession command; AdoptHandles
/// still replies with the same three handles.
constexpr u32 PluginRuntimeVersion = 4;

/// Sends the plugin copies of the SM session, the FS root and, when HEN could
/// create one, its hook session. The reply has the release event (writable),
/// the ready event (readable) and the client end of the plugin's control
/// session, which is INVALID_HANDLE when the plugin does not publish a
/// service. The caller owns the returned handles.
NX_INLINE Result pluginAdoptHandles(Service* plugin, Handle sm, Handle fs_root,
                                    Handle hook_session, u32* out_runtime_version,
                                    Handle* release_event, Handle* ready_event,
                                    Handle* control_session) {
    Handle handles[3] = {INVALID_HANDLE, INVALID_HANDLE, INVALID_HANDLE};
    u32 runtime_version = 0;
    const Result rc = hook_session != INVALID_HANDLE
        ? serviceDispatchOut(plugin, PluginStartupCommand_AdoptHandles, runtime_version,
            .in_num_handles = 3,
            .in_handles = {sm, fs_root, hook_session},
            .out_handle_attrs = {SfOutHandleAttr_HipcCopy, SfOutHandleAttr_HipcCopy, SfOutHandleAttr_HipcCopy},
            .out_handles = handles,
        )
        : serviceDispatchOut(plugin, PluginStartupCommand_AdoptHandles, runtime_version,
            .in_num_handles = 2,
            .in_handles = {sm, fs_root},
            .out_handle_attrs = {SfOutHandleAttr_HipcCopy, SfOutHandleAttr_HipcCopy, SfOutHandleAttr_HipcCopy},
            .out_handles = handles,
        );
    if (R_FAILED(rc))
        return rc;

    // The first runtime's reply has two handles when the flag is 0, so the
    // third value read is not a handle.
    if (runtime_version == 0)
        handles[2] = INVALID_HANDLE;

    *out_runtime_version = runtime_version;
    *release_event = handles[0];
    *ready_event = handles[1];
    *control_session = handles[2];
    return 0;
}

/// Asks a runtime version 4 plugin for a copy of its hook callback client
/// endpoint. The caller owns the returned handle.
NX_INLINE Result pluginGetHookSession(Service* plugin, Handle* out) {
    return serviceDispatch(plugin, PluginStartupCommand_GetHookSession,
        .out_handle_attrs = {SfOutHandleAttr_HipcCopy},
        .out_handles = out,
    );
}
