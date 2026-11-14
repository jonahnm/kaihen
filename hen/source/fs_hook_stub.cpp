// SPDX-License-Identifier: GPL-2.0-only
#include "fs_hook_stub.hpp"

#include <cstring>
#include "../../libkaihen/include/kaihen/hook.h"

namespace ounce::hook {
namespace {

constexpr uint32_t FrameSize = 0x200;
/// A far target reaches the stub with an indirect branch, so it needs a
/// landing pad. A near target's direct branch treats this as a hint.
constexpr uint32_t BtiJ = 0xD503249Fu;
constexpr uint32_t SavedX0 = 0x00;
constexpr uint32_t SavedX18 = 0x48;
constexpr uint32_t SavedX30 = 0x50;
constexpr uint32_t SavedLR = 0xd0;
constexpr uint32_t TlsSave = 0x100;
constexpr uint32_t TlsSize = 0x100;
constexpr uint32_t TlsWords = TlsSize / 8;
constexpr uint32_t RequestWords = 20; // HenFsHookCall: 80 bytes.
// svcSendSyncRequest reads and writes the message at the thread's TLS.
// The TIPC header is the first 8 bytes; the payload follows it.
constexpr uint32_t TlsRequest = 0;
constexpr uint32_t TlsResult = 8;
constexpr uint32_t TlsAction = 16;
constexpr uint32_t TlsRegisters = 24;

enum Label : uint32_t {
    LabelSessionOk,
    LabelCallOriginal,
    LabelReturn,
    LabelReturnOriginal,
    LabelFallback,
    LabelRestore,
    LabelTlsSaveLoop,
    LabelRestoreTls,
    LabelRestoreTlsLoop,
    LabelSend,
    LabelSendFail,
    LabelCount,
};

struct Patch {
    uint32_t word = 0;
    uint32_t label = 0;
    uint32_t base = 0;
    uint32_t shift = 0;
    uint32_t mask = 0;
};

class Emitter {
  public:
    explicit Emitter(uint32_t capacity) : capacity_(capacity) {}

    bool Word(uint32_t value) {
        if (size_ + sizeof(value) > capacity_) {
            overflow_ = true;
            return false;
        }
        std::memcpy(bytes_ + size_, &value, sizeof(value));
        size_ += sizeof(value);
        return true;
    }

    uint32_t pc() const { return size_; }
    uint32_t size() const { return size_; }
    bool overflow() const { return overflow_; }

    void Mark(Label label) { labels_[label] = pc(); }

    void Adr(uint32_t rd, int64_t delta) {
        const uint64_t value = static_cast<uint64_t>(delta);
        const uint32_t immlo = static_cast<uint32_t>(value & 3);
        const uint32_t immhi = static_cast<uint32_t>((value >> 2) & 0x7ffff);
        Word(0x10000000u | (immlo << 29) | (immhi << 5) | rd);
    }

    void B(Label label) {
        Patch patch{pc() / 4, label, 0x14000000u, 0, 0x03ffffffu};
        AddPatch(patch);
        Word(patch.base);
    }

    void Bne(Label label) {
        Patch patch{pc() / 4, label, 0x54000001u, 5, 0x7ffffu};
        AddPatch(patch);
        Word(patch.base);
    }

    void CbzX(uint32_t rt, Label label) {
        Patch patch{pc() / 4, label, 0xB4000000u | rt, 5, 0x7ffffu};
        AddPatch(patch);
        Word(patch.base);
    }

    void CbnzX(uint32_t rt, Label label) {
        Patch patch{pc() / 4, label, 0xB5000000u | rt, 5, 0x7ffffu};
        AddPatch(patch);
        Word(patch.base);
    }

    void CbnzW(uint32_t rt, Label label) {
        Patch patch{pc() / 4, label, 0x35000000u | rt, 5, 0x7ffffu};
        AddPatch(patch);
        Word(patch.base);
    }

    void Bl(Label label) {
        Patch patch{pc() / 4, label, 0x94000000u, 0, 0x03ffffffu};
        AddPatch(patch);
        Word(patch.base);
    }

    bool Finish() {
        if (overflow_)
            return false;
        for (uint32_t index = 0; index < patch_count_; index++) {
            const Patch& patch = patches_[index];
            const uint32_t target = labels_[patch.label];
            const int64_t delta = static_cast<int64_t>(target) -
                                  static_cast<int64_t>(patch.word * 4);
            if ((delta & 3) || delta < -(1ll << 25) || delta >= (1ll << 25))
                return false;
            const uint32_t immediate =
                static_cast<uint32_t>((static_cast<uint64_t>(delta) >> 2) & patch.mask);
            uint32_t instruction = 0;
            std::memcpy(&instruction, bytes_ + patch.word * 4, sizeof(instruction));
            instruction = patch.base | (immediate << patch.shift);
            std::memcpy(bytes_ + patch.word * 4, &instruction, sizeof(instruction));
        }
        return true;
    }

    const uint8_t* bytes() const { return bytes_; }

  private:
    void AddPatch(const Patch& patch) {
        if (patch_count_ == sizeof(patches_) / sizeof(patches_[0])) {
            overflow_ = true;
            return;
        }
        patches_[patch_count_++] = patch;
    }

    uint8_t bytes_[StubDataOffset] = {};
    uint32_t capacity_;
    uint32_t size_ = 0;
    bool overflow_ = false;
    Patch patches_[32] = {};
    uint32_t patch_count_ = 0;
    uint32_t labels_[LabelCount] = {};
};

void StrX(Emitter& out, uint32_t rt, uint32_t rn, uint32_t offset) {
    out.Word(0xF9000000u | ((offset / 8) << 10) | (rn << 5) | rt);
}

void LdrX(Emitter& out, uint32_t rt, uint32_t rn, uint32_t offset) {
    out.Word(0xF9400000u | ((offset / 8) << 10) | (rn << 5) | rt);
}

void StrW(Emitter& out, uint32_t rt, uint32_t rn, uint32_t offset) {
    out.Word(0xB9000000u | ((offset / 4) << 10) | (rn << 5) | rt);
}

void LdrW(Emitter& out, uint32_t rt, uint32_t rn, uint32_t offset) {
    out.Word(0xB9400000u | ((offset / 4) << 10) | (rn << 5) | rt);
}

void AddX(Emitter& out, uint32_t rd, uint32_t rn, uint32_t immediate) {
    out.Word(0x91000000u | (immediate << 10) | (rn << 5) | rd);
}

void SubSp(Emitter& out, uint32_t immediate) {
    out.Word(0xD1000000u | (immediate << 10) | (31 << 5) | 31);
}

void AddSp(Emitter& out, uint32_t immediate) {
    out.Word(0x91000000u | (immediate << 10) | (31 << 5) | 31);
}

void MovzW(Emitter& out, uint32_t rd, uint32_t immediate) {
    out.Word(0x52800000u | (immediate << 5) | rd);
}

void Svc(Emitter& out, uint32_t immediate) {
    out.Word(0xD4000001u | (immediate << 5));
}

void MrsTls(Emitter& out, uint32_t rt) {
    out.Word(0xD53BD060u | (rt & 31)); // MRS Xt, TPIDRRO_EL0
}

void LdrPostX(Emitter& out, uint32_t rt, uint32_t rn) {
    out.Word(0xF8408400u | (rn << 5) | (rt & 31)); // LDR Xt, [Xn], #8
}

void StrPostX(Emitter& out, uint32_t rt, uint32_t rn) {
    out.Word(0xF8008400u | (rn << 5) | (rt & 31)); // STR Xt, [Xn], #8
}

void SubsW(Emitter& out, uint32_t rd, uint32_t rn, uint32_t immediate) {
    out.Word(0x71000000u | (immediate << 10) | (rn << 5) | rd);
}

void CmpXImm(Emitter& out, uint32_t rt, uint32_t immediate) {
    out.Word(0xF1000000u | (immediate << 10) | (rt << 5) | 31);
}

void SaveTls(Emitter& out, Label loop) {
    MrsTls(out, 9);
    AddX(out, 10, 31, TlsSave);
    MovzW(out, 11, TlsWords);
    out.Mark(loop);
    LdrPostX(out, 12, 9);
    StrPostX(out, 12, 10);
    SubsW(out, 11, 11, 1);
    out.Bne(loop);
}

void Restore(Emitter& out) {
    for (uint32_t rt = 0; rt < 9; rt++)
        LdrX(out, rt, 31, SavedX0 + rt * 8);
    LdrX(out, 18, 31, SavedX18);
    LdrX(out, 30, 31, SavedX30);
    AddSp(out, FrameSize);
}

} // namespace

bool BuildStub(uint32_t hook_id, uint64_t trampoline, uint8_t* out,
               uint32_t capacity) {
    if (!out || capacity < StubSize)
        return false;
    std::memset(out, 0, StubSize);

    Emitter code(StubDataOffset);
    code.Word(BtiJ);
    SubSp(code, FrameSize);
    SaveTls(code, LabelTlsSaveLoop);
    for (uint32_t rt = 0; rt < 9; rt++)
        StrX(code, rt, 31, SavedX0 + rt * 8);
    StrX(code, 18, 31, SavedX18);
    StrX(code, 30, 31, SavedX30);

    // The first call opens the named port and keeps the session in the
    // parameter block. A later call reuses it.
    code.Adr(9, static_cast<int64_t>(StubDataOffset) - code.pc());
    LdrX(code, 10, 9, 8);
    code.CbnzX(10, LabelSessionOk);
    AddX(code, 0, 9, 8);
    AddX(code, 1, 9, StubPortOffset);
    Svc(code, 0x1f);
    code.CbnzX(0, LabelFallback);
    code.Adr(9, static_cast<int64_t>(StubDataOffset) - code.pc());
    LdrX(code, 10, 9, 8);
    code.CbzX(10, LabelFallback);
    code.Mark(LabelSessionOk);

    // BEFORE: the plugin can replace the arguments or ask for the original.
    MovzW(code, 16, 0);
    code.Bl(LabelSend);
    code.CbzX(0, LabelReturn);
    CmpXImm(code, 0, 1);
    code.Bne(LabelFallback);

    // The original runs through the trampoline. Its normal return comes back
    // to this stub because the call uses BLR. Restore the TLS message first:
    // FS may have been inside an IPC handler when it entered the hook.
    code.Mark(LabelCallOriginal);
    code.Bl(LabelRestoreTls);
    code.Adr(9, static_cast<int64_t>(StubDataOffset) - code.pc());
    LdrX(code, 9, 9, 0);
    for (uint32_t rt = 0; rt < 9; rt++)
        LdrX(code, rt, 31, SavedX0 + rt * 8);
    code.Word(0xD63F0120u); // BLR X9
    for (uint32_t rt = 0; rt < 9; rt++)
        StrX(code, rt, 31, SavedX0 + rt * 8);

    // AFTER: the original result is in the saved registers.
    MovzW(code, 16, 1);
    code.Bl(LabelSend);
    code.CbzX(0, LabelReturn);
    code.B(LabelReturnOriginal);

    code.Mark(LabelReturn);
    MrsTls(code, 10);
    for (uint32_t rt = 0; rt < 9; rt++)
        LdrX(code, rt, 10, TlsRegisters + rt * 8);
    for (uint32_t rt = 0; rt < 9; rt++)
        StrX(code, rt, 31, SavedX0 + rt * 8);
    code.Bl(LabelRestoreTls);
    code.B(LabelRestore);

    code.Mark(LabelReturnOriginal);
    code.Bl(LabelRestoreTls);
    code.B(LabelRestore);

    // A failed bridge is not allowed to break FS: run the original.
    code.Mark(LabelFallback);
    code.Bl(LabelRestoreTls);
    code.Adr(9, static_cast<int64_t>(StubDataOffset) - code.pc());
    LdrX(code, 9, 9, 0);
    for (uint32_t rt = 0; rt < 9; rt++)
        LdrX(code, rt, 31, SavedX0 + rt * 8);
    LdrX(code, 18, 31, SavedX18);
    LdrX(code, 30, 31, SavedX30);
    AddSp(code, FrameSize);
    code.Word(0xD61F0120u); // BR X9

    code.Mark(LabelRestore);
    Restore(code);
    code.Word(0xD65F03C0u); // RET

    // Restore the IPC message that the hooked thread had before the bridge
    // used its TLS. This runs after every send, and before the original.
    code.Mark(LabelRestoreTls);
    StrX(code, 30, 31, SavedLR);
    MrsTls(code, 9);
    AddX(code, 10, 31, TlsSave);
    MovzW(code, 11, TlsWords);
    code.Mark(LabelRestoreTlsLoop);
    LdrPostX(code, 12, 10);
    StrPostX(code, 12, 9);
    SubsW(code, 11, 11, 1);
    code.Bne(LabelRestoreTlsLoop);
    LdrX(code, 30, 31, SavedLR);
    code.Word(0xD65F03C0u); // RET

    // The send subroutine builds the TIPC request in the thread's TLS, sends
    // it, and returns the action in X0 or UINT64_MAX on failure. The response
    // registers stay in the TLS buffer.
    code.Mark(LabelSend);
    StrX(code, 30, 31, SavedLR);
    MrsTls(code, 10);
    MovzW(code, 11, 16 + HEN_FS_HOOK_CALL_COMMAND);
    StrW(code, 11, 10, TlsRequest);
    MovzW(code, 11, RequestWords);
    StrW(code, 11, 10, TlsRequest + 4);
    code.Adr(9, static_cast<int64_t>(StubDataOffset) - code.pc());
    LdrW(code, 11, 9, 16);
    StrW(code, 11, 10, TlsRequest + 8);
    StrW(code, 16, 10, TlsRequest + 12);
    for (uint32_t rt = 0; rt < 9; rt++) {
        LdrX(code, 11, 31, SavedX0 + rt * 8);
        StrX(code, 11, 10, TlsRequest + 16 + rt * 8);
    }
    LdrX(code, 0, 9, 8);
    Svc(code, 0x21);
    code.CbnzX(0, LabelSendFail);
    MrsTls(code, 10);
    LdrW(code, 11, 10, TlsResult);
    code.CbnzW(11, LabelSendFail);
    LdrW(code, 0, 10, TlsAction);
    LdrX(code, 30, 31, SavedLR);
    code.Word(0xD65F03C0u); // RET

    code.Mark(LabelSendFail);
    code.Word(0x92800000u); // MOVN X0, #0 (UINT64_MAX)
    LdrX(code, 30, 31, SavedLR);
    code.Word(0xD65F03C0u); // RET

    if (!code.Finish() || code.size() > StubDataOffset)
        return false;
    std::memcpy(out, code.bytes(), code.size());

    std::memcpy(out + StubDataOffset, &trampoline, sizeof(trampoline));
    const uint64_t session = 0;
    std::memcpy(out + StubDataOffset + 8, &session, sizeof(session));
    std::memcpy(out + StubDataOffset + 16, &hook_id, sizeof(hook_id));
    const char port[] = "kaih:fs";
    static_assert(sizeof(port) == StubPortSize, "bridge port name");
    std::memcpy(out + StubDataOffset + StubPortOffset, port, sizeof(port));
    return true;
}

} // namespace ounce::hook
