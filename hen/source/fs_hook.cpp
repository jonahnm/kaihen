// SPDX-License-Identifier: GPL-2.0-only
#include "fs_hook.hpp"

#include <cstdint>
#include <cstring>
#include "../../common/result.hpp"

namespace ounce {
namespace {

constexpr Result NotInitialized = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
constexpr Result BadInput = MAKERESULT(Module_Libnx, LibnxError_BadInput);
constexpr Result OutOfMemory = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
constexpr Result NotFound = MAKERESULT(Module_Libnx, LibnxError_NotFound);
constexpr Result AlreadyInstalled =
    MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);

bool hasBackend(const FsHookBackend& backend) {
    return backend.read && backend.write && backend.replace && backend.allocate;
}

Result translate(hook::Status status) {
    switch (status) {
    case hook::Status::Ok:
        return 0;
    case hook::Status::TrampolineFull:
        return OutOfMemory;
    default:
        return BadInput;
    }
}

} // namespace

FsHookManager::Entry* FsHookManager::find(u32 id) {
    for (u32 index = 0; index < count_; index++) {
        if (entries_[index].info.id == id)
            return &entries_[index];
    }
    return nullptr;
}

const FsHookManager::Entry* FsHookManager::find(u32 id) const {
    for (u32 index = 0; index < count_; index++) {
        if (entries_[index].info.id == id)
            return &entries_[index];
    }
    return nullptr;
}

Result FsHookManager::install(const HenFsHookRequest& request, u64 module_base,
                              u32 owner, HenFsHookInfo* out) {
    R_UNLESS(out, BadInput);
    R_UNLESS(hasBackend(backend_), NotInitialized);
    R_UNLESS(module_base && owner && request.handler_id, BadInput);
    R_UNLESS(request.flags == 0, BadInput);
    R_UNLESS(count_ < MaxHooks, OutOfMemory);
    R_UNLESS(request.target_offset <= UINT64_MAX - module_base, BadInput);

    const u64 target = hook::StripPac(module_base + request.target_offset);
    R_UNLESS(target && !(target & 3), BadInput);
    for (u32 index = 0; index < count_; index++) {
        if (entries_[index].info.target == target)
            return AlreadyInstalled;
    }

    u64 slot = 0;
    R_TRY(backend_.allocate(backend_.context, hook::StubSlotSize, &slot));
    R_UNLESS(slot && !(slot & 3), BadInput);

    u8 code[hook::MaxReadSize] = {};
    Result rc = backend_.read(backend_.context, target, code, sizeof(code));
    // A function without a BTI landing pad needs only MaxPatchSize bytes;
    // an entry at the very end of a code block may not expose the rest.
    if (R_FAILED(rc))
        rc = backend_.read(backend_.context, target, code, hook::MaxPatchSize);
    hook::Plan plan;
    if (R_SUCCEEDED(rc)) {
        const hook::Status status = hook::Build(target, slot, slot + hook::StubTrampolineOffset,
                                                code, sizeof(code), &plan);
        rc = translate(status);
    }
    if (R_FAILED(rc)) {
        if (backend_.release)
            backend_.release(backend_.context, slot, hook::StubSlotSize);
        return rc;
    }

    u8 stub[hook::StubSize] = {};
    uint32_t id = next_id_;
    if (!hook::BuildStub(id, plan.trampoline, stub, sizeof(stub))) {
        if (backend_.release)
            backend_.release(backend_.context, slot, hook::StubSlotSize);
        return OutOfMemory;
    }

    rc = backend_.write(backend_.context, slot, stub, sizeof(stub));
    if (R_SUCCEEDED(rc)) {
        rc = backend_.write(backend_.context, plan.trampoline,
                            plan.trampoline_bytes, plan.trampoline_size);
    }
    bool patched = false;
    if (R_SUCCEEDED(rc)) {
        rc = backend_.replace(backend_.context, plan.entry,
                              plan.original_bytes, plan.patch_bytes,
                              plan.patch_size);
        patched = R_SUCCEEDED(rc);
    }
    if (R_SUCCEEDED(rc)) {
        // A write that did not take effect must not be listed as a hook.
        u8 observed[hook::MaxPatchSize] = {};
        rc = backend_.read(backend_.context, plan.entry, observed,
                           plan.patch_size);
        if (R_SUCCEEDED(rc) &&
            std::memcmp(observed, plan.patch_bytes, plan.patch_size))
            rc = BadInput;
    }
    if (R_FAILED(rc)) {
        if (patched) {
            backend_.replace(backend_.context, plan.entry, plan.patch_bytes,
                             plan.original_bytes, plan.patch_size);
        }
        if (backend_.release)
            backend_.release(backend_.context, slot, hook::StubSlotSize);
        return rc;
    }

    Entry& entry = entries_[count_++];
    entry = {};
    entry.info.id = next_id_++;
    if (!next_id_)
        next_id_ = 1;
    entry.info.state = HEN_FS_HOOK_INSTALLED;
    entry.info.target = plan.target;
    entry.info.replacement = slot;
    entry.info.trampoline = plan.trampoline;
    entry.entry = plan.entry;
    entry.slot = slot;
    entry.trampoline = plan.trampoline;
    entry.slot_size = hook::StubSlotSize;
    entry.owner = owner;
    entry.handler_id = request.handler_id;
    entry.patch_size = plan.patch_size;
    std::memcpy(entry.patch, plan.patch_bytes, plan.patch_size);
    std::memcpy(entry.original, plan.original_bytes, plan.patch_size);
    *out = entry.info;
    return 0;
}

Result FsHookManager::remove(u32 id) {
    R_UNLESS(hasBackend(backend_), NotInitialized);
    Entry* entry = find(id);
    R_UNLESS(entry, NotFound);

    R_TRY(backend_.replace(backend_.context, entry->entry,
                           entry->patch, entry->original, entry->patch_size));
    if (backend_.release)
        backend_.release(backend_.context, entry->slot, entry->slot_size);

    const u32 index = static_cast<u32>(entry - entries_);
    for (u32 next = index; next + 1 < count_; next++)
        entries_[next] = entries_[next + 1];
    entries_[--count_] = {};
    return 0;
}

Result FsHookManager::info(u32 index, HenFsHookInfo* out) const {
    R_UNLESS(out, BadInput);
    R_UNLESS(index < count_, NotFound);
    *out = entries_[index].info;
    return 0;
}

Result FsHookManager::callInfo(u32 id, u32* out_owner, u32* out_handler_id) const {
    R_UNLESS(out_owner && out_handler_id, BadInput);
    const Entry* entry = find(id);
    R_UNLESS(entry, NotFound);
    *out_owner = entry->owner;
    *out_handler_id = entry->handler_id;
    return 0;
}

} // namespace ounce
