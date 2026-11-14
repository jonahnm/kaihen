// SPDX-License-Identifier: GPL-2.0-only
// HEN's inline hooks in FS. The manager builds the patches with
// inline_hook.hpp and writes them through a backend: on the console the
// ro:dmnt endpoint of RO, in host tests a byte buffer. Only plugin sessions
// reach the manager (server.cpp).
#pragma once

#include <switch/result.h>
#include <switch/types.h>

#include "../../libkaihen/include/kaihen/hook.h"
#include "fs_hook_stub.hpp"
#include "inline_hook.hpp"

namespace ounce {

/// FS code memory, as the hook manager uses it.
struct FsHookBackend {
    void* context = nullptr;
    Result (*read)(void* context, u64 address, void* out, u32 size) = nullptr;
    Result (*write)(void* context, u64 address, const void* bytes, u32 size) = nullptr;
    /// Writes only when the bytes at the address are still @p expected, and
    /// reads the result back.
    Result (*replace)(void* context, u64 address, const void* expected,
                      const void* replacement, u32 size) = nullptr;
    /// Returns writable executable FS code of at least @p size bytes.
    Result (*allocate)(void* context, u32 size, u64* out_address) = nullptr;
    void (*release)(void* context, u64 address, u32 size) = nullptr;
};

class FsHookManager {
  public:
    static constexpr u32 MaxHooks = 16;

    FsHookManager() = default;
    FsHookManager(const FsHookManager&) = delete;
    FsHookManager& operator=(const FsHookManager&) = delete;

    void setBackend(const FsHookBackend& backend) { backend_ = backend; }

    /// Installs a hook at request.target_offset from @p module_base. The
    /// target branches to a bridge stub; @p owner is the plugin record token
    /// that the handler is registered under.
    Result install(const HenFsHookRequest& request, u64 module_base, u32 owner,
                   HenFsHookInfo* out);

    /// Finds the plugin owner and handler ID of an installed hook.
    Result callInfo(u32 id, u32* out_owner, u32* out_handler_id) const;

    /// Restores the target's original instructions and frees its trampoline.
    Result remove(u32 id);

    u32 count() const { return count_; }

    /// Returns the hook at @p index in installation order.
    Result info(u32 index, HenFsHookInfo* out) const;

  private:
    struct Entry {
        HenFsHookInfo info = {};
        u64 entry = 0;       ///< target, or the first instruction after its BTI.
        u64 slot = 0;        ///< The bridge stub's address in the hook arena.
        u64 trampoline = 0;  ///< slot + hook::StubTrampolineOffset.
        u32 slot_size = 0;
        u32 owner = 0;       ///< The plugin record token.
        u32 handler_id = 0;  ///< The plugin runtime's handler ID.
        u32 patch_size = 0;
        u8 patch[hook::MaxPatchSize] = {};
        u8 original[hook::MaxPatchSize] = {};
    };

    Entry* find(u32 id);
    const Entry* find(u32 id) const;

    FsHookBackend backend_ = {};
    Entry entries_[MaxHooks] = {};
    u32 count_ = 0;
    u32 next_id_ = 1;
};

} // namespace ounce
