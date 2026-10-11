# Replacing the SDK: cooperative scheduler, fibers, and host subsystems

Written 2026-10-11. Companion to `PERFORMANCE_PLAN_NATIVE11.md` (which
keeps the measurements). This document is the design and the work order
for removing hardware emulation from Nebula: every Nintendo SDK subsystem
that today runs as translated PowerPC on top of emulated registers becomes
host code, guest threads become host fibers, and scheduling becomes
cooperative and polled. The target is the behaviour of WiiCompiled's
runtime (`runtime/src/hle/*`, `fiber_manager.h`), adapted to RMGE01 and to
what Nebula already intercepts.

The principle for every item: the game's own code (5.16 MB of the 5.40 MB
of `.text`) stays translated; the SDK (1,125 functions, 241 KB, listed with
addresses in `docs/sdk/RMGE01_SDK_FUNCTIONS.txt` from the public Petari
symbol table) is replaced function by function with host implementations
that keep the guest-visible data structures in guest RAM exactly as the
real SDK lays them out, so game code that reads those structures directly
keeps working. Nothing below the SDK (PI, VI, DSP, AI, DI, IPC, EXI, SI,
MI, CP registers, the decrementer, the time base registers) is emulated
any more; the fastmem fault handler and the device register model become
a compatibility fallback that should see zero traffic.

## 0. Ground truth to work from

- Symbol table: `D:\NebulaWork\ref\symbols\RMGE01-symbols.txt` (7.8 MB, all
  42,060 `.text` symbols with sizes); the SDK subset is in
  `docs/sdk/RMGE01_SDK_FUNCTIONS.txt`. Every address in this plan comes
  from it. Nebula's runtime already names many game functions
  (`main_frame_call_label` in `native_runtime.cpp`) and intercepts the
  scheduler entry points through the cached-call service list at
  `native_runtime.cpp:15589`.
- Translator hooks that already exist and are reused here: hand-written
  native substitutions (`native_function_body` in `translate.rs`, which
  emits a call to a `galaxy::native_*` function and `return;` for a guest
  address), the cached-call intercept list in the runtime (host code runs
  before, instead of, or after a translated function), and the direct
  dispatch table (`build_direct_dispatch_table`), which withholds every
  intercepted address so calls reach the host.
- Audio: RMGE01 does not use AX. It uses JAudio (JAS); the replacement is a
  host mixer at the JAudio driver interface (section 4), with no DSP and
  no microcode of any kind.
- Threads in RMGE01: the main thread, the JAudio thread (`JASAudioThread`),
  the DVD/file loader threads (`FileLoaderThread`), the THP/movie threads,
  the WPAD/KPAD driver thread (`WPADi`), the NAND/NWC24 helper threads, and
  the OS idle thread. All go through `OSCreateThread` 804AB488.

## 1. Thread model: guest threads as host fibers

Replace: `OSCreateThread` 804AB488, `OSResumeThread` 804ABB90,
`OSSuspendThread` 804ABE28, `OSSleepThread` 804ABFBC, `OSWakeupThread`
804AC0A8, `OSYieldThread` 804AB44C, `OSExitThread` 804AB6F4,
`OSCancelThread` 804AB7D8, `OSJoinThread` 804AB9B0, `SelectThread` 804AB20C,
`__OSReschedule` 804AB434, `OSLoadContext` 804A381C, `OSSaveContext`
804A379C, `OSSetCurrentContext` 804A3734, `OSClearContext` 804A395C,
`OSGetCurrentThread`, `OSSetThreadPriority`, `OSGetThreadPriority`,
`OSDisableScheduler`/`OSEnableScheduler`, `OSCheckActiveThreads`, the
mutex family (`OSInitMutex` 804A9434, `OSLockMutex` 804A946C,
`OSUnlockMutex` 804A9548, `OSTryLockMutex` 804A967C), the message queue
family (`OSInitMessageQueue` 804A8884, `OSSendMessage` 804A88E4,
`OSReceiveMessage` 804A89AC, `OSJamMessage` 804A8A88), and the condition
variable family if present (`OSInitCond`/`OSWaitCond`/`OSSignalCond` are
not in the symbol table; verify with the full list).

Design:

1. One host fiber per guest thread. `OSCreateThread` creates the fiber
   (`CreateFiberEx(256 KiB reserve, 32 KiB commit, FIBER_FLAG_FLOAT_SWITCH)`)
   whose entry calls the translated thread entry function with the guest
   stack pointer the game supplied (the guest stack stays in guest RAM;
   the host fiber stack only holds translated C++ frames). The simulation
   thread converts itself with `ConvertThreadToFiberEx` at boot; the boot
   thread's fiber is the main thread. The idle "thread" is the scheduler
   loop itself (WiiCompiled's `s_schedulerFiber`).
2. The OSThread structure in guest RAM is kept byte-exact and authoritative
   for everything game code reads (state at +0x2C8, suspend count +0x2CC,
   effective priority +0x2D0, base priority, queue link +0x2DC, the
   `OSContext` at the start of the thread). The host keeps a side table
   `guest_thread -> fiber handle` only.
3. Run queues: use the SDK's own queues in guest RAM (`RunQueue[32]` and
   `RunQueueBits` at 0x806A2D30, which `branch_checkpoint_body` already
   reads) so `OSCheckActiveThreads`, debuggers and any game code that
   peeks at them see the truth. `SelectThread` is host code: clear the
   current thread's RUNNING state if it must yield, pick the highest set
   bit, dequeue, mark RUNNING, write 0x800000E4 (current thread) and
   0x800000D4 / 0x800000C0 (current context, physical alias), and
   `SwitchToFiber`. The PPC register image is no longer saved to the
   OSContext on a switch (the fiber holds it in its own `PpcContext`),
   except for the fields game code inspects: SRR0/SRR1/MSR/GPR1/GPR13 of a
   sleeping thread (write them when the thread blocks; cheap).
4. Priority inheritance for mutexes exactly as the SDK does
   (`__OSPromoteThread`, `OSLockMutex` records the mutex in the thread's
   queue); WiiCompiled's `os_scheduler.cpp` carries the same logic.
5. Preemption points: only scheduler entries (sleep, yield, wakeup from a
   higher-priority wake, mutex contention, message queue block, join,
   exit, suspend) and the polled events in section 2. There are no
   checkpoints in translated code (translator option
   `cooperative_scheduler: true`, generation 18: no `call_return_checkpoint`,
   no budgeted branch checkpoints, no `architectural_interrupt_checkpoint`;
   `mtmsr`, `mfmsr`, `rfi`, `mtdec`, `mfdec`, `sync`, `isync`, `eieio`,
   `dcbf`, `dcbst`, `dcbi`, `icbi`, `dcbz`, `tlbie` lower to the cheapest
   correct host operation: MSR becomes a plain field, DEC reads return the
   host alarm clock, cache operations become no-ops except `dcbz` which
   stays a 32-byte zero store).
6. Deferred guest callbacks (VI retrace, alarms, audio frame, DVD
   completion) run on the fiber that polls them, with a private register
   file (`GuestInterruptCallbackContext` in WiiCompiled: copy the current
   fiber's `PpcContext`, call the handler, discard), with the scheduler
   disabled (`OS_HLE_BeginDeferredGuestCallbacks`) so a wakeup inside a
   callback only marks threads runnable and the switch happens when the
   polling point returns. This is exactly the semantics of an interrupt
   handler on the real hardware (handlers run on the interrupted thread's
   stack with interrupts disabled).
7. `OSDisableInterrupts` 804A80E8 / `OSEnableInterrupts` 804A80FC /
   `OSRestoreInterrupts` 804A8110 flip one host flag and the MSR[EE] bit in
   the current context; nothing else. Polled events are delivered only
   while the flag is enabled (WiiCompiled: `Audio_HLE_PollDeferred` and
   `OS_HLE_ProcessAlarmsDeferred` return early otherwise).

What this removes from the simulation thread: every GuestTransfer unwind,
the flat continuation dispatcher, `lookup_function` at hops, the
checkpoint bodies, the deadline broker, the decrementer timeline, the
interrupt dispatch records and proofs, the idle-loop wait machinery, the
same-context RFI retention protocols, the OSLoadContext ownership tokens,
and the `[sim-profile-other]` rows for the unwinder. Everything in
`native_runtime.cpp` that mentions `GuestTransfer`, `external_dispatch`,
`checkpoint_rfi_proof`, `pending_vi_boundary`, `DeadlineBroker`,
`decrementer_deadline`, `host_pause_candidate` is retired by this item.

Order of implementation (each step ships and is measured):
a. Fibers under the existing scheduler: intercept `OSLoadContext` to
   `SwitchToFiber` instead of `rfi` + throw; keep checkpoints and the
   broker. Measure: `[transfer] exception-unwinds` must be 0.
b. Polled delivery of VI/AI/DSP/IPC/DEC events at scheduler entries and in
   the idle loop (section 2); checkpoints become plain yield points.
c. Generation 18 without checkpoints.
d. Host `SelectThread`, sleep/wake/yield/mutex/message queues.

## 2. Timing: alarms, time base, VI, frame pacing

Replace: `OSGetTime` 804AC368, `OSGetTick` 804AC380, `__OSGetSystemTime`
804AC388, `OSTicksToCalendarTime` 804AC5B4, `OSSetAlarm` 804A2164,
`OSSetPeriodicAlarm` 804A21D4, `OSCancelAlarm` 804A2258,
`DecrementerExceptionHandler` 804A259C (never installed), `VIInit`
804B21E8, `__VIInit` 804B1FD8, `VIConfigure` 804B2CB0, `VIConfigurePan`
804B3394, `VIFlush` 804B36E8, `VIWaitForRetrace` 804B2730,
`VISetNextFrameBuffer` 804B37FC, `VIGetNextFrameBuffer` 804B3868,
`VIGetCurrentFrameBuffer` 804B3870, `VISetBlack` 804B3878,
`VIGetRetraceCount` 804B38F0, `VIGetCurrentLine` 804B38F8, `VIGetTvFormat`
804B3988, `VIGetDTVStatus` 804B39E8, `VISetPreRetraceCallback` 804B1E38,
`VISetPostRetraceCallback` 804B1E7C, `__VIRetraceHandler` 804B1628 (never
installed), the dimming and I2C/gamma/filter setters (no-ops that record
the values).

Design:

1. Time base: `OSGetTick` returns the host TSC-derived 60.75 MHz tick
   (`RuntimeTimeline::now_ticks`, already one `rdtsc`); `mftb`/`mftbu` in
   translated code lower to the same inline read (no service call).
2. Alarms: a sorted list in guest RAM using the SDK's own `OSAlarm` layout
   (handler +0x00, fire time +0x08, prev +0x10, next +0x14, period +0x18,
   start +0x20) so `OSCancelAlarm` and game code that walks it keep
   working. `ProcessAlarmQueue(max)` runs due alarms with the deferred
   callback context (section 1.6). It is called at every scheduler entry,
   in the idle loop, and from the host wait points (VI wait, DVD wait,
   audio frame). No decrementer exists.
3. VI: a software retrace clock at 16,683.4 us (59.94 Hz NTSC; use the
   render mode's rate) anchored to the host clock. `AdvanceDueRetraces`
   advances `retraceCount`, toggles the field, commits the pending
   framebuffer when a `VIFlush` armed it, calls the pre-retrace callback,
   wakes the `VIWaitForRetrace` sleepers (the SDK's retrace wait queue at
   its guest address), and calls the post-retrace callback. It is polled
   from `VIGetRetraceCount`, `VIGetNextFrameBuffer`, the scheduler idle
   loop, and `GXCopyDisp`. `VIWaitForRetrace` sleeps the calling fiber on
   the retrace queue until `retraceCount` changes (the idle loop then
   waits on the host clock with `SleepPreciselyUntil`: a high-resolution
   waitable timer to within 1 ms, then a 300 to 500 us spin).
   Game-side users to keep correct: `waitForRetrace__17MainLoopFrameworkFv`
   8039FEE0, `preRetraceProc__8JUTVideoFUl` 80419308,
   `postRetraceProc__8JUTVideoFUl` 80419554, `waitRetraceIfNeed__8JUTVideoFv`
   80419618 (JUTVideo installs the pre/post retrace callbacks and swaps the
   XFB inside them; the runtime's existing JUT XFB manager handling at
   0x806A2868 documents the fields).
4. Frame pacing and presentation: the XFB the game set with
   `VISetNextFrameBuffer` + `VIFlush` is what the renderer presents at the
   retrace boundary (the renderer already keeps XFB textures by address:
   `present_cached_xfb`). Present scheduling anchors to the retrace clock,
   not to the GPU fence; the swap chain runs with frame latency 1 and
   `DXGI_PRESENT_ALLOW_TEARING` off at 60 Hz displays, and the retrace
   clock is phase-locked to the display's vblank when the display is 60
   Hz (`IDXGIOutput::WaitForVBlank` once at startup to find the phase;
   this is F17 from the original list). Variable-refresh and uncapped
   modes come later (WiiCompiled's interpolation is a separate feature).
5. Process and thread QoS at startup: `timeBeginPeriod(1)`,
   `SetProcessInformation(ProcessPowerThrottling)` with
   `EXECUTION_SPEED` and `IGNORE_TIMER_RESOLUTION` both forced off,
   MMCSS "Games" for the simulation and render threads.

## 3. Graphics: GX API as host code, FIFO kept only for display lists

Today: the game writes GX commands to the write-gather pipe; the runtime
batches 4 KB bursts into a FIFO that the render thread parses
(`FifoParser`), with PE tokens and finish interrupts delivered through
emulated PE/CP registers and GPU fences; the simulation thread
pre-classifies every FIFO for PE events. The GX library itself (198 `GX*`
functions, 25 `__GX*`) is translated and executes register-write sequences
through `fast::store` into the gather pipe.

Replace with the SDK-level model, in two stages:

Stage A (keeps the FIFO parser, removes the emulated devices):
- `GXInit` 804B74E0, `GXInitFifoBase` 804B8458, `GXSetCPUFifo` 804B85DC,
  `GXSetGPFifo` 804B87BC, `GXGetFifoPtrs` 804B8C34, `GXFlush` 804B9B20,
  `GXSetDrawSync` 804B9EC8, `GXSetDrawDone` 804B9F7C, `GXDrawDone`
  804BA014, `GXSetDrawSyncCallback` 804BA248, `GXSetDrawDoneCallback`
  804BA310, `GXPeekZ` 804BA228, `GXPeekARGB` 804BA210, `GXCopyDisp`
  804BB1F8, `GXCopyTex` 804BB334, `GXAbortFrame`, `GXSetBreakPtCallback`
  become host functions. The game's gather-pipe writes still land in the
  host FIFO (the batched 4 KB buffer), but `GXDrawDone` and `GXSetDrawSync`
  no longer wait for a PE interrupt: `GXDrawDone` flushes the FIFO to the
  render thread and blocks the calling fiber on the frame's completion
  (a host event signalled by the render thread when the GPU fence for
  that submission completes), then runs the draw-done callback on the
  calling fiber; `GXSetDrawSync(token)` records the token in the FIFO and
  the render thread invokes the sync callback (deferred, section 1.6)
  when the parser reaches it. No PE register, no PI interrupt, no
  `pending_vi_boundary`. The pre-classification on the simulation thread
  is deleted (tokens are found by the parser on the render thread).
- `GXCopyDisp` marks the XFB ready for VI (section 2.4) and triggers the
  frame handoff (today's `take_gx_fifo` at the frame boundary).
- Display lists (`GXBeginDisplayList`/`GXEndDisplayList`/`GXCallDisplayList`)
  keep writing into guest RAM exactly as today; the parser already caches
  decoded display lists.

Stage B (direct host GX, the WiiCompiled/aurora model): the hot GX entry
points the game calls per draw (`GXBegin`, `GXEnd`, `GXSetVtxDesc`,
`GXSetVtxAttrFmt`, `GXSetArray`, `GXLoadPosMtxImm`, `GXLoadNrmMtxImm`,
`GXSetTevOrder`, `GXSetTevOp`, `GXSetTexCoordGen`, `GXLoadTexObj`,
`GXSetBlendMode`, `GXSetZMode`, `GXSetCullMode`, `GXSetChanCtrl`,
`GXSetNumChans`, `GXSetNumTexGens`, `GXSetNumTevStages`, `GXSetAlphaCompare`,
`GXSetFog`, `GXSetLineWidth`, `GXSetPointSize`, `GXSetViewport`,
`GXSetScissor`, `GXSetProjection`, `GXSetTexCopySrc/Dst`,
`GXSetDispCopySrc/Dst/YScale/Gamma`, `GXSetCopyFilter`, `GXSetCopyClamp`,
`GXClearBoundingBox`, `GXReadBoundingBox`, `GXInvalidateTexAll`,
`GXInvalidateVtxCache`, `GXSetTexCoordScaleManually`, the lighting
setters, the indirect texture setters) are replaced by host functions
that update the host `GxState` directly and record draws for the render
thread, bypassing the gather pipe and the FIFO parse for direct-mode
geometry. The byte stream a `GXBegin`/`GXEnd` pair writes (vertex data via
the gather pipe) is captured by the existing batched buffer and handed to
the vertex loader as today. This removes the register encode/decode round
trip (the parser, `decode_vertex_descriptor`, `GxState::load_bp` on the
render thread) and the `fast::store` WGPIPE test in generated code. It is
a large but mechanical port: the parser's handling of each BP/CP/XF
register already implements the semantics; each `GX*` function maps to
the registers it writes (the translated bodies are the specification).

The `GXPeek*` family and `GXReadBoundingBox` already have asynchronous
host implementations; keep them.

## 4. Audio: native JAudio mixer, no DSP, no ucode

RMGE01's audio is JAudio (JAS), the game's own middleware. JAudio keeps the
state of every playing voice in `JASDsp::TChannel` blocks in guest RAM,
written by the game's own code (`JASChannel::initialUpdateDSPChannel`
80492FAC, `JASChannel::updateDSPChannel` 804932A4,
`JASChannel::updateMixer` 80493710, `JASChannel::updateAutoMixer` 804935F4,
`JASChannel::updateEffectorParam` 80492D1C, `JASAramStream::updateChannel`
8048EBF8), and hands the whole set to the DSP once per audio frame
(`JASDriver::updateDSP` 804944D0), which mixes them into a buffer the game
then reads (`JASDriver::readDspBuffer` 804945CC) and finishes
(`JASDriver::finishDSPFrame` 804946E8). The DSP and its microcode are an
implementation detail below that interface.

The native replacement mixes those voices on the host directly:

1. Replace `JASDriver::updateDSP`, `readDspBuffer` and `finishDSPFrame`,
   the DSP boot and command functions (`DspBoot` 804961E0, `DspInitWork`
   804963E0, `DspStartWork` 80496420, `DSPSendCommands2` 804962C0,
   `DSPAddPriorTask` 80496520, `DSPReleaseHalt2` 80495EA0, the `DSP*` SDK
   functions 804CB28C to 804CB750) and the AI layer (`AIInit` 804C7FC0,
   `AIInitDMA` 804C7E7C, `AIStartDMA` 804C7EF8, `AIStopDMA` 804C7F0C,
   `AIRegisterDMACallback` 804C7E38, `AISetDSPSampleRate` 804C7F48,
   `JASDriver::initAI` 804941F8, `startDMA`/`stopDMA`) with host functions.
2. The host mixer reads each active `TChannel` block in guest RAM and
   produces the samples the hardware would: sample source (PCM8, PCM16,
   ADPCM in guest RAM or the ARAM buffer), pitch (resampling ratio),
   volume and pan per mix bus, envelope, loop points, filter; then the
   mix buses (main L/R, the effect sends) and the output volume. The
   `TChannel` layout and field meanings come from the game's own code that
   writes them: the Petari decompilation of RMGE01 (`include/JSystem/JAudio2/JASDSPInterface.hpp`
   and `src/JSystem/JAudio2/JASDSPInterface.cpp` in
   github.com/SMGCommunity/Petari, plus the shared JSystem headers in the
   other JSystem decompilations) documents every field and every value the
   game writes. That is the specification: what the game asks for, not how
   a DSP program implements it.
3. ADPCM decoding is the standard GameCube/Wii DSP-ADPCM format (4-bit
   nibbles, 8 coefficient pairs per sample header, predictor/scale byte
   per 8-byte frame); the coefficients are in each sample's header the
   game loads. Resampling: 4-tap polyphase or cubic at host rate; exact
   match to the console is not required.
4. Timing: the audio output thread (WASAPI, event-driven) owns the clock.
   Each 5 ms frame it mixes the voices whose `TChannel` state the game
   published at the last `updateDSP`, writes into a lock-free ring, and
   posts the AI DMA completion as a deferred guest callback, so the game's
   `JASAudioThread::DMACallback` 80494C70 and `DSPCallback` 80494CC0 run
   on the simulation thread at its next polling point (the
   `JASAudioThread::run` 80494B2C loop is then just a guest thread waiting
   on its message queue, scheduled cooperatively). The audio thread never
   runs guest code.
5. ARAM: a host buffer; `ARStartDMA` 804A0508 and the ARQ queue are
   `memcpy` plus immediate completion.
6. Validation: record 60 s of output from the current native-DSP path and
   from the host mixer at the same input replay; compare the spectrum and
   RMS per 5 ms frame (tolerance, not bit-exactness), and listen to the
   title theme, a stage, and a cutscene.

What this removes: the DSP worker thread (17 percent of a core on the
laptop), the mailbox spin (2.5 percent of the simulation thread), MRAM
transaction notifies (2 percent), the DSP and ARAM register faults,
`poll_native_dsp`, the AI DMA deadline emulation, and the compiled DSP
module.

## 5. Storage: DVD, NAND, IOS

Replace: `DVDInit` 804BFD58, `DVDOpen` 804BF2CC, `DVDFastOpen` 804BF264,
`DVDClose` 804BF578, `DVDReadPrio` 804BF844, `DVDReadAsyncPrio` 804BF75C,
`DVDGetCommandBlockStatus` 804C3DC4, `DVDConvertPathToEntrynum` 804BEF5C,
`DVDGetFileInfoStatus`, `DVDCancel`/`DVDCancelAll`, `DVDGetCurrentDiskID`,
`DVDCheckDisk`, the `DVDLow*` layer (27 functions, 8.9 KB), `IPCInit`
804D1E6C, the `IOS_*` family (15 functions), `ISFS_*` (27 functions),
`NAND*` (37 functions), `SC*` (32 functions), `NWC24*` (88 functions, all
can return "disabled"), `ESP*`, `WUD*` (Bluetooth stack: see section 6).

Design:

1. DVD: the FST is parsed on the host at boot (today's `open_game_pak`
   already indexes the pak); `DVDOpen`/`DVDFastOpen` fill the guest
   `DVDFileInfo` exactly (start offset, length, command block state);
   `DVDReadPrio` reads synchronously from the pak with pooled unbuffered
   `ReadFile` into guest RAM (no memory mapping: fixes the working-set
   growth); `DVDReadAsyncPrio` queues the read on the existing async disc
   worker and completes it as a deferred guest callback at the next
   polling point (callback with `r3 = bytes`, `r4 = fileinfo`), setting
   `DVD_STATE_END` in the command block. The DI register model, the
   `/dev/di` IPC path and the `ipc-reg-write-804D1F10` intercept are
   retired.
Device paths the runtime handles today (`native_host.cpp`): `/dev/di`,
`/dev/es`, `/dev/fs`, `/dev/stm/immediate`, `/dev/stm/eventhook`,
`/dev/usb/oh1/57e/305` (the Bluetooth adapter). Confirm the full set with a
boot trace of `IOS_Open` 804D27E8 before implementing; WiiConnect24
(`threadProc__15NWC24SendThreadFPv` 8040893C) and `/dev/net/*` should be
answered "not available" so their threads never start.

2. NAND and ISFS: already largely native (`AsyncNandState`); replace the
   remaining IOS request plumbing (`IOS_Open/Ioctl/Ioctlv/Read/Write/Close`
   and `IPCInit`) with direct host calls per device path (`/dev/fs`,
   `/dev/es`, `/dev/sc`, `/dev/stm`, `/dev/usb/*`, `/dev/net/*`):
   every path RMGE01 opens is finite; list them from a boot trace of
   `IOS_Open` and implement each as a host function returning what the
   game expects. `/dev/di` disappears with DVD.
3. SC: return the console settings from the launcher's settings file.
4. NWC24, WiiConnect24 and network: return "not available" codes.

## 6. Input: WPAD/KPAD as host code

Replace: `WPADInit` 804D779C, `WPADRead` 804D93C8, `WPADProbe` 804D8ACC,
`WPADSetDataFormat`, `WPADControlDpd`, `WPADControlSpeaker`, `WPADSetAutoSleepTime`,
`WPADGetStatus`, `WPADGetAccGravityUnit`, `WPADGetDpdSensitivity`,
`WPADSetConnectCallback`, `WPADSetSamplingCallback`, `WPADGetInfoAsync`,
`WPADDisconnect`, `WPADIsSpeakerEnabled`, `WPADStartSimpleSync`, the
rumble and extension controls (20 `WPAD*` + 5 `WPADC*` + `WPADi*`), `KPADInit`
80450E04, `KPADRead` 804506D8, `KPADReadEx`, `KPADSetPosParam`,
`KPADSetHoriParam`, `KPADSetDistParam`, `KPADSetAccParam`, `KPADSetBtnRepeat`,
`KPADSetSensorHeight`, `KPADSetFSStickClamp`, `KPADGetUnifiedWpadStatus`,
the 42 `WUD*` Bluetooth functions.

Design: the launcher's input layer already produces the Wii Remote model
(`SyntheticKpad*`, `poll_native_hid_controller`); `WPADRead` fills the
`WPADStatus` structure the game passes from that model and `KPADRead`
fills `KPADStatus` (the KPAD library is pure math over `WPADStatus`; its
translated body can even stay, fed by the host `WPADRead`). The WPAD
driver thread, the Bluetooth stack (`WUD*`), the HID report parser, the
sampling callbacks that today run through IPC/USB IOS calls and the
`/dev/usb/oh1` path are deleted. Rumble and the speaker map to host
controller APIs or no-ops.

## 7. Everything else in the SDK

| Library | Functions | Treatment |
| --- | --- | --- |
| `OSReport` 804A3E94, `OSPanic` 804A3F24, `DB*` (9) | stdout/log; `OSPanic` terminates with the message |
| `OSInit` 804A155C, `__OSContextInit` 804A3D20, `__OSPSInit` 804A1D94, `__OSInitSystemCall` 804AABA8, `__OSCacheInit` 804A33A8, `__OSInterruptInit`, `__OSExceptionInit`, `__OSInitAlarm`, `OSSetArena*`, `OSAllocFromArena*`, `OSGetConsoleType` 804A0C30, `OSGetPhysicalMemSize`, `OSGetConsoleSimulatedMemSize` | host `OSInit` that writes the low-memory globals (0x80000000 block: console type, memory sizes, bus/core clocks, arena bounds, the `__OSCurrentContext` pointers) and runs none of the hardware probes |
| Cache: `DCFlushRange` 804A2F20, `DCStoreRange`, `DCInvalidateRange`, `DCZeroRange`, `ICInvalidateRange` 804A3004, `ICFlashInvalidate`, `L2*`, `LC*` | no-ops, except `DCZeroRange` (memset) and the locked-cache `LC*` family (keep the 16 KB locked cache as a plain RAM window in the flat view) |
| `EXI*` (15), `SI*` (7) | success stubs; RMGE01 uses them only for the clock and boot info |
| `TRK*` (96, the MetroTRK debugger) | never called at runtime; leave translated or stub |
| `THPInit` 80454820, `THPVideoDecode` 804514EC, `THPAudioDecode` 804548BC, `THPGX*` (4) | already native (`NativeThpAsyncDecoder`); add two-frame decode-ahead and a SIMD IDCT |
| `MTX`/`PSMTX` (18), `VEC`, `QUAT`, `C_*` (8) | leave translated (they are the fast-float paired-single code and are already near native speed); optionally hand-written AVX versions of `PSMTXConcat`, `PSMTXMultVec`, `PSMTXInverse` which top the guest profile |
| `ARC*` (11) | leave translated (pure guest-memory parsing) |
| `JKR`, `JUT`, `JAS` (game middleware, not SDK) | leave translated; they are game code |

## 8. The translator after the replacement

- Generation 18: `cooperative_scheduler: true` removes every checkpoint
  and interrupt-related emission (section 1.5). Resident registers then
  survive across calls; expect the guest share to drop noticeably on top
  of the host savings.
- The host-call list grows by the ~400 SDK functions replaced above;
  every one is a `native_function_body` entry or a cached-call intercept.
  Keep translating the SDK bodies (they cost nothing if never called) so
  an `#ifdef`-style fallback per function remains possible during
  bring-up (`GALAXY_SDK_TRANSLATED=OSSleepThread,...` to force the
  translated version of a named function).
- Device-address lowering stays for whatever still touches a register
  (ideally nothing); the fault handler stays as a safety net and prints a
  warning for any fault after boot.

## 9. Order of work and validation

1. Thread model step a (fibers under the current scheduler). Validation:
   title smoke 120 s, replay-input gameplay smoke, `[transfer]
   exception-unwinds = 0`, no `GuestTransfer` in profiles.
2. Timing (alarms, time base, VI retrace clock) and polled delivery.
   Validation: VI cadence 60.0 new frames, no checkpoint gaps, audio
   without dropouts (JAudio still on the compiled DSP at this point).
3. Generation 18 without checkpoints. Validation: the module benchmark
   per function with the same harness, the gameplay smoke, the laptop
   recording.
4. Audio HLE (section 4). Validation: audio differential test against the
   compiled DSP (record 60 s of mixed output from both, compare RMS error
   per 5 ms frame; `GALAXY_DSP_NATIVE=1` keeps the old path).
5. DVD/IOS/NAND/SC (section 5). Validation: boot, file select, save/load,
   a full stage load, the THP intro.
6. Input (section 6). Validation: the launcher's input test scene and
   pointer mode.
7. GX stage A, then stage B function by function (section 3), with the
   frame dump comparison tool (`frame*.gxdump` exist in the tree) as the
   regression test per GX function.
8. Process QoS and pacing (section 2.5).

Each step must land with: the smoke scripts green, the laptop Detailed
recording digest (`diag_digest.py`) attached to the commit message, and
`docs/NATIVE_EXECUTION.md` updated with the measurement.

## 10. Expected outcome

With sections 1 to 6 done, the simulation thread runs game code plus the
host SDK with no emulation underneath: no faults, no interrupt
emulation, no checkpoints, no DSP worker, no deadline broker. On the
185H that is the difference between 61 percent of a core with 20 to 100
ms stutters and roughly 25 to 30 percent with none; on a 6th-generation
i7 it is the difference between not holding 60 and holding it with
margin. Section 3 stage B then removes the FIFO parse from the render
thread, which is what makes the 4-core, integrated-graphics target
comfortable.
