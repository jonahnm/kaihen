// SPDX-License-Identifier: GPL-2.0-only
// The AArch64 bridge stub that HEN writes into FS code. The stub saves the
// integer argument registers, asks HEN to run the plugin handler, and can
// call the original function through its trampoline before running the
// handler's AFTER phase.
#pragma once

#include <cstdint>
#include "inline_hook.hpp"

namespace ounce::hook {

/// Code plus the parameter block that the stub reads at StubDataOffset.
constexpr uint32_t StubDataOffset = 0x2e0;
constexpr uint32_t StubSize = StubDataOffset + 32;
constexpr uint32_t StubTrampolineOffset = 0x300;
constexpr uint32_t StubSlotSize = 0x400;
/// The port name in the stub's parameter block.
constexpr uint32_t StubPortOffset = 24;
constexpr uint32_t StubPortSize = 8;

/// Emits the bridge stub for one hook. @p out must hold StubSize bytes.
/// The caller writes it at the beginning of a StubSlotSize slot and writes
/// the trampoline at slot + StubTrampolineOffset.
bool BuildStub(uint32_t hook_id, uint64_t trampoline, uint8_t* out,
               uint32_t capacity);

} // namespace ounce::hook
