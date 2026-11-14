// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <switch.h>
#include "../../common/control.h"
#include "fs_hook.hpp"
#include "fs_hook_memory.hpp"
#include "game_arm.hpp"
#include "sm_proxy.hpp"
#include "../../libkaihen/source/cmif_server.hpp"

/// HEN's IPC server. It serves the mig:usr control port, on which the
/// bootstrap hands HEN its handles and configuration, the homebrew sessions
/// that it creates for processes that RO initialized, and the FS bridge
/// stub's TIPC port. Between requests its loop advances game arming.
class HenServer {
  public:
    HenServer();
    ~HenServer();
    HenServer(const HenServer&) = delete;
    HenServer& operator=(const HenServer&) = delete;

    /// Registers mig:usr and the FS hook port.
    Result initialize();

    /// Serves requests until a kernel call fails.
    Result run();

  private:
    static constexpr u32 MaxSessions = 16;
    /// HEN waits on every FsHook session in its main loop. Keep the total
    /// wait list within the kernel's 64-handle limit (three session kinds of
    /// MaxSessions, the FsHook sessions, and two ports).
    static constexpr u32 MaxFsHookSessions = 12;
    /// The most handles a request can copy (HIPC's limit).
    static constexpr u32 MaxCopyHandles = 15;

    enum class SessionKind {
        Control,  ///< A session on mig:usr.
        Homebrew, ///< A session from CreateHomebrewSession.
        Plugin,   ///< A plugin's session from CreatePluginSession: FS hooks only.
        FsHook,   ///< A bridge stub's TIPC session on kaih:fs.
    };

    struct SessionRef {
        SessionKind kind;
        u32 index;
    };

    /// The sessions that the loop waits on, and the ports before them.
    struct WaitList {
        Handle handles[2 + 3 * MaxSessions + MaxFsHookSessions];
        SessionRef sessions[2 + 3 * MaxSessions + MaxFsHookSessions]; ///< sessions[i] for handles[i], except ports.
        s32 count;
        s32 port_index;          ///< -1 once the port is released.
        s32 fs_hook_port_index;  ///< -1 when the named port is unavailable.
    };

    /// A request, copied out of the message buffer. Commands that make IPC
    /// calls of their own overwrite the buffer.
    struct ReceivedRequest {
        bool accepted;   ///< A valid CMIF request with inline data and copied handles only.
        u32 type;        ///< CmifCommandType
        u32 command_id;
        u32 num_copy_handles;
        Handle copy_handles[MaxCopyHandles];
        u8 data[ounce::MessageSize];
        size_t data_size;
    };

    /// What a command answers.
    struct CommandReply {
        Result result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        u8 data[sizeof(HenPluginName)] = {}; ///< The largest reply.
        size_t data_size = 0;
        /// A handle the reply copies to the client, or INVALID_HANDLE.
        Handle copy_handle = INVALID_HANDLE;
        /// The copied handle is a temporary of HEN's: close it once sent.
        bool close_copy_after_send = false;
        /// HEN keeps the handles that the request copied; otherwise they are closed.
        bool keeps_request_handles = false;

        template <typename T>
        void setData(const T& value) {
            static_assert(sizeof(T) <= sizeof(data));
            __builtin_memcpy(data, &value, sizeof(T));
            data_size = sizeof(T);
        }
    };

    struct PluginRecord {
        HenPluginName name = {};
        u64 process_id = 0;
        u32 token = 0; ///< The owner of the plugin's sessions and hooks.
        bool listed = false; ///< Publishes a service that GetPluginName lists.
        Handle control_session = INVALID_HANDLE;
        Handle hook_session = INVALID_HANDLE; ///< The plugin's callback client.
    };

    // The loop
    void collectWaits(WaitList& waits) const;
    void stepBetweenRequests();
    void acceptSession();
    void acceptFsHookSession();
    Result serveSession(SessionRef session);
    void serveFsHookSession(u32 index);
    Handle& sessionHandle(SessionRef session);
    void closeSession(SessionRef session);

    // Requests
    ReceivedRequest receiveRequest() const;
    CommandReply serveRequest(const ReceivedRequest& request, SessionRef session);
    void serveControlCommand(const ReceivedRequest& request, CommandReply& reply);
    void serveHomebrewCommand(const ReceivedRequest& request, u32 index, CommandReply& reply);
    void servePluginCommand(const ReceivedRequest& request, u32 index, CommandReply& reply);

    // Control commands
    Result startGameArm(Handle bootstrap_process);
    Result createHomebrewSession(Handle copied_process, Handle* out_client);
    Result adoptFilesystemRoot(Handle copied_root);
    Result adoptServiceManager(Handle copied_session);
    Result adoptPlugin(const HenPluginRecord& plugin, Handle control_session);
    Result adoptPluginHooks(const HenPluginRecord& plugin, Handle hook_session);
    Result createPluginSession(const HenPluginRecord& plugin, Handle* out_client);
    Result releaseControlPort();

    // Homebrew commands
    Result openPlugin(const HenPluginName& name, Handle* out_session);
    void removePlugin(u32 index);
    void removeExitedPlugins();
    u32 listedPluginCount() const;

    // Plugin registry
    Result ensurePlugin(const HenPluginRecord& plugin, PluginRecord** out);
    PluginRecord* findPluginByName(const HenPluginName& name);
    PluginRecord* findPluginByToken(u32 token);

    // Plugin commands and the FS bridge
    Result installFsHook(const HenFsHookRequest& request, u32 owner,
                         HenFsHookInfo* out);
    Result invokeFsHook(const HenFsHookCall& call, HenFsHookResult* out);

    Handle port_ = INVALID_HANDLE;
    Handle fs_hook_port_ = INVALID_HANDLE;
    Handle fs_root_ = INVALID_HANDLE;
    /// FS's SM session. Only the SM proxy uses it: programs get proxy
    /// sessions (GetServiceManager).
    Handle sm_session_ = INVALID_HANDLE;
    SmProxy sm_proxy_;
    bool settings_ready_ = false;
    Handle sessions_[MaxSessions] = {};
    Handle homebrew_sessions_[MaxSessions] = {};
    Handle homebrew_processes_[MaxSessions] = {};
    Handle plugin_sessions_[MaxSessions] = {};
    u32 plugin_session_owners_[MaxSessions] = {};
    Handle fs_hook_sessions_[MaxFsHookSessions] = {};
    PluginRecord plugins_[HEN_MAX_PLUGINS] = {};
    u32 plugin_count_ = 0;        ///< Bootstrap-created plugin records, listed or not.
    u32 listed_plugin_count_ = 0; ///< Records that publish a service.
    u32 next_plugin_token_ = 1;
    HenStatus status_ = {};
    GameArm game_ = {};
    ounce::FsHookManager fs_hooks_ = {};
    ounce::FsHookMemory fs_hook_memory_ = {};
};
