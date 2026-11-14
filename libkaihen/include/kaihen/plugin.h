/* SPDX-License-Identifier: ISC
 * Copyright 2026 kaiHEN contributors
 *
 * Startup for an kaiHEN plugin: a native program (NSO) that the bootstrap
 * starts as its own process before HEN.
 *
 * At startup, the bootstrap reads every /kaihen/plugins/<name>.nso on the
 * SD card, packages it with the migration program's policy, and starts it.
 * Then it hands the plugin an initialized SM session and the FS root through
 * a temporary mig:usr service. pluginRuntimeInitialize() receives those
 * handles and installs them as libounce service overrides, so that
 * smGetService() works afterwards. Plugins that need FS call fsInitialize()
 * and fsExit() themselves; plugins without FS work need no FS sessions.
 * The runtime defaults __nx_fs_num_sessions to 1. A plugin may define a
 * strong value before linking to select a larger pool.
 *
 * The bootstrap waits up to 5 seconds for the ready event that
 * pluginRuntimeInitialize() returns. Signal the event (svcSignalEvent) once
 * the plugin's own setup is done. A plugin that is not ready in time keeps
 * running, but HEN does not list its service. The bootstrap skips a plugin
 * whose file name is too long or whose package it cannot write. It stops
 * startup, without HEN, when starting a plugin process or handing it the
 * services fails. /kaihen/plugin-status.txt says what happened to each
 * plugin.
 *
 * A plugin has no services before pluginRuntimeInitialize(), so it replaces
 * libounce's application setup and sets a heap of its own:
 *
 *     u32 __nx_applet_type = AppletType_None;
 *     size_t __nx_heap_size = 0x200000;
 *     void __appInit(void) {}
 *     void __appExit(void) {}
 *
 *     int main(void) {
 *         Handle ready;
 *         Result rc = pluginRuntimeInitialize(&ready);
 *         if (R_FAILED(rc))
 *             return rc;
 *         // ... set up the plugin, then:
 *         svcSignalEvent(ready);
 *         // ... the plugin's work ...
 *         pluginRuntimeExit();
 *         return 0;
 *     }
 *
 * Link with -lkaihen_plugin -lounce_core (libounce without mimalloc: its
 * fixed costs do not fit a heap of a few MiB), and convert the ELF to an NSO
 * with elf2nso. The devkit's examples/plugin-echo builds one.
 */
#pragma once

#include <switch.h>
#include "hook.h"
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Receives the SM session and FS root from the bootstrap and makes
 *        them the program's services.
 * @param[out] out_ready_event Event to signal when the plugin is ready. It
 *        belongs to the runtime until pluginRuntimeExit().
 */
Result pluginRuntimeInitialize(Handle* out_ready_event);

/**
 * @brief Initializes a plugin and publishes an IPC session factory through HEN.
 * @param[out] out_ready_event Signal after the plugin's own setup completes.
 * @param[out] out_control_session Borrowed server session. Serve requests to
 *        create client sessions until pluginRuntimeExit; do not close it separately.
 * @note Plugins that use pluginRuntimeInitialize are not listed by HEN.
 */
Result pluginRuntimeInitializeWithService(Handle* out_ready_event,
                                          Handle* out_control_session);

/// Maximum scalar request and reply size supported by PluginService.
#define KAIHEN_PLUGIN_MESSAGE_SIZE 64

/**
 * @brief Handles one scalar CMIF command. The request data is copied before
 *        the callback, so the callback may make other IPC calls.
 * @param command_id The plugin's own command number.
 * @param input Input bytes, or NULL when input_size is zero.
 * @param input_size Number of input bytes.
 * @param output Buffer of KAIHEN_PLUGIN_MESSAGE_SIZE bytes.
 * @param[out] output_size Bytes written to output on success.
 * @param context Caller-owned context kept alive until pluginServiceStop.
 * @note For mapped buffers, transferred handles, or domains, implement a
 *       custom server with libounce-nx's IPC primitives instead.
 */
typedef Result (*PluginServiceHandler)(u32 command_id, const void* input,
                                       size_t input_size, void* output,
                                       size_t* output_size, void* context);

/// Maximum number of client sessions that a PluginService serves at once.
#define KAIHEN_PLUGIN_MAX_SESSIONS 16

/**
 * @brief A simple scalar-command server.
 * @note Two threads serve it. The control thread answers HEN's requests to
 *       create a client session, so that HEN never waits for the handler.
 *       The worker thread serves the client sessions and calls the handler.
 *       The fields belong to the server between Start and Stop.
 */
typedef struct {
    Handle control_session;                       ///< Borrowed from pluginRuntimeInitializeWithService.
    Handle stop_write;
    Handle stop_read;
    Handle wake_write;                            ///< Signaled when the control thread adds a session.
    Handle wake_read;
    Thread worker_thread;
    Thread control_thread;
    Mutex sessions_lock;
    Handle sessions[KAIHEN_PLUGIN_MAX_SESSIONS]; ///< Server ends of the client sessions.
    PluginServiceHandler handler;
    void* context;
    Result worker_result;
    Result control_result;
    bool started;
} PluginService;

/// Starts the threads that create client sessions through the control session and serve them.
Result pluginServiceStart(PluginService* service, Handle control_session,
                          PluginServiceHandler handler, void* context);

/**
 * @brief Stops the threads and closes the client sessions. The borrowed
 *        control session stays open.
 * @return The first failure of the worker or control thread, or 0. HEN
 *         closing the control session is not a failure.
 */
Result pluginServiceStop(PluginService* service);

/// Closes the services and the handles from the bootstrap.
void pluginRuntimeExit(void);

/**
 * @brief A function that handles one phase of an FS hook call.
 * @param context The hook ID, phase, x0-x8, and the user pointer from
 *        pluginFsHookInstall.
 * @return 0, or a failure that makes HEN fall back to the original function.
 * @note In HEN_FS_HOOK_BEFORE, set context->action to
 *       HEN_FS_HOOK_CALL_ORIGINAL (or call henFsHookCallOriginal) to run the
 *       original FS function. The function is then called again with phase
 *       HEN_FS_HOOK_AFTER and the original return registers. A hook that
 *       returns without requesting the original does not run an AFTER phase.
 * @note The callback runs on the plugin's hook thread. It may make IPC calls,
 *       but it must not block forever: FS waits for it. Do not install or
 *       remove a hook from a handler: HEN is waiting for that handler's
 *       reply while it holds the hook session.
 */
typedef Result (*HenFsHookHandler)(HenFsHookContext* context);

/// Sets context->action so that HEN runs the original FS function next.
static inline void henFsHookCallOriginal(HenFsHookContext* context) {
    context->action = HEN_FS_HOOK_CALL_ORIGINAL;
}

/**
 * @brief Installs an FS inline hook whose handler runs in this plugin.
 * @param target_offset The FS function entry's offset from the base of the
 *        FS module. HEN keeps a BTI landing pad and builds a trampoline for
 *        the original prologue.
 * @param handler The BEFORE and AFTER handler function. It is registered in
 *        this plugin and is never an address in the FS process.
 * @param user Passed to the handler through context->user. May be NULL.
 * @param[out] out The installed hook. info.trampoline runs the original
 *        prologue and jumps back; the bridge stub uses it for an original
 *        call. info.replacement is the HEN-owned bridge stub in FS.
 * @note Only plugins have this interface: HEN serves it on the session that
 *       the bootstrap gives a plugin during its handoff, never on a homebrew
 *       session. HEN needs the FS process handle that the bootstrap adopts
 *       after the plugins start; a call before then returns
 *       LibnxError_NotInitialized.
 * @note Hooks must be installed after the plugin runtime's hook thread has
 *       started. The runtime starts it during pluginRuntimeInitialize.
 */
Result pluginFsHookInstall(u64 target_offset, HenFsHookHandler handler,
                           void* user, HenFsHookInfo* out);

/// Restores the original instructions of an installed FS hook.
Result pluginFsHookRemove(u32 id);

/// Returns the number of installed FS hooks.
Result pluginFsHookCount(u32* out_count);

/// Returns the FS hook at @p index in installation order.
Result pluginFsHookInfo(u32 index, HenFsHookInfo* out);

#ifdef __cplusplus
}
#endif
