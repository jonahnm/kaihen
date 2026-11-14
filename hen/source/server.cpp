// SPDX-License-Identifier: GPL-2.0-only
#include <cstring>
#include "server.hpp"

namespace {

/// The service space that HEN registers mig:usr in.
constexpr u32 MigrationServiceSpace = 1;
/// How long the loop sleeps when no request is waiting.
constexpr u64 IdleSleepNs = 1000000;

bool isRequest(u32 type) {
    return type == CmifCommandType_Request || type == CmifCommandType_RequestWithContext;
}

bool hasBuffers(const HipcMetadata& meta) {
    return meta.num_send_statics || meta.num_recv_statics || meta.num_send_buffers ||
        meta.num_recv_buffers || meta.num_exch_buffers;
}

/// Checks a copy request: one nonempty wire chunk, no reserved bits, and an
/// FS range that does not wrap around the address space.
Result checkCopyRequest(const HenFsCopyRequest& request) {
    if (!request.size || request.size > HEN_FS_COPY_CHUNK_SIZE || request.reserved ||
        request.address > UINT64_MAX - request.size)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    return 0;
}

} // namespace

HenServer::HenServer() = default;

HenServer::~HenServer() {
    ounce::closeHandle(&fs_root_);
    ounce::closeHandle(&sm_session_);
    if (settings_ready_) {
        setsysExit();
    }
    for (Handle& session : sessions_) {
        ounce::closeHandle(&session);
    }
    for (Handle& session : homebrew_sessions_) {
        ounce::closeHandle(&session);
    }
    for (Handle& process : homebrew_processes_) {
        ounce::closeHandle(&process);
    }
    for (Handle& session : plugin_sessions_) {
        ounce::closeHandle(&session);
    }
    for (Handle& session : fs_hook_sessions_) {
        ounce::closeHandle(&session);
    }
    ounce::closeHandle(&fs_hook_port_);
    ounce::closeHandle(&port_);
    for (PluginRecord& plugin : plugins_) {
        ounce::closeHandle(&plugin.control_session);
        ounce::closeHandle(&plugin.hook_session);
    }
}

Result HenServer::initialize() {
    status_.protocol_version = HEN_PROTOCOL_VERSION;
    Result rc = svcGetProcessId(&status_.process_id, CUR_PROCESS_HANDLE);
    if (R_SUCCEEDED(rc)) {
        rc = setsysInitialize();
        settings_ready_ = R_SUCCEEDED(rc);
    }
    if (R_SUCCEEDED(rc)) {
        rc = smRegisterServiceInSpace(&port_, MigrationServiceSpace, smEncodeName("mig:usr"),
                                      false, MaxSessions);
    }
    // The bridge stubs ask for this port by name. HEN still works without
    // it; installing a hook then reports NotInitialized.
    if (R_SUCCEEDED(rc)) {
        if (R_FAILED(svcManageNamedPort(&fs_hook_port_, "kaih:fs", MaxFsHookSessions)))
            fs_hook_port_ = INVALID_HANDLE;
    }
    status_.initialization_result = rc;
    return rc;
}

//-----------------------------------------------------------------------------
// The loop
//-----------------------------------------------------------------------------

Result HenServer::run() {
    for (;;) {
        WaitList waits;
        collectWaits(waits);

        std::memset(armGetTls(), 0, ounce::MessageSize);
        s32 index = -1;
        const Result rc = svcReplyAndReceive(&index, waits.handles, waits.count,
                                             INVALID_HANDLE, 0);
        if (index < 0 || index >= waits.count) {
            if (rc == KERNELRESULT(TimedOut)) {
                stepBetweenRequests();
                continue;
            }
            if (R_FAILED(rc)) {
                return rc;
            }
            continue;
        }
        if (R_FAILED(rc) && !(index != waits.port_index && rc == KERNELRESULT(ConnectionClosed))) {
            return rc;
        }

        if (index == waits.port_index) {
            acceptSession();
            continue;
        }
        if (index == waits.fs_hook_port_index) {
            acceptFsHookSession();
            continue;
        }
        const SessionRef session = waits.sessions[index];
        if (rc == KERNELRESULT(ConnectionClosed)) {
            closeSession(session);
            continue;
        }
        if (session.kind == SessionKind::FsHook) {
            serveFsHookSession(session.index);
            continue;
        }
        const Result serve_result = serveSession(session);
        if (R_FAILED(serve_result)) {
            return serve_result;
        }
    }
}

void HenServer::collectWaits(WaitList& waits) const {
    waits.count = 0;
    waits.port_index = -1;
    waits.fs_hook_port_index = -1;
    if (port_ != INVALID_HANDLE) {
        waits.port_index = waits.count;
        waits.handles[waits.count++] = port_;
    }
    if (fs_hook_port_ != INVALID_HANDLE) {
        waits.fs_hook_port_index = waits.count;
        waits.handles[waits.count++] = fs_hook_port_;
    }
    for (u32 index = 0; index < MaxSessions; index++) {
        if (sessions_[index] != INVALID_HANDLE) {
            waits.sessions[waits.count] = {SessionKind::Control, index};
            waits.handles[waits.count++] = sessions_[index];
        }
    }
    for (u32 index = 0; index < MaxSessions; index++) {
        if (homebrew_sessions_[index] != INVALID_HANDLE) {
            waits.sessions[waits.count] = {SessionKind::Homebrew, index};
            waits.handles[waits.count++] = homebrew_sessions_[index];
        }
    }
    for (u32 index = 0; index < MaxSessions; index++) {
        if (plugin_sessions_[index] != INVALID_HANDLE) {
            waits.sessions[waits.count] = {SessionKind::Plugin, index};
            waits.handles[waits.count++] = plugin_sessions_[index];
        }
    }
    for (u32 index = 0; index < MaxFsHookSessions; index++) {
        if (fs_hook_sessions_[index] != INVALID_HANDLE) {
            waits.sessions[waits.count] = {SessionKind::FsHook, index};
            waits.handles[waits.count++] = fs_hook_sessions_[index];
        }
    }
}

/// Advances game arming between incoming requests.
void HenServer::stepBetweenRequests() {
    game_.step();
    svcSleepThread(IdleSleepNs);
}

void HenServer::acceptSession() {
    Handle session = INVALID_HANDLE;
    if (R_FAILED(svcAcceptSession(&session, port_))) {
        return;
    }
    for (Handle& available : sessions_) {
        if (available == INVALID_HANDLE) {
            available = session;
            return;
        }
    }
    svcCloseHandle(session);
}

void HenServer::acceptFsHookSession() {
    Handle session = INVALID_HANDLE;
    if (R_FAILED(svcAcceptSession(&session, fs_hook_port_)))
        return;
    for (Handle& available : fs_hook_sessions_) {
        if (available == INVALID_HANDLE) {
            available = session;
            return;
        }
    }
    svcCloseHandle(session);
}

/// Answers the request that the session sent.
Result HenServer::serveSession(SessionRef session) {
    const ReceivedRequest request = receiveRequest();
    if (request.type == CmifCommandType_Close) {
        closeSession(session);
        return 0;
    }

    CommandReply reply = serveRequest(request, session);
    if (!reply.keeps_request_handles) {
        for (u32 index = 0; index < request.num_copy_handles; index++) {
            svcCloseHandle(request.copy_handles[index]);
        }
    }

    void* message = armGetTls();
    const u32 num_copy_handles = reply.copy_handle != INVALID_HANDLE ? 1 : 0;
    ounce::writeReply(message, reply.result, reply.data, reply.data_size,
                      &reply.copy_handle, num_copy_handles);
    const Result rc = ounce::sendReply(sessionHandle(session));
    if (reply.close_copy_after_send) {
        ounce::closeHandle(&reply.copy_handle);
    }
    if (rc == KERNELRESULT(ConnectionClosed)) {
        closeSession(session);
        return 0;
    }
    return rc;
}

Handle& HenServer::sessionHandle(SessionRef session) {
    if (session.kind == SessionKind::Homebrew)
        return homebrew_sessions_[session.index];
    if (session.kind == SessionKind::Plugin)
        return plugin_sessions_[session.index];
    if (session.kind == SessionKind::FsHook)
        return fs_hook_sessions_[session.index];
    return sessions_[session.index];
}

void HenServer::closeSession(SessionRef session) {
    ounce::closeHandle(&sessionHandle(session));
    if (session.kind == SessionKind::Homebrew)
        ounce::closeHandle(&homebrew_processes_[session.index]);
    if (session.kind == SessionKind::Plugin)
        plugin_session_owners_[session.index] = 0;
}

//-----------------------------------------------------------------------------
// Requests
//-----------------------------------------------------------------------------

/// Copies the request out of the message buffer. A request that is not
/// accepted still has its handles, so that they can be closed.
HenServer::ReceivedRequest HenServer::receiveRequest() const {
    void* message = armGetTls();
    const ounce::CmifRequest parsed = ounce::parseRequest(message);
    const HipcMetadata& meta = parsed.hipc.meta;

    ReceivedRequest request = {};
    request.type = meta.type;
    request.num_copy_handles = meta.num_copy_handles < MaxCopyHandles ? meta.num_copy_handles
                                                                      : MaxCopyHandles;
    for (u32 index = 0; index < request.num_copy_handles; index++) {
        request.copy_handles[index] = parsed.hipc.data.copy_handles[index];
    }
    // HEN takes no moved handles; close them now.
    for (u32 index = 0; index < meta.num_move_handles; index++) {
        svcCloseHandle(parsed.hipc.data.move_handles[index]);
    }

    request.accepted = parsed.valid && !hasBuffers(meta) && !meta.num_move_handles;
    if (request.accepted) {
        request.command_id = parsed.command_id;
        request.data_size = parsed.data_size < sizeof(request.data) ? parsed.data_size
                                                                    : sizeof(request.data);
        std::memcpy(request.data, parsed.data, request.data_size);
    }
    return request;
}

/// Answers a request. The status commands answer on both kinds of session;
/// the others on one kind only. Anything else is BadInput.
HenServer::CommandReply HenServer::serveRequest(const ReceivedRequest& request,
                                                SessionRef session) {
    CommandReply reply;
    if (!request.accepted) {
        return reply;
    }
    if (request.type == CmifCommandType_Control &&
        request.command_id == ounce::CmifControl_QueryPointerBufferSize) {
        // HEN has no pointer buffer.
        const u16 pointer_buffer_size = 0;
        reply.result = 0;
        reply.setData(pointer_buffer_size);
        return reply;
    }
    if (!isRequest(request.type)) {
        return reply;
    }

    switch (request.command_id) {
    case HenControlCommand_GetStatus:
        if (!request.num_copy_handles) {
            reply.result = 0;
            reply.setData(status_);
        }
        return reply;
    case HenControlCommand_GetGameStatus:
        if (!request.num_copy_handles) {
            reply.result = 0;
            reply.setData(game_.status());
        }
        return reply;
    default:
        break;
    }

    if (session.kind == SessionKind::Control) {
        serveControlCommand(request, reply);
    } else if (session.kind == SessionKind::Plugin) {
        servePluginCommand(request, session.index, reply);
    } else {
        serveHomebrewCommand(request, session.index, reply);
    }
    return reply;
}

/// The commands of the bootstrap's control session.
void HenServer::serveControlCommand(const ReceivedRequest& request, CommandReply& reply) {
    const u32 copies = request.num_copy_handles;
    const Handle* handles = request.copy_handles;

    switch (request.command_id) {
    case HenControlCommand_ConfigureGame:
        if (!copies && request.data_size >= sizeof(HenGameConfig)) {
            HenGameConfig configuration;
            std::memcpy(&configuration, request.data, sizeof(configuration));
            reply.result = game_.configure(configuration);
        }
        break;
    case HenControlCommand_AdoptGameHandles0:
        if (copies == 8) {
            reply.result = game_.adoptHandles0(handles);
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_AdoptGameHandles1:
        if (copies == 7) {
            reply.result = game_.adoptHandles1(handles);
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_AdoptLoaderRoot:
        if (copies == 1) {
            reply.result = game_.adoptLoaderRoot(handles[0]);
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_StartGameArm:
        if (copies == 1 && fs_root_ != INVALID_HANDLE) {
            reply.result = startGameArm(handles[0]);
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_CreateHomebrewSession:
        if (copies == 1) {
            reply.result = createHomebrewSession(handles[0], &reply.copy_handle);
            reply.close_copy_after_send = R_SUCCEEDED(reply.result);
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_AdoptFilesystemRoot:
        if (copies == 1) {
            reply.result = adoptFilesystemRoot(handles[0]);
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_AdoptServiceManager:
        if (copies == 1) {
            reply.result = adoptServiceManager(handles[0]);
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_ReleaseControlPort:
        if (!copies) {
            reply.result = releaseControlPort();
        }
        break;
    case HenControlCommand_CreateServiceManagerSession:
        // A plugin's SM session, on the proxy as a homebrew program's is.
        if (!copies) {
            reply.result = sm_proxy_.openSession(&reply.copy_handle);
            reply.close_copy_after_send = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_CloneFilesystemRoot:
        // A plugin's FS root: a session of its own, not HEN's root.
        if (!copies && fs_root_ != INVALID_HANDLE) {
            reply.result = cmifCloneCurrentObject(fs_root_, &reply.copy_handle);
            reply.close_copy_after_send = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_AdoptPlugin:
        if (copies == 1 && request.data_size >= sizeof(HenPluginRecord)) {
            HenPluginRecord plugin;
            std::memcpy(&plugin, request.data, sizeof(plugin));
            reply.result = adoptPlugin(plugin, handles[0]);
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_AdoptPluginHooks:
        if (copies == 1 && request.data_size >= sizeof(HenPluginRecord)) {
            HenPluginRecord plugin;
            std::memcpy(&plugin, request.data, sizeof(plugin));
            reply.result = adoptPluginHooks(plugin, handles[0]);
            reply.keeps_request_handles = R_SUCCEEDED(reply.result);
        }
        break;
    case HenControlCommand_CreatePluginSession:
        // The bootstrap gives this session to a plugin. Only it can install
        // FS hooks; homebrew sessions cannot.
        if (!copies && request.data_size >= sizeof(HenPluginRecord)) {
            HenPluginRecord plugin;
            std::memcpy(&plugin, request.data, sizeof(plugin));
            reply.result = createPluginSession(plugin, &reply.copy_handle);
            reply.close_copy_after_send = R_SUCCEEDED(reply.result);
        }
        break;
    default:
        break;
    }
}

/// The commands of a homebrew process's session.
void HenServer::serveHomebrewCommand(const ReceivedRequest& request, u32 index,
                                     CommandReply& reply) {
    if (request.num_copy_handles) {
        return;
    }

    switch (request.command_id) {
    case HenHomebrewCommand_GetFilesystemRoot:
        if (fs_root_ == INVALID_HANDLE) {
            reply.result = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
            break;
        }
        reply.result = cmifCloneCurrentObject(fs_root_, &reply.copy_handle);
        reply.close_copy_after_send = R_SUCCEEDED(reply.result);
        break;
    case HenHomebrewCommand_GetSettingsSession:
        reply.result = cmifCloneCurrentObject(setsysGetServiceSession()->session,
                                              &reply.copy_handle);
        reply.close_copy_after_send = R_SUCCEEDED(reply.result);
        break;
    case HenHomebrewCommand_GetProcessHandle:
        // The process that RO initialized this session for. HEN keeps it.
        reply.result = 0;
        reply.copy_handle = homebrew_processes_[index];
        break;
    case HenHomebrewCommand_GetServiceManager:
        // A session of the program's own on the SM proxy, never the master:
        // SM closes a session after a malformed request, and the master
        // serves every program.
        reply.result = sm_proxy_.openSession(&reply.copy_handle);
        reply.close_copy_after_send = R_SUCCEEDED(reply.result);
        break;
    case HenHomebrewCommand_GetPluginCount:
        removeExitedPlugins();
        reply.result = 0;
        reply.setData(listedPluginCount());
        break;
    case HenHomebrewCommand_GetPluginName: {
        u32 plugin = 0;
        if (request.data_size < sizeof(plugin)) {
            break;
        }
        std::memcpy(&plugin, request.data, sizeof(plugin));
        removeExitedPlugins();
        u32 listed = 0;
        const PluginRecord* found = nullptr;
        for (u32 index = 0; index < plugin_count_; index++) {
            if (!plugins_[index].listed)
                continue;
            if (listed++ == plugin) {
                found = &plugins_[index];
                break;
            }
        }
        if (!found) {
            reply.result = MAKERESULT(Module_Libnx, LibnxError_NotFound);
            break;
        }
        reply.result = 0;
        reply.setData(found->name);
        break;
    }
    case HenHomebrewCommand_OpenPlugin: {
        HenPluginName name;
        if (request.data_size < sizeof(name)) {
            break;
        }
        std::memcpy(&name, request.data, sizeof(name));
        removeExitedPlugins();
        reply.result = openPlugin(name, &reply.copy_handle);
        reply.close_copy_after_send = R_SUCCEEDED(reply.result);
        break;
    }
    default:
        break;
    }
}

//-----------------------------------------------------------------------------
// Control commands
//-----------------------------------------------------------------------------

/// Starts game arming on a clone of HEN's FS root. The game arm closes the
/// bootstrap's process handle once it has started.
Result HenServer::startGameArm(Handle bootstrap_process) {
    Handle root = INVALID_HANDLE;
    Result rc = cmifCloneCurrentObject(fs_root_, &root);
    if (R_FAILED(rc)) {
        return rc;
    }
    rc = game_.start(root, bootstrap_process);
    if (R_FAILED(rc)) {
        svcCloseHandle(root);
    }
    return rc;
}

/// Creates a session for a process that RO initialized, and keeps the
/// process handle for GetProcessHandle.
Result HenServer::createHomebrewSession(Handle copied_process, Handle* out_client) {
    for (u32 index = 0; index < MaxSessions; index++) {
        if (homebrew_sessions_[index] != INVALID_HANDLE) {
            continue;
        }
        Handle server = INVALID_HANDLE;
        Handle client = INVALID_HANDLE;
        Result rc = svcCreateSession(&server, &client, 0, 0);
        if (R_SUCCEEDED(rc)) {
            homebrew_sessions_[index] = server;
            homebrew_processes_[index] = copied_process;
            *out_client = client;
        }
        return rc;
    }
    return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
}

Result HenServer::adoptFilesystemRoot(Handle copied_root) {
    if (fs_root_ != INVALID_HANDLE) {
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    }
    fs_root_ = copied_root;
    return 0;
}

Result HenServer::adoptServiceManager(Handle copied_session) {
    if (sm_session_ != INVALID_HANDLE) {
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    }
    sm_session_ = copied_session;
    sm_proxy_.setMaster(sm_session_);
    return 0;
}

Result HenServer::createPluginSession(const HenPluginRecord& plugin, Handle* out_client) {
    R_UNLESS(out_client, MAKERESULT(Module_Libnx, LibnxError_BadInput));
    PluginRecord* record = nullptr;
    R_TRY(ensurePlugin(plugin, &record));

    for (u32 index = 0; index < MaxSessions; index++) {
        if (plugin_sessions_[index] != INVALID_HANDLE)
            continue;
        Handle server = INVALID_HANDLE;
        Handle client = INVALID_HANDLE;
        Result rc = svcCreateSession(&server, &client, 0, 0);
        if (R_SUCCEEDED(rc)) {
            plugin_sessions_[index] = server;
            plugin_session_owners_[index] = record->token;
            *out_client = client;
        }
        return rc;
    }
    return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
}

Result HenServer::adoptPluginHooks(const HenPluginRecord& plugin, Handle hook_session) {
    if (hook_session == INVALID_HANDLE)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    PluginRecord* record = nullptr;
    R_TRY(ensurePlugin(plugin, &record));
    R_UNLESS(record->hook_session == INVALID_HANDLE,
             MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized));
    record->hook_session = hook_session;
    return 0;
}

Result HenServer::releaseControlPort() {
    if (port_ == INVALID_HANDLE) {
        return 0;
    }
    const Result rc = smUnregisterServiceInSpace(MigrationServiceSpace, smEncodeName("mig:usr"));
    if (R_FAILED(rc)) {
        return rc;
    }
    ounce::closeHandle(&port_);
    return 0;
}

Result HenServer::adoptPlugin(const HenPluginRecord& plugin, Handle control_session) {
    if (control_session == INVALID_HANDLE)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    PluginRecord* record = nullptr;
    R_TRY(ensurePlugin(plugin, &record));
    R_UNLESS(record->control_session == INVALID_HANDLE,
             MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized));
    record->control_session = control_session;
    if (!record->listed) {
        record->listed = true;
        listed_plugin_count_++;
    }
    return 0;
}

Result HenServer::ensurePlugin(const HenPluginRecord& plugin, PluginRecord** out) {
    const HenPluginName& name = plugin.name;
    if (!name.name[0] || !std::memchr(name.name, '\0', sizeof(name.name)))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    PluginRecord* existing = findPluginByName(name);
    if (existing) {
        if (existing->process_id && plugin.process_id &&
            existing->process_id != plugin.process_id)
            return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
        if (!existing->process_id)
            existing->process_id = plugin.process_id;
        *out = existing;
        return 0;
    }

    if (plugin_count_ == HEN_MAX_PLUGINS)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    PluginRecord& record = plugins_[plugin_count_++];
    record = {};
    record.name = name;
    record.process_id = plugin.process_id;
    record.token = next_plugin_token_++;
    if (!next_plugin_token_)
        next_plugin_token_ = 1;
    *out = &record;
    return 0;
}

HenServer::PluginRecord* HenServer::findPluginByName(const HenPluginName& name) {
    for (u32 index = 0; index < plugin_count_; index++) {
        if (!std::strcmp(plugins_[index].name.name, name.name))
            return &plugins_[index];
    }
    return nullptr;
}

HenServer::PluginRecord* HenServer::findPluginByToken(u32 token) {
    if (!token)
        return nullptr;
    for (u32 index = 0; index < plugin_count_; index++) {
        if (plugins_[index].token == token)
            return &plugins_[index];
    }
    return nullptr;
}

u32 HenServer::listedPluginCount() const {
    return listed_plugin_count_;
}

//-----------------------------------------------------------------------------
// Plugin commands
//-----------------------------------------------------------------------------

/// The FS hook interface. It is served on CreatePluginSession sessions only:
/// the bootstrap gives one to each plugin, and homebrew has no way to obtain
/// one. HEN writes FS code through RO's ro:dmnt endpoint, so the game arm's
/// reader and FS process handle must have been adopted first. The copy
/// commands need the same two handles, but not the kaih:fs port.
void HenServer::servePluginCommand(const ReceivedRequest& request, u32 index,
                                   CommandReply& reply) {
    if (request.num_copy_handles) {
        return;
    }
    const u32 owner = plugin_session_owners_[index];
    if (!owner)
        return;

    switch (request.command_id) {
    case HenPluginCommand_InstallFsHook:
        if (request.data_size >= sizeof(HenFsHookRequest)) {
            HenFsHookRequest hook;
            std::memcpy(&hook, request.data, sizeof(hook));
            HenFsHookInfo info = {};
            reply.result = installFsHook(hook, owner, &info);
            if (R_SUCCEEDED(reply.result))
                reply.setData(info);
        }
        break;
    case HenPluginCommand_RemoveFsHook:
        if (request.data_size >= sizeof(u32)) {
            u32 id = 0;
            std::memcpy(&id, request.data, sizeof(id));
            reply.result = fs_hooks_.remove(id);
        }
        break;
    case HenPluginCommand_GetFsHookCount:
        reply.result = 0;
        reply.setData(fs_hooks_.count());
        break;
    case HenPluginCommand_GetFsHookInfo:
        if (request.data_size >= sizeof(u32)) {
            u32 index = 0;
            std::memcpy(&index, request.data, sizeof(index));
            HenFsHookInfo info = {};
            reply.result = fs_hooks_.info(index, &info);
            if (R_SUCCEEDED(reply.result))
                reply.setData(info);
        }
        break;
    case HenPluginCommand_CopyToFs:
        if (request.data_size >= sizeof(HenFsCopyRequest)) {
            HenFsCopyRequest copy;
            std::memcpy(&copy, request.data, sizeof(copy));
            reply.result = copyToFs(copy);
        }
        break;
    case HenPluginCommand_CopyFromFs:
        if (request.data_size >= sizeof(HenFsCopyRequest)) {
            HenFsCopyRequest copy;
            std::memcpy(&copy, request.data, sizeof(copy));
            HenFsCopyReply data = {};
            reply.result = copyFromFs(copy, &data);
            if (R_SUCCEEDED(reply.result))
                reply.setData(data);
        }
        break;
    default:
        break;
    }
}

Result HenServer::ensureFsHookMemory() {
    if (fs_hook_memory_.ready())
        return 0;
    const Handle reader = game_.processReader();
    const Handle process = game_.filesystemProcess();
    if (reader == INVALID_HANDLE || process == INVALID_HANDLE)
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    fs_hook_memory_.configure(reader, process);
    if (!fs_hook_memory_.ready())
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    fs_hooks_.setBackend(fs_hook_memory_.backend());
    return 0;
}

Result HenServer::copyToFs(const HenFsCopyRequest& request) {
    R_TRY(checkCopyRequest(request));
    R_TRY(ensureFsHookMemory());
    return fs_hook_memory_.copyIn(request.address, request.data, request.size);
}

Result HenServer::copyFromFs(const HenFsCopyRequest& request, HenFsCopyReply* out) {
    R_UNLESS(out, MAKERESULT(Module_Libnx, LibnxError_BadInput));
    R_TRY(checkCopyRequest(request));
    R_TRY(ensureFsHookMemory());
    return fs_hook_memory_.copyOut(request.address, out->data, request.size);
}

Result HenServer::installFsHook(const HenFsHookRequest& request, u32 owner,
                                HenFsHookInfo* out) {
    if (fs_hook_port_ == INVALID_HANDLE)
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    R_TRY(ensureFsHookMemory());

    u64 module_base = 0;
    R_TRY(fs_hook_memory_.moduleBase(&module_base));
    return fs_hooks_.install(request, module_base, owner, out);
}

//-----------------------------------------------------------------------------
// The FS bridge
//-----------------------------------------------------------------------------

/// Receives one TIPC call from a bridge stub, invokes the plugin handler and
/// writes the action and registers back. The stub falls back to the original
/// when the TIPC result is nonzero.
void HenServer::serveFsHookSession(u32 index) {
    void* message = armGetTls();
    const HipcParsedRequest parsed = hipcParseRequest(message);
    const HipcMetadata& meta = parsed.meta;

    HenFsHookCall call = {};
    Result result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (meta.type == ounce::sm::TipcRequestTypeBase + HEN_FS_HOOK_CALL_COMMAND &&
        !meta.num_copy_handles && !meta.num_move_handles && !hasBuffers(meta) &&
        meta.num_data_words >= sizeof(HenFsHookCall) / sizeof(u32)) {
        std::memcpy(&call, parsed.data.data_words, sizeof(call));
        HenFsHookResult hook_result = {};
        result = invokeFsHook(call, &hook_result);
        if (R_SUCCEEDED(result)) {
            std::memset(message, 0, ounce::MessageSize);
            HipcRequest reply = hipcMakeRequestInline(message,
                .num_data_words = 2 + sizeof(HenFsHookResult) / sizeof(u32));
            reply.data_words[0] = 0;
            reply.data_words[1] = 0;
            std::memcpy(reply.data_words + 2, &hook_result, sizeof(hook_result));
            const Result send_rc = ounce::sendReply(fs_hook_sessions_[index]);
            if (send_rc == KERNELRESULT(ConnectionClosed))
                closeSession({SessionKind::FsHook, index});
            return;
        }
    }

    // A malformed request, or one for a hook whose plugin is gone, must run
    // the original. The stub checks the TIPC result and performs that fallback.
    HenFsHookResult fallback = {};
    fallback.action = HEN_FS_HOOK_CALL_ORIGINAL;
    fallback.result = result;
    std::memcpy(fallback.regs, call.regs, sizeof(fallback.regs));
    std::memset(message, 0, ounce::MessageSize);
    HipcRequest reply = hipcMakeRequestInline(message,
        .num_data_words = 2 + sizeof(HenFsHookResult) / sizeof(u32));
    reply.data_words[0] = result;
    reply.data_words[1] = 0;
    std::memcpy(reply.data_words + 2, &fallback, sizeof(fallback));
    const Result send_rc = ounce::sendReply(fs_hook_sessions_[index]);
    if (send_rc == KERNELRESULT(ConnectionClosed))
        closeSession({SessionKind::FsHook, index});
}

Result HenServer::invokeFsHook(const HenFsHookCall& call, HenFsHookResult* out) {
    R_UNLESS(out, MAKERESULT(Module_Libnx, LibnxError_BadInput));
    R_UNLESS(call.phase == HEN_FS_HOOK_BEFORE || call.phase == HEN_FS_HOOK_AFTER,
             MAKERESULT(Module_Libnx, LibnxError_BadInput));

    u32 owner = 0;
    u32 handler_id = 0;
    R_TRY(fs_hooks_.callInfo(call.hook_id, &owner, &handler_id));
    PluginRecord* plugin = findPluginByToken(owner);
    R_UNLESS(plugin && plugin->hook_session != INVALID_HANDLE,
             MAKERESULT(Module_Libnx, LibnxError_NotInitialized));

    HenFsHookEvent event = {};
    event.hook_id = call.hook_id;
    event.handler_id = handler_id;
    event.phase = call.phase;
    std::memcpy(event.regs, call.regs, sizeof(event.regs));

    HenFsHookReply reply = {};
    Service service = {};
    serviceCreate(&service, plugin->hook_session);
    service.own_handle = false;
    const Result rc = serviceDispatchInOut(&service, HEN_PLUGIN_HOOK_CALL, event, reply);

    out->action = call.phase == HEN_FS_HOOK_BEFORE ? HEN_FS_HOOK_CALL_ORIGINAL
                                                   : HEN_FS_HOOK_RETURN;
    out->result = R_SUCCEEDED(rc) ? reply.result : rc;
    std::memcpy(out->regs, call.regs, sizeof(out->regs));
    if (R_SUCCEEDED(rc) && R_SUCCEEDED(reply.result)) {
        std::memcpy(out->regs, reply.regs, sizeof(out->regs));
        if (call.phase == HEN_FS_HOOK_BEFORE &&
            reply.action == HEN_FS_HOOK_CALL_ORIGINAL)
            out->action = HEN_FS_HOOK_CALL_ORIGINAL;
        else
            out->action = HEN_FS_HOOK_RETURN;
    }
    // A failed plugin call is not an FS failure: the stub receives a
    // successful TIPC reply with the original action and registers.
    return 0;
}

//-----------------------------------------------------------------------------
// Homebrew commands
//-----------------------------------------------------------------------------

/// Asks the named plugin for a new client session.
Result HenServer::openPlugin(const HenPluginName& name, Handle* out_session) {
    if (!std::memchr(name.name, '\0', sizeof(name.name))) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    for (u32 index = 0; index < plugin_count_; index++) {
        if (!plugins_[index].listed)
            continue;
        if (std::strcmp(plugins_[index].name.name, name.name)) {
            continue;
        }
        Service control = {};
        serviceCreate(&control, plugins_[index].control_session);
        control.own_handle = false;
        const Result rc = serviceDispatch(&control, HEN_PLUGIN_CONTROL_CREATE_SESSION,
            .out_handle_attrs = {SfOutHandleAttr_HipcCopy},
            .out_handles = out_session,
        );
        // The plugin closed its control session, so it no longer serves.
        if (rc == KERNELRESULT(ConnectionClosed)) {
            removePlugin(index);
            return MAKERESULT(Module_Libnx, LibnxError_NotFound);
        }
        return rc;
    }
    return MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

void HenServer::removePlugin(u32 index) {
    PluginRecord& plugin = plugins_[index];
    if (plugin.listed && listed_plugin_count_)
        listed_plugin_count_--;
    ounce::closeHandle(&plugin.control_session);
    ounce::closeHandle(&plugin.hook_session);

    // A hook whose plugin has exited must not call the old callback session.
    // The hook stays listed, but invokeFsHook finds no token and falls back.
    for (u32 slot = 0; slot < MaxSessions; slot++) {
        if (plugin_sessions_[slot] != INVALID_HANDLE &&
            plugin_session_owners_[slot] == plugin.token) {
            ounce::closeHandle(&plugin_sessions_[slot]);
            plugin_session_owners_[slot] = 0;
        }
    }

    plugin_count_--;
    for (u32 next = index; next < plugin_count_; next++)
        plugins_[next] = plugins_[next + 1];
    plugins_[plugin_count_] = {};
}

/// Stops listing the plugins whose process has exited. It runs before the
/// count is returned and before a plugin is opened, so a list read between
/// the two can still name a plugin that exits in the meantime.
void HenServer::removeExitedPlugins() {
    for (u32 index = plugin_count_; index > 0; index--) {
        if (!game_.processExists(plugins_[index - 1].process_id)) {
            removePlugin(index - 1);
        }
    }
}
