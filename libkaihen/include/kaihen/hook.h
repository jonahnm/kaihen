/* SPDX-License-Identifier: ISC
 * Copyright 2026 kaiHEN contributors
 *
 * The FS hook interface that only plugins receive. HEN creates a plugin
 * session for each plugin during the bootstrap's handoff and serves these
 * commands on it alone: the homebrew interface (protocol.h) does not have
 * them. A target and a replacement are offsets from the base of the FS
 * module; HEN resolves them in the FS process, replaces the first
 * instructions of the target with a branch to the replacement, and keeps the
 * original instructions in a trampoline that the plugin can call.
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
} HenPluginCommand;

/// A request to hook an FS function.
typedef struct {
    u64 target_offset;      ///< The FS function entry's offset from the module base.
    u64 replacement_offset; ///< The replacement's offset from the module base.
    u32 flags;              ///< Reserved; must be 0.
    u32 reserved;
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
    u64 replacement; ///< The resolved, PAC-stripped branch destination address.
    u64 trampoline;  ///< Writable FS code that runs the original prologue and jumps back.
} HenFsHookInfo;

#define HEN_FS_HOOK_INSTALLED 1

#ifdef __cplusplus
static_assert(sizeof(HenFsHookInfo) == 32, "FS hook info layout");
#else
_Static_assert(sizeof(HenFsHookInfo) == 32, "FS hook info layout");
#endif
