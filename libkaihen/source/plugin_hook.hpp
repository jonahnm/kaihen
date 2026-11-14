// SPDX-License-Identifier: ISC
// The plugin runtime's FS hook callback server. HEN calls it on the session
// that the bootstrap took from the handoff; handlers run here, in the plugin
// process, and never in FS.
#pragma once

#include <switch.h>
#include "../include/kaihen/plugin.h"

namespace kaihen {

Result RegisterHookHandler(HenFsHookHandler handler, void* user, u32* out_id);
void UnregisterHookHandler(u32 id);

/// Starts serving hook calls on @p session, which the runtime still owns.
Result StartHookServer(Handle session);
void StopHookServer();
bool HookServerRunning();

} // namespace kaihen
