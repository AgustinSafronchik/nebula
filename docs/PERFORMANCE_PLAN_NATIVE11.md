# Nebula performance plan: Native11 and beyond

Written 2026-10-11 at the end of the Native10 session. Audience: the next
agent (Opus or later) continuing the performance program. Read this whole
file before touching anything. Every claim below has the measurement that
produced it; every work item has exact steps and a validation step.

## 0. Goal, target hardware, and where we stand

Goal set by the user: locked, stutter-free, slowdown-free 60 fps at 480p
internal on a 6th-generation Core i7 with Intel HD 530 integrated graphics
or a GT 1030 and 8 GB of RAM, after first perfecting a Core Ultra 9 185H
(Arc iGPU, 16 GB). Beat Dolphin and WiiCompiled. Bit-exactness is not
required anywhere; "mostly native and much faster" is the standard. The
user has given full autonomy.

Measured state with Native10 (`D:\NebulaWork\native\Nebula-Native10-win64-AVX2.zip`,
SHA256 5FFDE3294A3B6444BC751397A240114080DD9620E1657C95448052A954B06F7C):

| Machine | Run | Simulation thread | Frame production | Notes |
| --- | --- | --- | --- | --- |
| 9950X3D desktop, title screen, 120 s | native10-boot | 23.7 % of one core running | 60 fps | reference |
| 185H laptop, 480p internal, 238 s | 20261011-030107 | 61 % of one core (profiler: 58 % of samples running) | 60.0 presented, 60.0 new frames per second in the steady windows; 44 to 49 during boot and the intro video | 5 checkpoint gaps of 20 to 105 ms = the stutters |
| 185H laptop, 1080p internal (EFB scale 3), 255 s | 20261011-025642 | 61 % | 60.0 / 60.0 in the steady windows, same early dips | user reports it feels worse: GPU-bound on the iGPU, not CPU |
| 185H laptop, 480p, crashed at 109 s | 20261011-025441 | 74 % | n/a | access violation in ntdll on a worker thread, no minidump (see P0) |

Simulation-thread sample breakdown on the laptop (run 030107, 159,547
samples at 1 ms): running 93,147 of which guest code 42,166 (45 %), runtime
host code 24,264 (26 %), foreign modules (ntdll, vcruntime, ucrt, win32u)
26,717 (29 %). Blocked 66,400: idle loop 53,837, THP video decode join
8,836, VI waits 3,376.

What the foreign-module 29 percent is (other-caller attribution, run 030107):

| Caller in runtime | Samples | What it calls | Share of running |
| --- | --- | --- | --- |
| `return_from_interrupt+0x1430` | 9,517 | `throw GuestTransfer` (RaiseException, RtlUnwind, __CxxFrameHandler4, RtlLookupFunctionEntry, RtlVirtualUnwind2) | 10.2 % |
| `dispatch_decrementer+0x790` and its catch funclet | 3,649 | the same unwinder | 3.9 % |
| `install_host_pointer_transition_provider+0x1690` | 2,181 | NtUserGetAsyncKeyState per key, every poll | 2.3 % |
| `DspMramTransactionBoundary::service_pending+0x1a0` | 1,865 | condition-variable notify (ZwAlertThreadByThreadId) | 2.0 % |
| `current_thread_cpu_time_100ns` | 843 | ZwQueryInformationThread (GetThreadTimes) | 0.9 % |
| misattributed `basic_stringstream` symbol +0x7360 | 779 | ucrt `strchr` = a getenv_s in a hot path | 0.8 % |
| `DeadlineBroker::replace_once` | 1,189 | ZwSetEvent, lock | 1.3 % |
| `steady_clock::now` | 547 | QPC | 0.6 % |

Host-code hot spots on the laptop: `read_device+0x8a0` 2,373 (the DSP mailbox
spin waiting for the coprocessor worker, 2.5 %), time base and timeline
reads about 2,000 (2 %), `lookup_function` 425, `drain_guest_memory_writes`
780, `branch_checkpoint_body` 260, `lock_slot` 226, DSP MRAM/ARAM staging
~400. `[host-census]`: 46.6 million time-base reads in 238 s, 6.4 million of
them from the guest; 7.8 million checkpoint bodies (33 thousand per second).

Guest hot spots are flat: the top guest function is 1.1 percent. By caller,
`0x803804B0` (the frame loop) 11 %, `0x804A381C` (OSLoadContext, i.e. the
thread switch path) 6 %, `0x803812C4` 3 %, `0x8048362C` 2.7 %,
`0x804A8110` 2.7 %.

Conclusion: at 480p the 185H already produces 60 new frames per second in
gameplay windows; what remains on that machine is stutter (timer
oversleeps, video decode waits, worker-thread crash) and headroom. The
6th-generation i7 target has roughly 45 to 55 percent of the 185H's
single-thread throughput, so the simulation thread must shrink by more than
2x to hold 60 there with margin. The breakdown says where: the interrupt
and thread-switch machinery (14 %), the checkpoint and timing substrate
(5 to 8 %), the DSP coprocessor handshake (3 to 5 %), the syscall leftovers
(5 %), and only then the guest code itself.

## 1. Why WiiCompiled is faster, and what to copy

Researched 2026-10-11 from github.com/patchzyy/Wiicompiled (GPLv3; Nebula
already adapts some of its code with attribution, keep doing that):

- It is an SDK-level recompilation. The game's code is translated, but the
  whole Nintendo SDK is replaced by host code: `runtime/src/hle/os/`
  (os_thread, os_scheduler, os_interrupt, os_alarm, os_context, os_sleep,
  os_time, os_message), `hle/audio/ax_*` (the AX DSP as a host mixer),
  `hle/gx/*` (GX API functions implemented natively, the FIFO parsed by
  their own code), `hle/vi.cpp`, `hle/storage/dvd.cpp`, `hle/ios.cpp`,
  `hle/input/*`. Nebula translates the SDK and emulates the hardware under
  it (PI/VI/DSP/AI registers, FIFO, IPC, decrementer).
- Guest threads are host fibers (`runtime/include/fiber_manager.h`,
  `GuestFiberManager::SwitchToThread` from the OSLoadContext HLE). A thread
  switch is a fiber switch: no exception unwinding, no flat continuation.
- Scheduling is cooperative and polled, exactly the Wii's own semantics:
  `SelectThread_801a9c08` runs `ProcessSleepTimers`, then picks the
  highest-priority run queue; the idle loop polls `ProcessSleepTimers`,
  `Audio_HLE_Poll` (a 4 kHz event), `VI_HLE_PollRetrace`,
  `ProcessTimerEvents`, `ProcessAlarmQueue`, and
  `VI_HLE_WaitForNextRetracePoll`. There is no asynchronous interrupt
  delivery and no safepoint polling inside translated code. Preemption
  only happens at scheduler entries (OSSleepThread, OSYieldThread,
  OSWakeupThread, mutex contention).
- Consequences for Nebula: no checkpoint tests in generated code at all, no
  interrupt-return unwinding, no deadline broker, no decrementer emulation,
  no VI register timing emulation, no DSP mailbox spin, no AI DMA
  emulation. These are, item by item, the host overheads in the table above.

The plan below moves Nebula to that model in stages that each ship.

## 2. Prioritized work items

Ordered by expected gain on the low-end target. Each has: evidence,
expected gain, exact steps, validation.

### P0. Make crashes debuggable and fix the worker-thread crash (first, small)

Evidence: run 025441 died with `0xC0000005 at ntdll+0x15ae1` on thread
31524 (a low-CPU worker, 3 % of a core), `mov r8,[r9+8]` with r9 =
0xFFFFFFFFFFFFFFF0, r8 = 0xFFFFFFFFFFFFFF90: a CONTAINING_RECORD on a null
list entry inside ntdll, i.e. a destroyed or never-initialized
synchronization object or heap block used from a worker. The minidump
failed: `generated\diagnostics\NebulaRuntime_crash_32960.dmp` error 3
(path not found; the directory is relative to the working directory and
does not exist in an installed copy). The previous session saw the same
crash signature once (`mov r8,[r9+8]`, "crashed in a host DLL").

Steps:
1. In `native_runtime.cpp` near line 30019 (`NebulaRuntime_crash_%lu.dmp`),
   create the directory with `CreateDirectoryW` for each path component, or
   write the dump next to the launcher's diagnostics folder
   (`%LOCALAPPDATA%\Nebula\diagnostics`, the same folder the launcher
   records into). Use `MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo
   | MiniDumpWithIndirectlyReferencedMemory` at minimum.
2. Name every worker thread with `SetThreadDescription` (DSP worker, texture
   decode pool, pipeline compile worker, async disc, async NAND, THP
   decoder, deadline broker, render thread, window thread) and print
   `[thread] name=... id=...` at creation so the crash line's thread id is
   identifiable in the log.
3. Ask the user for the next crash's dump; open it with `cdb -z` or WinDbg
   (`!analyze -v`, `~*k`). Likely culprits, in order: the pipeline compile
   worker retiring a `ID3D12PipelineState` while a frame still references
   it; `NativeThpAsyncDecoder` or texture pool freeing a resource that the
   render thread still uses; a condition variable in `DspMramTransaction
   Boundary` notified after destruction at shutdown. Fix the one the dump
   names.

Validation: a forced crash (`GALAXY_CRASH_TEST=1` if present, otherwise a
deliberate null deref behind an env flag) produces a dump in the diagnostics
folder.

### P1. Cooperative scheduler with fibers (the structural 2x)

This replaces the translated OS scheduler and the hardware interrupt
emulation with a host scheduler, as WiiCompiled does. It removes, from the
simulation thread: every GuestTransfer unwind (14 % on the laptop), the
flat continuation dispatcher and `lookup_function` at every hop (0.5 %),
the checkpoint bodies (33 thousand per second, 3 to 5 %), the deadline
broker handshakes (1.3 %), and most of the time-base reads (the runtime's
own 40 million per 238 s). It also removes the generated-code checkpoint
tests (334,128 call-return sites, the budget counters at every backward
branch), which is a measurable share of the guest 45 % itself.

Design (mirror WiiCompiled, adapt to Nebula's existing intercepts):

1. Fibers. Each guest OSThread becomes a host fiber created in the
   OSCreateThread intercept (`0x804A2FD8` region; the cached-call list at
   `native_runtime.cpp:15589` already names the scheduler entry points:
   `0x804AB20C` SelectThread, `0x804A381C` OSLoadContext, `0x804ABFBC`
   OSSleepThread, `0x804AB488`, `0x804ABFBC`, `0x804AC380`, `0x804A8A0C`
   OSWakeupThread family). Use `ConvertThreadToFiberEx` for the simulation
   thread, `CreateFiberEx(64 KiB reserve, 16 KiB commit, FIBER_FLAG_FLOAT_
   SWITCH)` per guest thread (the guest stack lives in guest RAM; the host
   fiber stack only holds translated C++ frames: measure the deepest native
   stack with the profiler's `stack-frames-max=24`; 256 KiB reserve is safe).
   Keep one `PpcContext` per fiber. A switch is: store the current fiber's
   context pointer, `SwitchToFiber(target)`; on resume, continue in the
   translated code right after the call that yielded. Nothing unwinds.
2. Scheduler. Implement SelectThread natively using the game's own thread
   structures in guest RAM (run queues at the addresses `branch_checkpoint_
   body` already reads: `0x806A2D30` run-queue bits, `0x800000E4` current
   thread, `0x800000D4` current context). WiiCompiled's `os_scheduler.cpp`
   is the reference; Nebula's `execute_yield_select_*` and the
   `is_guest_scheduler_idle_context` helpers show which fields RMGE01 uses.
   Keep the translated OSLoadContext/OSSaveContext bodies for the register
   image (they are leaf functions) but never execute the translated `rfi`;
   the OSLoadContext intercept switches fibers instead.
3. Interrupts become polled events. VI retrace, AI DMA completion, DSP
   mail, IPC reply, decrementer alarms are already published into
   `PendingEventLatch` by workers and the broker. Deliver them only at
   scheduler entries and in the idle loop by calling the game's registered
   handlers directly (the translated `__OSDispatchInterrupt` can still be
   used: build the PI cause word, call it as a normal function with a
   synthetic context, and let it return normally; its `rfi` is intercepted
   to return). WiiCompiled polls exactly these: `ProcessSleepTimers`,
   `Audio_HLE_Poll`, `VI_HLE_PollRetrace`, alarms, `VI_HLE_WaitForNextRetracePoll`.
4. Remove the checkpoints from generated code. Translator option
   `cooperative_scheduler: true` (generation 18): emit no
   `call_return_checkpoint`, no `branch_checkpoint_taken_budgeted`, no
   `architectural_interrupt_checkpoint`; `mtmsr` becomes a plain MSR store;
   `rfi` returns; `mtdec` just records the value. The residency pass then
   has no Sync::Checkpoint/CallReturn blocks, so resident registers stay in
   registers across whole functions. Keep `GALAXY_FLAG_TRANSFER` and the
   exception path only as fallbacks for modules without the new flag.
5. Long-running guest loops that wait on hardware (VIWaitForRetrace,
   `DSPCheckMailToDSP` spin, `OSGetTime` polling loops) must yield to the
   scheduler: intercept the SDK wait functions (VIWaitForRetrace at the
   `0x804D2668` idle loop pc, the DSP mailbox poll in `read_device`), and
   run the poll set from there. This is what WiiCompiled's
   `VI_HLE_WaitForNextRetracePoll` does.

Order of implementation so each step ships:
- a. Fibers only, exceptions kept: replace `throw GuestTransfer` in
  `return_from_interrupt` with a fiber switch to the resumed context's
  fiber (one fiber per guest context; the idle context gets its own). Keep
  checkpoints and the broker. This alone removes the unwinder and the flat
  continuation dispatcher. Risk: every place that caught GuestTransfer
  (`run_guest_entry` top loop, `dispatch_decrementer` catch,
  `branch_checkpoint_retaining_same_context`, the yield-select protocols,
  `fpu_unavailable`) becomes a plain return or a switch; the F20-lite work
  (P2) already enumerated these sites and the host paths that must not
  continue after a switch, so do P2's audit list first.
- b. Polled interrupt delivery at scheduler entries and the idle loop;
  delete `dispatch_external_interrupt` from checkpoints; keep checkpoints
  only as yield points (no dispatch).
- c. Generation 18 without checkpoints.
- d. Native SelectThread/OSSleepThread/OSWakeupThread/OSYieldThread (HLE),
  removing the translated scheduler and its `0x804AB360` idle checkpoint
  reload hack.

Expected gain: 25 to 40 percent of simulation-thread time on the laptop,
plus the guest-code share that checkpoints cost. This is the single
largest item and the only path to the low-end target.

Validation at each step: the 120 s title smoke (`smoke-app.ps1`), then a
gameplay smoke with the replay input log, then a laptop Detailed
recording; compare `[sim-profile]` running share and the
`[sim-profile-other-caller]` table; `[transfer]` counters must read zero
exception unwinds.

### P2. Finish flag-based interrupt return (bridge until P1a lands)

State at hand-off: implemented end to end (translator gen 17 emits
`GALAXY_RETURN_IF_TRANSFER_PENDING` in every checkpoint cold block and
after `mtmsr`/`sc`/`mtdec`; the runtime sets `kPendingEventTransferBit`
(mask bit 7) in `return_from_interrupt` instead of throwing, every
dispatcher and the checkpoint body return early while pending,
`run_guest_entry` consumes it with `take_pending_flat_transfer`, and
`branch_checkpoint_retaining_same_context` retains same-context RFIs
without any unwind). Thousands of flag transfers complete; the last
observed failure modes and their fixes:

1. Boot entry returned (fixed: consume after `entry(...)`).
2. VI boundary state machine continued after a dispatch (fixed: two guards
   in the boundary switch).
3. Residency cold blocks stored resident registers into the context before
   the transfer test (fixed: test first, templates in `residency.rs`).
4. The Home Button sidecar module (`gen10/home/home_button_native.cpp`,
   1,971 call-return checkpoints) had no transfer tests; its code ran on
   with a stale context (`[flag-retain] site=0x91F073E0`). Fixed in
   `generate_home_button_sidecar` by applying `apply_transfer_returns`
   (exported from the core crate). The validation of this fix was still
   running at hand-off: `D:\NebulaWork\native\chain10m.log`, smoke label
   `rt11-mod10-flag8`; if it failed, the traced rerun is `rt11-flag-trace5`.

5. Last observed failure (chain10n, smoke `rt11-mod10-flag9`, traced
   `rt11-flag-trace6`): a transfer set at Home Button RSO checkpoint site
   `0x91F073E0` (external interrupt, thread switch to `0x807ACCA0` resuming
   `0x804AB30C`), retention rejected (`mismatch=5`, expected), the RSO
   function returned through its checkpoint macro, and the sidecar then
   raised `[fatal] guest error at 0x804AC088: invalid static Home Button
   RSO local return`. The sidecar generator (`crates/nebula-recomp-core/src/rso.rs`
   and `generate_home_button_sidecar` in the CLI) emits, after every call
   to another sidecar function, a static local-return validation; it must
   emit `GALAXY_RETURN_IF_TRANSFER_PENDING(services);` between the call and
   that validation (and before any other post-call code). The runtime side
   already guards `home_button_rso_try_call` and `resume_home_button_rso_native`.
   After that fix, rerun the chain; the next failure, if any, will again be
   named by the `[flag-set]/[flag-retain]/[flag-take]` trace.

How to continue: run `chain10n.ps1` (scratchpad of session 0d216f6c) or
its three steps by hand (cargo test, `build-rt.ps1`, `gen-mod.ps1 -Gen
gen10 -Mod mod10`, `smoke-app.ps1 ... -Module mod10 ... -Set
GALAXY_FLAG_TRANSFER=1`). A failure prints either the entry assertion
`translated code called a guest function while a flat transfer is pending`
(a missed propagation site: find it with `GALAXY_TRACE_FLAG_TRANSFER=1`,
whose `[flag-set]/[flag-take]/[flag-retain]` lines name the RFI pc, the
resume address and the checkpoint site) or a scheduler identity failure
(stale code wrote the context: look for a host path that continues after
a dispatch; the audit list is the `pending_transfer.active` guard sites,
`grep -n "pending_transfer.active" runtime/src/native_runtime.cpp`).
The runtime default is `GALAXY_FLAG_TRANSFER=0` until a 120 s title smoke
and a gameplay smoke pass; flip it in `read_env_flag("GALAXY_FLAG_TRANSFER", ...)`.

Expected gain when it lands: 10 to 14 percent of simulation-thread time on
the laptop (the unwinder share), measured by `[transfer] exception-unwinds`
going to zero and the `return_from_interrupt`/`dispatch_decrementer`
other-caller rows disappearing.

### P3. AX audio HLE instead of the DSP coprocessor

Evidence: the DSP worker is the second-busiest thread on the laptop (17 %
of a core, thread id 33760/20916 in the recordings); the simulation thread
spends 2.5 % spinning in `read_device+0x8a0` waiting for the worker to
take a mail, 2 % notifying MRAM transactions, and takes the remaining
0xCD000000/0xCC005000 faults for mailbox and ARAM traffic; `[fastmem]`
mmio counts are dominated by it; the previous session's DSP sync pacing
and mailbox waits are all workarounds for a coprocessor model.
WiiCompiled runs `hle/audio/ax_mix.cpp` (a host AX mixer polled at 4 kHz)
and Dolphin's default is AX HLE for the same reason.

Steps: implement the AX ucode protocol in host code (Dolphin's
`Source/Core/Core/HW/DSPHLE/UCodes/AX.cpp` and `AXWii.cpp` are the
reference, GPLv2; WiiCompiled's `ax_mix.cpp` is GPLv3 and already shaped
for a recompilation runtime): parse the AX command list the game writes
(`AXCommand` list from the mailbox `0xCDD1`/`0xBABE` protocol), mix voices
(PB chains in MRAM/ARAM) into the output buffers the game expects, run it
from the audio output thread at the AI DMA cadence, and acknowledge mails
immediately (the busy bit clears on write). Keep the compiled DSP module
(`RMGE01_dsp.dll`) behind `GALAXY_DSP_NATIVE=1` for A/B. The JAudio
command framing is documented by the `[dsp-cmdsnoop]` trace already in
`dsp_native_forward_mail`.

Expected gain: 4 to 6 percent of the simulation thread, a whole worker
thread (important on a 4-core i7), and the end of the DSP-related stutter
sources (MRAM transaction waits, `poll_native_dsp` blocked samples).

### P4. Checkpoint substrate (until P1c removes it)

Evidence: 33 thousand bodies per second on both machines; the body reads
the time base about five times (`runtime_time_base_ticks` twice,
`now_ticks`, `synchronize_decrementer_deadline`, phase timers), runs up to
23 phases, takes the latch lock (`lock_slot` 226 samples), and
`publish_next_inline_checkpoint_slow` schedules the next body at
`current + 1` whenever any of: VI pending, input pending, DEC pending,
AI deadline, AI playing fallback probe, host pump, DSP, IPC, PE level is
set. On the title screen that is nearly always.

Steps (each independently measurable with `[host-census]`):
1. One time-base read per body: read once at entry, pass the value to
   `synchronize_decrementer_deadline` and the phases (add an overload
   taking `now_ticks`).
2. Make the "current + 1" schedules proportional: a pending but
   undeliverable event (EE masked) should not force a body at the next
   checkpoint; use `current + 64` while `!guest_external_interrupts_enabled`.
3. Move `record_checkpoint_gap` and `record_vi_service_checkpoint` behind
   the Detailed-recording flag (they are diagnostics).
4. Raise `kBranchCheckpointBudget` from 32 to 128 in `native_api.h` (the
   inline countdown costs nothing, the counted path less often).

Expected gain: 2 to 4 percent now; moot after P1c.

### P5. Laptop stutters: timer resolution, power throttling, thread QoS

Evidence: `[checkpoint-gap-record]` entries of 19.8, 32.3, 48.1, 54.8 and
75.3 ms, all from the idle loop (`previous-pc=0x804AB360`) to the VI
handler (`current-pc=0x804A87BC`): the idle wait overslept by one to three
clock ticks. The runtime never calls `timeBeginPeriod` and never opts out
of Windows 11 power throttling; it does use `CREATE_WAITABLE_TIMER_HIGH_
RESOLUTION`. Windows 11 ignores a process's timer-resolution requests when
its window is occluded or it is classified EcoQoS, and on a laptop on
battery the scheduler parks the simulation thread on an E-core unless told
otherwise.

Steps (all in `native_runtime.cpp` startup, before the simulation thread
starts):
1. `timeBeginPeriod(1)` once (winmm). Pair with `timeEndPeriod` at exit.
2. `SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state,
   sizeof state)` twice: `ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_
   SPEED, StateMask = 0` (never EcoQoS), and `ControlMask = PROCESS_POWER_
   THROTTLING_IGNORE_TIMER_RESOLUTION, StateMask = 0` (always honor the
   timer resolution request).
3. `SetThreadInformation(..., ThreadPowerThrottling, ...)` with the same
   masks for the simulation, render, DSP and audio threads.
4. MMCSS: `AvSetMmThreadCharacteristicsW(L"Games")` for the simulation and
   render threads (the audio thread already uses "Pro Audio"); the code
   path exists (`AvSetMmThreadCharacteristicsW` at line 30392) but check it
   is applied to the simulation thread.
5. The idle wait's slice: `idle_loop_wait_for_next_deadline` waits up to the
   next VI deadline with a 500 us guard; after the above, measure again. If
   gaps remain, spin-wait the final 300 us (the broker already does
   `wait_realtime_vi_duration_high_res`).
6. Keep the window visible and unoccluded detection: when the window is
   minimized, Windows ignores the timer request regardless; that case can
   legitimately throttle.

Validation: a laptop Detailed recording with zero `[checkpoint-gap-record]`
entries above 17 ms outside loads.

### P6. THP video decode on the laptop (intro and cutscene stutter)

Evidence: `NativeThpAsyncDecoder::take_finished` is 8,836 blocked samples
(5.5 % of all samples, 9 s over 238 s); `[thp-hle-video-join]` shows
decode 2.5 to 2.8 ms per 640x368 frame and wait 2.0 to 2.3 ms: the
decode-ahead worker is only one frame ahead and the simulation thread
waits for it almost every video frame. The early present windows (44 to
49 new frames per second) are the intro video.

Steps:
1. Decode two frames ahead (ring of three plane sets) in
   `NativeThpAsyncDecoder`; join only when the frame the game asks for is
   not ready.
2. SIMD the IDCT and color conversion (`thp` decoder: 8x8 blocks, SSE2
   integer IDCT as in libjpeg-turbo's `jidctint-sse2`; YUV to the planes
   the game expects). 2.6 ms per frame on a 185H means the decoder is
   scalar.
3. Measure with `[thp-hle-video-join]`: wait-mean-us must be near zero.
4. Confirmed: `runtime/src/native_thp_video.cpp` contains no SSE intrinsics at all (scalar IDCT, scalar color conversion); the keyboard poll (`collect_native_key_levels` over `kFunctionalVirtualKeys`, `native_host.cpp` line 8046) calls `GetAsyncKeyState` once per key per poll, which is the `NtUserGetAsyncKeyState` row in the laptop profile.

### P7. 1080p internal resolution on the iGPU

Evidence: at EFB scale 3 the simulation thread share is unchanged (61 %),
so the slowdown the user feels is the GPU. Both laptop runs still present
60.0 fps on average with `frame-ms(max)` 18.4 to 19.0 ms, so what the user
sees at 1080p are per-frame spikes (occasional dropped presents: 11,700
requested, 11,693 presented), not a lower average. `[efb-copy-work]` for the
255 s run at 3x: 50,820 shader copies (200 per second) and 2,694 identity
copies carrying 28.3 GB of payload, against 3.1 GB for the same copies at
1x: each identity copy at 3x moves about 10.5 MB through the iGPU's shared
memory bus, which is the first thing to remove. `[efb-copy-work]` reports
76,134 EFB copies in 9 minutes (141 per second, more than two per frame)
and 2.8 GB of identity-copy payload; at 3x scale every copy is 9x the
pixels, and the Arc iGPU shares memory bandwidth with the CPU.

Steps:
1. Record a GPU timeline on the laptop (the launcher's Detailed recording
   already has `detailedRendererTimings`; extend `renderer_d3d12.cpp` to
   print per-pass GPU timestamps: EFB draws, each EFB copy, XFB copy,
   present) and find the pass that exceeds 16 ms at 3x.
2. EFB copies that feed textures (`shader-copies`) should be made at the
   copy's native destination size unless the copy is the main XFB; most of
   the 141 per second are bloom/depth-of-field intermediates that the game
   immediately downsamples. Add `GALAXY_EFB_COPY_SCALE_CAP` (default 1 for
   non-XFB copies when the EFB scale is above 1) and compare frame times.
3. Skip identity copies with a cached-resource alias; the counter already
   separates them. The path is `renderer_d3d12.cpp` line 5484:
   `detail::efb_identity_copy_box` selects a copy whose destination layout
   equals the EFB color target, and `detail::record_efb_identity_copy`
   issues a `CopyTextureRegion` of the whole scaled EFB (10.5 MB at 3x,
   2,694 times in 255 s). Replace it by registering the EFB color texture
   itself (or the previous frame's retired EFB target, which the renderer
   already double-buffers for `peek`) as the copy destination for the
   frames until the next EFB write, and only materialize a copy when the
   EFB is next cleared while a consumer still references it.
4. On integrated GPUs prefer `D3D12_HEAP_TYPE_UPLOAD` resources read
   directly by the GPU for vertex rings (UMA is already detected:
   `unified-memory=1`), and avoid readbacks (the depth peek field is
   already asynchronous).
5. The 6th-generation i7's HD 530 is 3 to 4x slower than the 185H's Arc;
   480p internal with a 1080p output blit is the configuration to validate
   there, and the EFB copy cap matters even at 1x.

### P8. Working set 5.3 GB

Evidence: `process.csv` working set reached 5,316 MB (run 030107) and
5,104 MB (run 025642) on a 16 GB laptop. An 8 GB target cannot hold that
plus Windows. Suspects, in order: the memory-mapped `game.pak` (every byte
read stays resident; the disc image is over 2 GB), the texture pool (256
MB cap) plus live textures, the 240 MB GPU rings, the pipeline cache and
shader blobs, dbghelp with `NebulaRuntime.pdb` loaded for the profiler's
symbolization (tens of MB), and the dirty-page bitmaps.

Steps:
1. Print `[memory] private=... working-set=...` every 10 s in the launcher's
   process sampler and at exit, with the big owners: pak mapping resident
   bytes (`QueryWorkingSetEx` over the mapping), texture pool bytes, ring
   bytes, GPU resident bytes (`IDXGIAdapter3::QueryVideoMemoryInfo`).
2. Read the disc with unbuffered `ReadFile` into pooled buffers instead of
   a persistent mapping, or `VirtualUnlock`/`OfferVirtualMemory` the
   mapping ranges after the read-ahead window passes; cap the pak's
   resident set at 256 MB.
3. Lower the texture pool cap to 128 MB on 8 GB machines (read total
   physical memory at startup).
4. Symbolize the profile offline (the PDB is shipped; resolve in the
   launcher after the run, not in the game process).

### P9. Syscall and hot-path leftovers (measured, small, cheap)

| Item | Evidence | Fix |
| --- | --- | --- |
| Pointer provider polls `GetAsyncKeyState` per key | 779 to 907 samples (`NtUserGetAsyncKeyState`), `install_host_pointer_transition_provider+0x1690` | Poll the keyboard once per report with `GetKeyboardState`, or move the poll to the input thread and publish a snapshot; raise `GALAXY_HOST_POINTER_POLL_CACHE_MS` to 8 |
| `strchr` in a hot path | 1,559 samples; caller symbol misattributed (`basic_stringstream` +0x7360) | Confirmed candidates in `native_host.cpp`: `virtual_wiimote_count()` (line 523, a `getenv_s` per call, called from the input poll path), and the uncached generic readers `read_env_float_setting`, `read_env_int_setting`, `read_env_u64_setting`, `read_env_path` (lines 1197 to 1329) wherever a caller invokes them per poll or per frame instead of once into a `static const`. Cache every one of them; then re-profile |
| `GetThreadTimes` | 898 samples at a 1 ms cache | Cache for 16 ms (a suspension candidate needs a whole retrace) or drop the CPU-time evidence in favor of the TSC-based gap test |
| `QueryThreadCycleTime` | 242 samples | Same as above |
| Deadline broker wake per DEC replace | `replace_once` 1,189 samples, `ZwSetEvent` 699 | Do not wake the worker when the new deadline is later than the one it sleeps toward (already) and when it is within 100 us of it; batch DEC rewrites that happen inside one checkpoint |
| `ZwAlertThreadByThreadId` 1,740 | WakeByAddress from latch publications and the DSP MRAM notify | After P3 most disappear; otherwise coalesce notifies per checkpoint |
| `drain_guest_memory_writes` 780 | dirty-page bitmap scan per frame | Track only the display-list and vertex-array ranges the parser depends on (the dependency cache knows them) instead of 320 MiB of pages |
| `lookup_function` 425 | hash lookup per flat hop | Use the direct dispatch table for interior entries; moot after P1 |
| Remaining faults: IPC 0xCD000004 (2,000 per s), VI 0xCC002000, CP 0xCC000000, PE 0xCC001000 | `[fastmem-hot-address]` | These are pointer-based accesses (`__IPCRegs`-style globals). Add a translator rule: an address loaded from a known global whose value is a constant in the DOL's data section (the translator can read `.sdata`/`.data` initial values) is static; or in the runtime patch the hot sites by backpatching the faulting instruction to a `call` thunk when the same (pc, page) faults more than N times, the Dolphin way |

### P10. Translator-level gains in guest code (45 % of running time)

1. Float residency (F5 float) is implemented but off; re-measure on top of
   fast float with the module benchmark, using the SAME harness binary for
   both sides (the harness overhead differs between builds; see section 4).
2. Generation 18 without checkpoints (P1c) frees every resident register
   across calls; expect the biggest guest-code gain.
3. `lmw`/`stmw` as 16-byte vector moves with byte swaps.
4. The direct dispatch table withholds 2,149 entries; list why
   (`[direct-dispatch] withheld`) and shrink the withheld set.
5. Inline the WGPIPE check out of `fast::store` once P1 intercepts GX at the
   call level (GXBegin/GXEnd intercepts already exist as hand-written
   natives: `native_gx_begin_804BA6B0`).

### P11. Render thread (18 % of a core on the laptop; matters on 4 cores)

Measure first (per-pass CPU timings are in the Detailed recording):
`FifoParser::run_window`, `decode_vertex_descriptor`, texture decode (SSE
decoders exist for the common formats; CMPR and I4/IA4 may be scalar),
pipeline compile (first-use is asynchronous), EFB copies. On a 4-core
6th-generation i7 the simulation thread, render thread, DSP worker and
texture workers compete; P3 removes one, and the texture decode pool
should be sized to physical cores minus two.

### P12. Low-end target specifics (i7-6xxx, HD 530 or GT 1030, 8 GB)

- CPU features: Skylake has AVX2, FMA3, MOVBE, BMI2: the AVX2 module is
  fine. No AVX-512 anywhere.
- Memory: P8 is mandatory; also disable the 128 MB vertex ring reservation
  growth beyond 64 MB on 8 GB machines.
- GPU: GT 1030 is feature level 12_1 and fine; HD 530 is 12_1 at low
  throughput: 480p internal, 720p or 1080p output, EFB copy cap (P7.2),
  bloom at half resolution.
- The installer compiles the module on the user's machine: 585 MB of
  source at 24 jobs took 140 s on the 9950X3D; on a 4-core i7 budget 20 to
  30 minutes and keep PCH on.

## 3. Measurement and tooling (use these, do not re-derive)

- Work dirs: `D:\NebulaWork\native\{rt, gen10, mod10, gen9, mod9,
  Nebula-Native10-win64-AVX2, smoke\<label>}`. Session 0d216f6c scratchpad
  (`C:\Users\vaibh\AppData\Local\Temp\claude\C--Users-vaibh-Documents-MarioGalaxyRecomp\0d216f6c-640f-49bb-a3d6-8ffe98d05e19\scratchpad`):
  `build-rt.ps1` (runtime build + 60 tests), `gen-mod.ps1 -Gen gX -Mod mX
  [-SkipGenerate -SkipCargo -ExtraDefines]` (translator build, generate,
  module build, bench), `smoke-app.ps1 -Package ... -App ... -Module ...
  -Label ... -Seconds ... -Set NAME=VALUE,...` (copies a package, swaps
  the runtime from `rt` and the module, runs headless with the replay
  input log, prints the exit lines), `chain10*.ps1` (sequenced jobs),
  `diag_digest.py <recording dir>` (digest of a launcher recording),
  `asm-census.ps1` + `asm_census.py` (compile shards to assembly and
  histogram memory forms), `patchlib.py` (+ `patch_*.py`, the edits of this
  session as reproducible scripts).
- Long jobs: start them detached (`Start-Process powershell -File ...
  -RedirectStandardOutput D:\NebulaWork\native\<name>.log`) and wait with a
  background `until grep -q "=== done" ...` loop; the tool call limit is 10
  minutes. Never run two runtime builds concurrently (object files lock).
- Profiles: `GALAXY_SIM_PROFILE=1` prints `[sim-profile*]` at exit:
  `running/blocked`, `guest`, `host` (symbolized when `NebulaRuntime.pdb`
  sits beside the exe), `other` (foreign modules), `other-caller` (the
  runtime function that called into the foreign module), `blocked-site`.
  `[host-census]` counts time-base reads, checkpoint bodies, CPU-time
  syscalls. `[transfer]` counts RFI unwinds by kind. `[fastmem]` and
  `[fastmem-hot-address]` count faults.
- A/B rule: compare the same harness/package with one variable changed, 120
  s, machine otherwise idle; the title-screen running share has about 1
  percentage point of noise.
- The module benchmark (`D:\NebulaWork\native\bench\bench_f*.exe`) measures
  harness overhead too: `bench_f6.exe` (built Oct 9) reports about 30 ns
  less per call than any harness rebuilt from `bench_module.cpp`. Only
  compare runs of the same harness binary.
- Shell pitfalls on this machine: quoted heredocs collapse `\\` to `\`
  (write patch scripts with the Write tool, not heredocs); `sed` replacement
  strings mangle backslashes; the Bash tool blocks `sleep`-only commands.

## 4. Session log of the Native10 work (what changed, for the record)

Static device lowering (translator `discover_static_device_access_pcs`);
plain fastmem forms and the extended fault decoder; live render memory as
default; TSC timeline; producer pools; config changes; census and transfer
counters; F20-lite as described in P2. Details and measurements are in
`docs/NATIVE_EXECUTION.md` ("Native10 session"). The benchmark "regression"
reported at hand-off from the previous session was a harness artifact.

## 5. References

- WiiCompiled: github.com/patchzyy/Wiicompiled, `runtime/src/hle/os/os_scheduler.cpp`
  (SelectThread_801a9c08, WakeupThreadQueue, idle poll set),
  `runtime/include/fiber_manager.h` (GuestFiberManager), `runtime/src/hle/audio/ax_mix.cpp`,
  `runtime/src/hle/vi.cpp`, `runtime/src/guest_flat_memory.cpp` (fault-backed flat memory, `FlatWriteNeedsPolicy`).
- Dolphin: `Source/Core/Core/HW/DSPHLE/UCodes/AX*.cpp` (AX HLE),
  `Source/Core/Core/PowerPC/JitCommon/JitBackpatch` (fault backpatching),
  `Source/Core/Core/Fifo.cpp` (dual-core sync, SyncGPU).
- Windows: `SetProcessInformation(ProcessPowerThrottling)` with
  `PROCESS_POWER_THROTTLING_EXECUTION_SPEED` and
  `PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION` (Windows 11 ignores
  timer-resolution requests of occluded or EcoQoS processes);
  `timeBeginPeriod`; `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`; MMCSS "Games".
