// SPDX-License-Identifier: GPL-2.0-only
#include "compiler_tls.hpp"
#include "../../common/domain_mount.hpp"
#include "../../common/loader_gate.hpp"
#include "pm_launch.hpp"
#include "../../common/package.hpp"
#include "policies.hpp"
#include "../../common/control.h"
#include "../../libkaihen/source/plugin_startup.h"
#include "startup.hpp"
#include "../../common/policy_package.hpp"
#include "../../common/sd_writer.hpp"

using BootstrapMemory = ounce::RetainedMemory<ounce::MemoryWriter::RetainedStorage>;
using BootstrapGate = ounce::LoaderGate<ounce::MemoryWriter::RetainedStorage>;
using BootstrapMount = ounce::DomainMount<ounce::MemoryWriter::RetainedStorage>;
using BootstrapPackage = ounce::NativePackage<ounce::MemoryWriter::RetainedStorage>;

constexpr u64 PluginReadyTimeoutNs = 5000000000ull;

/// What happened to one plugin at startup, for /kaihen/plugin-status.txt.
enum PluginOutcome : u32 {
    PluginOutcome_Running,         ///< Started and ready.
    PluginOutcome_Published,       ///< Started and ready, and HEN lists its service.
    PluginOutcome_NotReady,        ///< Started, but not ready in time; its service is not published.
    PluginOutcome_TooManyServices, ///< Started and ready, but HEN already lists HEN_MAX_PLUGINS services.
    PluginOutcome_NameTooLong,     ///< Not started: the file name is too long.
    PluginOutcome_PackageFailed,   ///< Not started: its package could not be written.
    PluginOutcome_StartFailed,     ///< Starting it failed, and startup stopped.
};

struct LaunchState {
    struct PluginRecord {
        HenPluginName name;
        PluginOutcome outcome;
        BootstrapOperation stage;
        Result result;
    };
    static constexpr u32 MaxPluginRecords = 2 * HEN_MAX_PLUGINS;

    const BootstrapInput& input;
    BootstrapResult* report = nullptr;
    ounce::PmLaunchRequest pm = {};
    Result result = 0;
    Result gate_restore_result = 0;
    Result channel_restore_result = 0;
    bool gate_restore_pending = false;
    u64 thread = 0;
    u32 mounts = 0;
    bool exchanged = false;
    Handle reader_session = INVALID_HANDLE;
    BootstrapOperation stage = BootstrapOperation_Begin;
    /// Plugins whose service HEN lists.
    u32 plugin_count = 0;
    HenPluginName current_plugin = {};
    PluginOutcome current_outcome = PluginOutcome_Running;
    PluginRecord plugin_records[MaxPluginRecords] = {};
    u32 plugin_record_count = 0;
    /// HEN's control session, from HEN's start until startup ends. Plugins
    /// get their SM and FS sessions through it.
    Handle hen_session = INVALID_HANDLE;

    ~LaunchState() {
        if (hen_session != INVALID_HANDLE) {
            svcCloseHandle(hen_session);
        }
    }

    void mark(BootstrapOperation operation) {
        stage = operation;
        if (report) {
            report->operation = operation;
        }
    }
};

static Result _launchOpenProcessReader(LaunchState* args) {
    TipcService sm = {};
    tipcCreate(&sm, args->input.sm_session);
    const SmServiceName name = smEncodeName("ro:dmnt");
    return tipcDispatchIn(&sm, 1, name,
        .out_handle_attrs = {SfOutHandleAttr_HipcMove},
        .out_handles = &args->reader_session,
    );
}

static void _launchExchangeMount(LaunchState* args, BootstrapGate& gate,
                                 BootstrapMount& mount, ounce::CompilerTlsScope& tls) {
    for (u32 step = 0; step < 30000 && !args->exchanged; step++) {
        bool ready = false;
        args->result = gate.pending(ready);
        if (R_FAILED(args->result)) {
            break;
        }
        if (!ready) {
            tls.suspend();
            u64 finished = args->input.pthread_trywait(args->thread, 0, 0);
            tls.resume();
            // PM can return the new PID before Loader opens its CodeFS.
            // Keep watching the parked gate after a successful PM reply.
            if (finished == 1 && R_FAILED(args->pm.result)) {
                break;
            }
            svcSleepThread(1000000);
            continue;
        }
        ounce::DomainEntry before[ounce::MaxDomainEntries], after[ounce::MaxDomainEntries], added;
        u32 before_count = 0, after_count = 0;
        args->mark(BootstrapOperation_Mount);
        args->result = mount.snapshot(before, before_count);
        if (R_FAILED(args->result)) {
            break;
        }
        alignas(16) u8 request[ounce::LoaderRequestSize];
        args->result = gate.step(request);
        if (R_FAILED(args->result)) {
            break;
        }
        args->result = mount.snapshot(after, after_count);
        if (R_FAILED(args->result)) {
            break;
        }
        bool target = false;
        args->result = mount.inspectRequest(request, target);
        if (R_FAILED(args->result)) {
            break;
        }
        if (target) {
            args->mounts++;
            args->result = mount.added(
                before, before_count, after, after_count, added);
            if (R_FAILED(args->result)) {
                break;
            }
            if (args->mounts == 2) {
                args->mark(BootstrapOperation_Exchange);
                args->result = mount.exchange(added);
                if (R_SUCCEEDED(args->result)) {
                    args->exchanged = true;
                }
                break;
            }
        }
    }
}

static void _launchRequest(LaunchState* args, BootstrapGate& gate,
                           BootstrapMount& mount, ounce::CompilerTlsScope& tls) {
    args->mark(BootstrapOperation_Park);
    args->result = gate.park();
    if (R_FAILED(args->result)) {
        return;
    }

    args->mark(BootstrapOperation_Launch);
    u64 thread = 0;
    tls.suspend();
    args->result = args->input.pthread_create(&thread, 0, ounce::launchFromSdkThread, &args->pm);
    tls.resume();
    args->thread = thread;
    if (R_FAILED(args->result)) {
        return;
    }

    _launchExchangeMount(args, gate, mount, tls);
}

static void _launchRestoreAndJoin(LaunchState* args, BootstrapGate& gate,
                                  BootstrapMemory& memory, ounce::CompilerTlsScope& tls,
                                  bool require_complete) {
    args->mark(BootstrapOperation_Restore);
    args->gate_restore_result = gate.restore();
    if (require_complete) {
        for (u32 attempt = 0; attempt < 4 &&
                args->gate_restore_result == KERNELRESULT(TimedOut) &&
                gate.pendingRestoreWithReceipt(); attempt++) {
            args->gate_restore_result = gate.restore();
        }
    }
    args->gate_restore_pending =
        args->gate_restore_result == KERNELRESULT(TimedOut) &&
        gate.pendingRestoreWithReceipt();
    args->channel_restore_result = memory.restore();
    if (args->thread) {
        args->mark(BootstrapOperation_Join);
        tls.suspend();
        u64 returned = 0;
        args->input.pthread_join(args->thread, &returned, 0);
        tls.resume();
    }
}

static void _launchStartProcess(LaunchState* args) {
    args->mark(BootstrapOperation_Start);
    Service* monitor = pmdmntGetServiceSession();
    serviceCreate(monitor, args->pm.monitor_session);
    u64 identity = 0;
    args->result = pmdmntGetProgramId(&identity, args->pm.pid);
    if (R_FAILED(args->result)) {
        return;
    }
    if (identity != args->pm.program_id) {
        args->result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return;
    }
    args->result = pmdmntStartProcess(args->pm.pid);
    if (R_SUCCEEDED(args->result) && args->report) {
        args->report->process_id = args->pm.pid;
    }
}

static Result _launchArmGame(LaunchState* args, Service* hen) {
    const BootstrapInput& input = args->input;
    // HEN reads the selected game's title from Loader's pending CodeFS
    // request. The package supplies one reusable signed game policy.
    HenGameConfig config = {};
    config.data_offset = input.memory.data_offset;
    config.pointer_offset = input.memory.pointer_offset;
    config.server = input.gate.server;
    config.vptr = input.gate.vptr;
    config.domain = input.gate.domain;
    config.metadata = input.gate.metadata;
    config.request = input.gate.request;
    __builtin_memcpy(config.waiters, input.gate.waiters, sizeof(config.waiters));
    config.table = input.mount.table;
    config.loader_root_id = input.mount.root_id;
    config.receive = input.gate.receive;
    config.original_wait = input.gate.original_wait;
    config.park = input.gate.park;
    config.bootstrap_restore_pending = args->gate_restore_pending ? 1 : 0;

    args->mark(BootstrapOperation_GameConfig);
    Result rc = henConfigureGame(hen, &config);
    if (R_FAILED(rc)) {
        return rc;
    }
    args->mark(BootstrapOperation_GameHandles0);
    rc = henAdoptGameHandles0(hen, input.memory.storage, input.gate.workers);
    if (R_FAILED(rc)) {
        return rc;
    }
    const Handle handles[] = {
        input.gate.server_copy, input.gate.event_read,
        input.gate.event_write, input.monitor_session,
        args->reader_session, input.fs_process, input.fs_notifier,
    };
    args->mark(BootstrapOperation_GameHandles1);
    rc = henAdoptGameHandles1(hen, handles);
    if (R_FAILED(rc)) {
        return rc;
    }
    rc = henAdoptLoaderRoot(hen, input.loader_root_session);
    if (R_FAILED(rc)) {
        return rc;
    }
    args->mark(BootstrapOperation_GameStart);

    return henStartGameArm(hen, CUR_PROCESS_HANDLE);
}

static void _launchConnectRo(LaunchState* args, Handle hen_handle) {
    Service ro = {};
    serviceCreate(&ro, args->input.ro_session);
    const u64 pid_placeholder = 0;
    u64 hen_pid = 0;
    args->result = serviceDispatchInOut(
        &ro, 65001, pid_placeholder, hen_pid,
        .in_send_pid = true,
        .in_num_handles = 1,
        .in_handles = { hen_handle },
    );
}

/// Gives HEN a plugin's control session, or records why it is not
/// published.
static void _launchPublishPlugin(LaunchState* args, Handle control_session) {
    if (control_session == INVALID_HANDLE) {
        args->current_outcome = PluginOutcome_Running;
        return;
    }
    if (args->plugin_count == HEN_MAX_PLUGINS) {
        args->current_outcome = PluginOutcome_TooManyServices;
        svcCloseHandle(control_session);
        return;
    }
    Service hen = {};
    serviceCreate(&hen, args->hen_session);
    const HenPluginRecord record = {args->current_plugin, args->pm.pid};
    args->result = henAdoptPlugin(&hen, &record, control_session);
    svcCloseHandle(control_session);
    if (R_FAILED(args->result))
        return;
    args->plugin_count++;
    args->current_outcome = PluginOutcome_Published;
}

/// Gives a started plugin its SM session and FS root through its temporary
/// mig:usr service, releases that service, and waits for the plugin to be
/// ready. Both sessions are the plugin's own, from HEN: a session on HEN's
/// SM proxy and a clone of HEN's FS root. FS's SM session and the FS root
/// itself stay with HEN and the bootstrap, so that a plugin cannot close
/// them for everyone. A third session, when HEN can create one, is the
/// plugin-only interface for HEN's FS hooks; a plugin can start without it.
static void _launchHandOffToPlugin(LaunchState* args, Handle plugin_handle) {
    args->mark(BootstrapOperation_PluginHandoff);
    Handle ready_read = INVALID_HANDLE;
    Handle release_write = INVALID_HANDLE;
    Handle control_session = INVALID_HANDLE;
    Handle sm_session = INVALID_HANDLE;
    Handle fs_root = INVALID_HANDLE;
    Handle hook_session = INVALID_HANDLE;
    Handle hook_callback = INVALID_HANDLE;
    u32 runtime_version = 0;
    const HenPluginRecord record = {args->current_plugin, args->pm.pid};
    Service hen = {};
    serviceCreate(&hen, args->hen_session);
    args->result = henCreateServiceManagerSession(&hen, &sm_session);
    if (R_SUCCEEDED(args->result))
        args->result = henCloneFilesystemRoot(&hen, &fs_root);
    if (R_SUCCEEDED(args->result)) {
        // The hook interface is optional: a plugin still starts without it.
        henCreatePluginSession(&hen, &record, &hook_session);
        Service service = {};
        serviceCreate(&service, plugin_handle);
        args->result = pluginAdoptHandles(&service, sm_session, fs_root,
                                          hook_session, &runtime_version,
                                          &release_write, &ready_read,
                                          &control_session);
        // A runtime 4 plugin created a callback session pair. Give HEN the
        // client copy before releasing the handoff, so that it can call the
        // handler for every bridge-stub request.
        if (R_SUCCEEDED(args->result) && runtime_version >= 4 &&
            hook_session != INVALID_HANDLE) {
            Result hook_rc = pluginGetHookSession(&service, &hook_callback);
            if (R_SUCCEEDED(hook_rc)) {
                hook_rc = henAdoptPluginHooks(&hen, &record, hook_callback);
                svcCloseHandle(hook_callback);
            }
        }
    }
    // The plugin has its copies of the sessions now. Signal release while the
    // mig:usr session is still open: the plugin serves GetHookSession and
    // then waits for this event on that session. Closing the session first
    // would look like a failed handoff.
    if (sm_session != INVALID_HANDLE)
        svcCloseHandle(sm_session);
    if (fs_root != INVALID_HANDLE)
        svcCloseHandle(fs_root);
    if (hook_session != INVALID_HANDLE)
        svcCloseHandle(hook_session);
    if (R_SUCCEEDED(args->result))
        args->result = svcSignalEvent(release_write);
    svcCloseHandle(plugin_handle);
    if (R_FAILED(args->result))
        return;

    args->mark(BootstrapOperation_PluginReady);
    Result ready_result = 0;
    if (R_SUCCEEDED(args->result))
        ready_result = svcWaitSynchronizationSingle(ready_read, PluginReadyTimeoutNs);
    svcCloseHandle(release_write);
    svcCloseHandle(ready_read);

    // A plugin that is not ready in time has already released mig:usr, so
    // startup goes on. The plugin keeps running; its service is not published.
    if (ready_result == KERNELRESULT(TimedOut)) {
        args->current_outcome = PluginOutcome_NotReady;
        if (control_session != INVALID_HANDLE)
            svcCloseHandle(control_session);
        return;
    }
    if (R_FAILED(ready_result))
        args->result = ready_result;
    if (R_FAILED(args->result)) {
        if (control_session != INVALID_HANDLE)
            svcCloseHandle(control_session);
        return;
    }
    _launchPublishPlugin(args, control_session);
}

static void _launchConnectServices(LaunchState* args, bool plugin) {
    args->mark(BootstrapOperation_Connect);
    TipcService sm = {};
    // The initialized SM session is borrowed from RO.
    // Closing the session protocol would close RO's
    // underlying shared connection as well.
    tipcCreate(&sm, args->input.sm_session);
    Handle hen_handle = INVALID_HANDLE;
    SmServiceName name = smEncodeName("mig:usr");
    if (plugin) {
        args->mark(BootstrapOperation_PluginLookup);
    }
    args->result = tipcDispatchIn(
        &sm, 1, name,
        .out_handle_attrs = { SfOutHandleAttr_HipcMove },
        .out_handles = &hen_handle,
    );
    if (R_FAILED(args->result)) {
        return;
    }

    if (plugin) {
        _launchHandOffToPlugin(args, hen_handle);
        return;
    }

    // HEN starts first: it keeps FS's SM session and the FS root, and gives
    // each plugin sessions of its own. Then it releases mig:usr, which each
    // plugin registers in turn for its handoff.
    args->hen_session = hen_handle;
    Service hen = {};
    serviceCreate(&hen, hen_handle);
    HenStatus status = {};
    args->result = henGetStatus(&hen, &status);
    if (R_SUCCEEDED(args->result)) {
        args->result = henAdoptFilesystemRoot(&hen, args->input.fs_root_session);
    }
    if (R_SUCCEEDED(args->result)) {
        args->result = henAdoptServiceManager(&hen, args->input.sm_session);
    }
    if (R_SUCCEEDED(args->result)) {
        args->result = henReleaseControlPort(&hen);
    }
}

/// After the plugins: gives RO HEN's control session and starts game
/// arming. Then closes the bootstrap's handle of HEN's session.
static void _launchFinishStartup(LaunchState* args) {
    Service hen = {};
    serviceCreate(&hen, args->hen_session);
    _launchConnectRo(args, args->hen_session);
    if (R_SUCCEEDED(args->result)) {
        args->result = _launchArmGame(args, &hen);
    }

    // RO retains a copy after handoff. Close only our kernel handle;
    // CMIF Close would invalidate the shared session underneath RO.
    svcCloseHandle(args->hen_session);
    args->hen_session = INVALID_HANDLE;
}

static void _launchOne(LaunchState* args, ounce::CompilerTlsScope& tls,
                       const char* package_path, bool plugin) {
    args->pm.result = 0;
    args->pm.pid = 0;
    if (args->report) {
        args->report->process_id = 0;
    }
    args->pm.completed = 0;
    args->thread = 0;
    args->mounts = 0;
    args->exchanged = false;
    args->gate_restore_pending = false;
    args->gate_restore_result = 0;
    args->channel_restore_result = 0;
    BootstrapMemory memory(args->input.memory, args->reader_session,
                           args->input.fs_process);
    args->mark(BootstrapOperation_Begin);
    args->result = memory.begin();
    if (R_FAILED(args->result)) {
        return;
    }

    BootstrapGate gate(memory, args->input.gate);
    ounce::DomainMountConfig mount_config = args->input.mount;
    BootstrapPackage package(memory, args->input.fs_root_session);
    args->mark(BootstrapOperation_Package);
    args->result = package.open(package_path, mount_config);
    if (R_SUCCEEDED(args->result)) {
        BootstrapMount mount(memory, mount_config);
        _launchRequest(args, gate, mount, tls);
    }

    const BootstrapOperation failed_stage = args->stage;

    // Restore both channels even when parking, thread creation or exchange fails.
    // Joining must happen after restoration, so the PM request can finish.
    _launchRestoreAndJoin(args, gate, memory, tls, plugin);
    if (R_FAILED(args->result)) {
        package.close();
        args->mark(failed_stage);
        return;
    }
    if ((R_FAILED(args->gate_restore_result) && !args->gate_restore_pending) ||
        R_FAILED(args->channel_restore_result)) {
        package.close();
        args->mark(BootstrapOperation_Restore);
        args->result = R_FAILED(args->gate_restore_result)
            ? args->gate_restore_result : args->channel_restore_result;
        return;
    }
    if (R_FAILED(args->pm.result)) {
        package.close();
        args->mark(BootstrapOperation_Launch);
        args->result = args->pm.result;
        return;
    }
    if (!args->exchanged || !args->pm.completed) {
        package.close();
        args->mark(failed_stage);
        args->result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return;
    }

    _launchStartProcess(args);
    if (R_SUCCEEDED(args->result)) {
        if (plugin && args->gate_restore_pending) {
            args->mark(BootstrapOperation_Restore);
            args->result = KERNELRESULT(TimedOut);
        } else {
            _launchConnectServices(args, plugin);
        }
    }
    if (R_SUCCEEDED(args->result)) {
        args->mark(BootstrapOperation_Complete);
    }
    package.close();
}

static bool _launchPluginPath(char* out, size_t capacity,
                              const char* prefix, const char* name,
                              const char* suffix) {
    const size_t a = __builtin_strlen(prefix);
    const size_t b = __builtin_strlen(name);
    const size_t c = __builtin_strlen(suffix);
    if (a + b + c + 1 > capacity) {
        return false;
    }
    __builtin_memcpy(out, prefix, a);
    __builtin_memcpy(out + a, name, b);
    __builtin_memcpy(out + a + b, suffix, c + 1);
    return true;
}

static void _launchRecordPlugin(LaunchState* args, const char* name, PluginOutcome outcome) {
    if (args->plugin_record_count == LaunchState::MaxPluginRecords)
        return;
    LaunchState::PluginRecord& record = args->plugin_records[args->plugin_record_count++];
    record = {};
    for (size_t index = 0; index < HEN_PLUGIN_NAME_SIZE - 1 && name[index]; index++)
        record.name.name[index] = name[index];
    record.outcome = outcome;
    record.stage = args->stage;
    record.result = args->result;
}

/// Starts the plugin /kaihen/plugins/<name>.nso. A plugin whose name is
/// too long or whose package cannot be written is skipped: nothing of it has
/// run. Any later failure sets args->result and stops startup.
static void _launchPlugin(LaunchState* args, ounce::CompilerTlsScope& tls,
                          const SignedPolicy& policy, const char* name) {
    char executable[FS_MAX_PATH];
    char package[FS_MAX_PATH];
    char package_name[FS_MAX_PATH];
    if (__builtin_strlen(name) >= HEN_PLUGIN_NAME_SIZE ||
        !_launchPluginPath(executable, sizeof(executable),
                           "/kaihen/plugins/", name, ".nso") ||
        !_launchPluginPath(package, sizeof(package),
                           "/kaihen/plugin-packages/", name, ".nsp") ||
        !_launchPluginPath(package_name, sizeof(package_name),
                           "@Sdcard:/kaihen/plugin-packages/", name, ".nsp")) {
        _launchRecordPlugin(args, name, PluginOutcome_NameTooLong);
        return;
    }

    args->current_plugin = {};
    __builtin_memcpy(args->current_plugin.name, name, __builtin_strlen(name));

    args->mark(BootstrapOperation_PluginPackage);
    args->result = writeExeFsPackage(args->input.fs_root_session,
                                     executable, package, policy);
    if (R_FAILED(args->result)) {
        _launchRecordPlugin(args, name, PluginOutcome_PackageFailed);
        args->result = 0;
        return;
    }

    args->current_outcome = PluginOutcome_Running;
    _launchOne(args, tls, package_name, true);
    _launchRecordPlugin(args, name,
        R_FAILED(args->result) ? PluginOutcome_StartFailed : args->current_outcome);
}

/// Makes sure that /kaihen/plugins exists as a directory, and creates
/// /kaihen/plugin-packages. `found` is false when there is no plugin
/// directory, which is not an error.
static Result _launchPreparePluginDirectories(FsFileSystem* sd, bool* found) {
    *found = false;
    FsDirEntryType type;
    if (R_FAILED(fsFsGetEntryType(sd, "/kaihen/plugins", &type)))
        return 0;
    if (type != FsDirEntryType_Dir)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *found = true;

    Result rc = fsFsGetEntryType(sd, "/kaihen/plugin-packages", &type);
    if (R_SUCCEEDED(rc))
        return type == FsDirEntryType_Dir ? 0 : MAKERESULT(Module_Libnx, LibnxError_BadInput);

    rc = fsFsCreateDirectory(sd, "/kaihen/plugin-packages");
    if (R_SUCCEEDED(rc))
        rc = fsFsCommit(sd);
    return rc;
}

/// Writes `value` as 8 hexadecimal digits.
static char* _launchWriteHex(char* out, u32 value) {
    for (int shift = 28; shift >= 0; shift -= 4)
        *out++ = "0123456789abcdef"[(value >> shift) & 0xf];
    return out;
}

static char* _launchWriteText(char* out, const char* end, const char* text) {
    while (*text && out < end)
        *out++ = *text++;
    return out;
}

static const char* _launchPluginOutcomeText(PluginOutcome outcome) {
    switch (outcome) {
    case PluginOutcome_Running:         return "running";
    case PluginOutcome_Published:       return "running; HEN lists its service";
    case PluginOutcome_NotReady:        return "running, but not ready after 5 seconds; its service is not listed";
    case PluginOutcome_TooManyServices: return "running; its service is not listed, because HEN lists 16 already";
    case PluginOutcome_NameTooLong:     return "not started: the file name is too long";
    case PluginOutcome_PackageFailed:   return "not started: its package could not be written";
    case PluginOutcome_StartFailed:     return "failed to start; the plugins after it were not started";
    }
    return "unknown";
}

/// Writes /kaihen/plugin-status.txt: one line for each plugin, with the
/// stage and result when it was not started. A failure to write it does not
/// stop startup.
static void _launchWritePluginStatus(LaunchState* args) {
    char text[4096];
    char* out = text;
    const char* end = text + sizeof(text) - 1;
    for (u32 index = 0; index < args->plugin_record_count; index++) {
        const LaunchState::PluginRecord& record = args->plugin_records[index];
        out = _launchWriteText(out, end, record.name.name);
        out = _launchWriteText(out, end, ": ");
        out = _launchWriteText(out, end, _launchPluginOutcomeText(record.outcome));
        if (R_FAILED(record.result) && end - out > 40) {
            out = _launchWriteText(out, end, " (stage ");
            out = _launchWriteHex(out, record.stage);
            out = _launchWriteText(out, end, ", result 0x");
            out = _launchWriteHex(out, record.result);
            out = _launchWriteText(out, end, ")");
        }
        out = _launchWriteText(out, end, "\n");
    }

    SdWriter writer;
    const u64 size = static_cast<u64>(out - text);
    if (R_SUCCEEDED(writer.begin(args->input.fs_root_session, "/kaihen/plugin-status.txt", size)) &&
        R_SUCCEEDED(writer.write(text, size)))
        writer.finish();
}

static void _launchPlugins(LaunchState* args, ounce::CompilerTlsScope& tls,
                           const SignedPolicy& policy) {
    Service root = {};
    serviceCreate(&root, args->input.fs_root_session);
    root.own_handle = false;
    FsFileSystem sd = {};
    FsDir directory = {};
    bool found = false;

    args->mark(BootstrapOperation_PluginScan);
    args->result = serviceDispatch(&root, 18,
        .out_num_objects = 1,
        .out_objects = &sd.s,
    );
    if (R_FAILED(args->result))
        return;

    args->result = _launchPreparePluginDirectories(&sd, &found);
    if (R_FAILED(args->result) || !found)
        goto close_sd;

    args->result = fsFsOpenDirectory(&sd, "/kaihen/plugins", FsDirOpenMode_ReadFiles, &directory);
    if (R_FAILED(args->result))
        goto close_sd;

    for (;;) {
        FsDirectoryEntry entry = {};
        s64 count = 0;
        args->result = fsDirRead(&directory, &count, 1, &entry);
        if (R_FAILED(args->result) || !count)
            break;

        const size_t length = __builtin_strlen(entry.name);
        if (entry.type != FsDirEntryType_File || length < 5 ||
            __builtin_strcmp(entry.name + length - 4, ".nso"))
            continue;
        entry.name[length - 4] = '\0';

        _launchPlugin(args, tls, policy, entry.name);
        if (R_FAILED(args->result))
            break;
    }
    fsDirClose(&directory);
    _launchWritePluginStatus(args);

close_sd:
    fsFsClose(&sd);
}

static void _launchRun(LaunchState* args, ounce::CompilerTlsScope& tls) {
    args->mark(BootstrapOperation_Begin);
    args->result = _launchOpenProcessReader(args);
    if (R_FAILED(args->result)) {
        return;
    }
    SignedPolicy policy = {};
    args->mark(BootstrapOperation_ReadMigrationPolicy);
    args->result = readSignedPolicy(args->input.loader_root_session,
        args->pm.program_id, NcmStorageId_BuiltInSystem, 1, &policy);
    if (R_FAILED(args->result)) {
        return;
    }
    args->mark(BootstrapOperation_WriteMigrationPackage);
    args->result = writeExeFsPackage(args->input.fs_root_session,
        "/kaihen/bin/hen/main", "/kaihen/bin/hen/exefs.nsp", policy);
    if (R_FAILED(args->result)) {
        return;
    }
    _launchOne(args, tls, args->input.package_path, false);
    if (R_FAILED(args->result)) {
        return;
    }
    // A plugin that fails to start stops the plugins after it (it may still
    // hold mig:usr), not the startup: HEN is running already.
    _launchPlugins(args, tls, policy);
    args->result = 0;
    _launchFinishStartup(args);
    if (R_SUCCEEDED(args->result)) {
        args->mark(BootstrapOperation_Complete);
    }
}

extern "C" __attribute__((section(".text.entry"))) void* bootstrapMain(void* argument) {
    auto* args = static_cast<BootstrapArgs*>(argument);
    const BootstrapInput& input = args->input;
    LaunchState state{input};
    args->output = {};
    state.report = &args->output;
    state.pm.shell_session = input.shell_session;
    state.pm.monitor_session = input.monitor_session;
    state.pm.program_id = input.program_id;
    state.pm.flags = input.flags;
    state.pm.storage_id = input.storage_id;
    {
        ounce::CompilerTlsScope tls;
        _launchRun(&state, tls);
    }
    if (state.reader_session != INVALID_HANDLE) {
        svcCloseHandle(state.reader_session);
    }
    args->output = {
        state.stage, state.result, state.pm.pid,
        state.gate_restore_result, state.channel_restore_result,
    };
    return reinterpret_cast<void*>(static_cast<uintptr_t>(args->output.result));
}
