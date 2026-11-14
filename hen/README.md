# HEN

HEN is the resident homebrew environment. The native bootstrap starts it
and hands it the sessions it keeps for the whole boot: the FS proxy root,
FS's SM session, the process handles for game launches, and the control
sessions of the plugins. Build it with `make` in kaiHEN (or `make -C hen`);
`sh hen/test.sh` runs its host tests.

One thread serves HEN's IPC and runs game arming as resumable steps. A
synchronous service call on that thread pauses the whole loop, so keep slow
work off it.

## Control interface

The bootstrap and RO use the control interface (`common/control.h`, HEN's
own CMIF commands). It adopts the sessions and handles above, creates
homebrew sessions, starts game arming and reports its state, and, for each
plugin, creates an SM proxy session and a clone of the FS root.

## Homebrew interface

Homebrew programs get a HEN session through RO
(`kaihenOpenHomebrewSession` in `libkaihen/include/kaihen.h`). Its
commands are declared in `libkaihen/include/kaihen/protocol.h`, protocol
version 6:

| Command | Returns |
|---|---|
| 0 `GetStatus` | HEN's process ID, protocol version and startup result |
| 16 `GetFilesystemRoot` | a new clone of the FS proxy root |
| 17 `GetSettingsSession` | a new clone of HEN's `set:sys` session |
| 18 `GetProcessHandle` | a copy of the caller's process handle |
| 19 `GetServiceManager` | a new session on HEN's SM proxy |
| 20, 21 `GetPluginCount`, `GetPluginName` | the plugins that publish IPC |
| 22 `OpenPlugin` | a new session with a plugin |

The FS root is already initialized. libounce installs it as the `fsp-srv`
override and clones it without sending `SetCurrentProcess`, which would
change the process identity that FS uses for every holder of the root.

## Plugin FS hooks

HEN gives each plugin a session of its own for FS hooks, created by
`CreatePluginSession` during the bootstrap's handoff. The homebrew sessions
never serve those commands. The plugin also creates a callback session pair;
the bootstrap takes a copy of its client end with the handoff's
`GetHookSession` command and gives it to HEN with `AdoptPluginHooks`.

FS and the plugin are separate processes, so the FS function cannot branch
directly into plugin code. HEN writes a small AArch64 bridge stub into FS
instead (`source/fs_hook_stub.cpp`). The patched entry branches to the stub;
the stub sends a TIPC call on the named port `kaih:fs`
(`source/server.cpp`), and HEN invokes the plugin's handler on the callback
session. The handler receives x0-x8 in a `HenFsHookContext`.

A handler can return `HEN_FS_HOOK_CALL_ORIGINAL` in the BEFORE phase. HEN
answers with that action, and the stub calls the existing trampoline with
`BLR`: the trampoline runs the overwritten prologue and branches back into
the target, and the target's normal return goes to the stub because `BLR`
set x30. The stub then sends the AFTER event with the original result, and
the handler may replace those registers. This is how an original call works
across the process boundary, without moving plugin code into FS.

The bridge slot is 1 KiB: stub code and parameters, then the trampoline.
Sixteen slots fill RO's 0x4000-byte writable-code arena. The patch and
stub are written to FS through RO's `ro:dmnt` endpoint; the arena and module
base come from RO's writable-code and image-base lookups. HEN uses the FS
process handle that `AdoptGameHandles1` brings. If the named port cannot be
created, HEN still starts, but a hook install returns
`LibnxError_NotInitialized`.

The same plugin session copies FS memory in both directions
(`HenPluginCommand_CopyToFs` and `CopyFromFs`), 192 bytes per request so
that the request and the reply fit the 0x100-byte message buffer. HEN
serves them through RO's `RoProcessCopyReadCommand` and
`RoProcessCopyWriteCommand`, which reach any readable or writable FS memory
rather than only code. They need the FS process handle, but not the `kaih:fs`
port.

## SM proxy

Programs never receive FS's SM session: SM closes a session after a request
it cannot process, and a shared session would then take SM from every
program until HEN restarted. Each program gets its own session on HEN's SM
proxy (`source/sm_proxy.cpp`), served on a thread of its own so that a
lookup that waits does not stop HEN's main loop. The proxy answers
RegisterClient and DetachClient itself and forwards GetServiceHandle to SM
as a request it builds from the decoded arguments; it refuses
RegisterService and UnregisterService. A request it cannot decode closes
only that program's session.

## Game arming

When the player holds R while starting a native game, HEN lets the game's
process start with the homebrew loader in place of the game. The first such
launch runs the game normally while HEN saves its signed policy to the SD
card; later launches start hbloader, which runs hbmenu.
