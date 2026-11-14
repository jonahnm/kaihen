/* SPDX-License-Identifier: ISC
 * Copyright 2026 kaiHEN contributors
 *
 * FS hooks that only plugins receive. HEN creates a plugin session for each
 * plugin during the bootstrap's handoff and serves these commands on it
 * alone: the homebrew interface (protocol.h) does not have them. The target
 * is an offset from the base of the FS module.
 *
 * HEN patches the target to branch to a small bridge stub in FS. The stub
 * calls the plugin through an IPC callback session, so the replacement
 * function runs in the plugin's process. A handler runs in two phases:
 *
 *   BEFORE: inspect or replace the arguments. Return
 *           HEN_FS_HOOK_CALL_ORIGINAL to run the original FS function.
 *   AFTER:  runs only for a hook that requested the original. The original
 *           return registers are in context->regs, and the handler may
 *           replace them.
 *
 * The original runs through the trampoline that HEN writes into FS code,
 * and returns to the bridge stub. It is not a cross-process call.
 *
 * The same session copies bytes between the plugin and FS memory
 * (HenPluginCommand_CopyToFs and _CopyFromFs), so a handler can read the
 * data behind the FS addresses it receives without translating addresses.
 */
#pragma once

#include <switch/types.h>

/// HEN's commands on a plugin session. They start above the status commands
/// that every HEN session answers (0 and 8).
typedef enum {
    /// Takes HenFsHookRequest, returns HenFsHookInfo.
    HenPluginCommand_InstallFsHook = 64,
    /// Takes the u32 ID of an installed hook.
    HenPluginCommand_RemoveFsHook = 65,
    /// Returns the number of installed hooks as a u32.
    HenPluginCommand_GetFsHookCount = 66,
    /// Takes a u32 index and returns HenFsHookInfo.
    HenPluginCommand_GetFsHookInfo = 67,
    /// Takes HenFsCopyRequest; copies its bytes into FS memory.
    HenPluginCommand_CopyToFs = 68,
    /// Takes HenFsCopyRequest; returns HenFsCopyReply with bytes from FS memory.
    HenPluginCommand_CopyFromFs = 69,
} HenPluginCommand;

/// The command that HEN sends to a plugin's hook callback session.
#define HEN_PLUGIN_HOOK_CALL 0

/// The command that the FS bridge stub sends to HEN, as a TIPC command.
#define HEN_FS_HOOK_CALL_COMMAND 0

/// The pointer-sized registers that a hook handler can inspect and change.
#define HEN_FS_HOOK_REGISTER_COUNT 9

/// Handler phases. See the file comment.
#define HEN_FS_HOOK_BEFORE 0
#define HEN_FS_HOOK_AFTER 1

/// Handler actions.
#define HEN_FS_HOOK_RETURN 0
#define HEN_FS_HOOK_CALL_ORIGINAL 1

/// A request to hook an FS function.
typedef struct {
    u64 target_offset; ///< The FS function entry's offset from the module base.
    u32 handler_id;    ///< The plugin runtime's ID for the handler function.
    u32 flags;         ///< Reserved; must be 0.
    u64 reserved;
} HenFsHookRequest;

#ifdef __cplusplus
static_assert(sizeof(HenFsHookRequest) == 24, "FS hook request layout");
#else
_Static_assert(sizeof(HenFsHookRequest) == 24, "FS hook request layout");
#endif

/// An installed FS hook.
typedef struct {
    u32 id;          ///< The ID that RemoveFsHook takes.
    u32 state;       ///< HEN_FS_HOOK_INSTALLED.
    u64 target;      ///< The resolved, PAC-stripped function entry address.
    u64 replacement; ///< The bridge stub's address in FS.
    u64 trampoline;  ///< Writable FS code that runs the original prologue and jumps back.
} HenFsHookInfo;

#define HEN_FS_HOOK_INSTALLED 1

#ifdef __cplusplus
static_assert(sizeof(HenFsHookInfo) == 32, "FS hook info layout");
#else
_Static_assert(sizeof(HenFsHookInfo) == 32, "FS hook info layout");
#endif

/// The FS memory bytes that one copy command carries. That is what fits the
/// 0x100-byte message buffer together with the CMIF headers; larger copies
/// are consecutive commands.
#define HEN_FS_COPY_CHUNK_SIZE 192

/// One chunk of a copy between the plugin and FS memory.
typedef struct {
    u64 address;  ///< The FS address to copy from or to.
    u32 size;     ///< Bytes in data; at most HEN_FS_COPY_CHUNK_SIZE.
    u32 reserved; ///< Reserved; must be 0.
    u8 data[HEN_FS_COPY_CHUNK_SIZE];
} HenFsCopyRequest;

#ifdef __cplusplus
static_assert(sizeof(HenFsCopyRequest) == 208, "FS copy request layout");
#else
_Static_assert(sizeof(HenFsCopyRequest) == 208, "FS copy request layout");
#endif

/// The reply of HenPluginCommand_CopyFromFs: the first request.size bytes.
typedef struct {
    u8 data[HEN_FS_COPY_CHUNK_SIZE];
} HenFsCopyReply;

#ifdef __cplusplus
static_assert(sizeof(HenFsCopyReply) == 192, "FS copy reply layout");
#else
_Static_assert(sizeof(HenFsCopyReply) == 192, "FS copy reply layout");
#endif

/// The call that the FS bridge stub sends to HEN.
typedef struct {
    u32 hook_id;
    u32 phase;
    u64 regs[HEN_FS_HOOK_REGISTER_COUNT]; ///< x0-x8 at the hooked entry.
} HenFsHookCall;

#ifdef __cplusplus
static_assert(sizeof(HenFsHookCall) == 80, "FS hook call layout");
#else
_Static_assert(sizeof(HenFsHookCall) == 80, "FS hook call layout");
#endif

/// HEN's reply to the FS bridge stub.
typedef struct {
    u32 action; ///< HEN_FS_HOOK_RETURN or HEN_FS_HOOK_CALL_ORIGINAL.
    u32 result; ///< The handler's Result, for diagnostics.
    u64 regs[HEN_FS_HOOK_REGISTER_COUNT];
} HenFsHookResult;

#ifdef __cplusplus
static_assert(sizeof(HenFsHookResult) == 80, "FS hook result layout");
#else
_Static_assert(sizeof(HenFsHookResult) == 80, "FS hook result layout");
#endif

/// The call that HEN sends to the plugin's hook callback session.
typedef struct {
    u32 hook_id;
    u32 handler_id;
    u32 phase;
    u32 reserved;
    u64 regs[HEN_FS_HOOK_REGISTER_COUNT];
} HenFsHookEvent;

#ifdef __cplusplus
static_assert(sizeof(HenFsHookEvent) == 88, "FS hook event layout");
#else
_Static_assert(sizeof(HenFsHookEvent) == 88, "FS hook event layout");
#endif

/// The plugin's reply to HEN's hook callback.
typedef struct {
    u32 action;
    u32 result;
    u64 regs[HEN_FS_HOOK_REGISTER_COUNT];
} HenFsHookReply;

#ifdef __cplusplus
static_assert(sizeof(HenFsHookReply) == 80, "FS hook reply layout");
#else
_Static_assert(sizeof(HenFsHookReply) == 80, "FS hook reply layout");
#endif

/// The context that a plugin handler receives.
typedef struct {
    u32 hook_id;
    u32 handler_id;
    u32 phase;  ///< HEN_FS_HOOK_BEFORE or HEN_FS_HOOK_AFTER.
    u32 action; ///< Set to HEN_FS_HOOK_CALL_ORIGINAL to run the original.
    u64 regs[HEN_FS_HOOK_REGISTER_COUNT]; ///< x0-x8.
    void* user; ///< The pointer passed to pluginFsHookInstall.
} HenFsHookContext;

#ifdef __cplusplus
static_assert(sizeof(HenFsHookContext) == 96, "FS hook context layout");
#else
_Static_assert(sizeof(HenFsHookContext) == 96, "FS hook context layout");
#endif
