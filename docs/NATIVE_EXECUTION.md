# Native execution model

Status of the performance program recorded on 2026-10-10. Items are numbered
F1 to F23 as in the root-cause analysis. "Shipped" means the change is in this
tree and takes effect through the normal install (the installer compiles the
generated module on the user's machine; the runtime ships prebuilt).

## Root cause

Both machines were bound on the single simulation thread, never on the GPU.
Each guest float instruction was two out-of-line calls computing exact FPSCR
status, each load/store a checked region lookup, each virtual call a host
service boundary with address comparisons, and each gather-pipe store a device
dispatch. GPU time per frame was under 0.3 ms on an Intel Arc iGPU.

## Shipped

| Item | Change | Where |
| --- | --- | --- |
| F1 | Native scalar fadd/fsub/fmul/fdiv/fmadd family/frsp, single and double | `native_fast_paths.h`, translator `native_float` |
| F2 | Native paired single (ps_add/sub/mul/muls/div, ps_madd family, ps_sum), quantized loads/stores on the flat view | same |
| F3 | Flat 4 GiB guest view is the default memory model; generated loads and stores address it directly | `native_host.cpp`, translator `flat_ram_reads`/`flat_ram_writes` |
| F7 | `-mfma` with contraction off so explicit `std::fma` is one instruction on AVX2 hosts | generated `CMakeLists.txt` |
| F9 | GXPeekZ and GXPeekARGB answered from asynchronous 640x528 depth and color fields (one per peek kind, private resources); no FIFO drain or GPU round trip after the first peek of each kind. Before the color field, 32,851 synchronous color peeks in a five-minute A/B blocked the simulation thread 7.3 s in total (8.1 ms maximum) | `gx_backend.cpp`, `renderer_d3d12.cpp`, `gx_efb_peek` |
| F15 | Simulation and render threads restricted to performance cores on hybrid CPUs; simulation thread at HIGHEST | `windows_power.h`, `configure_runtime_scheduling` |
| F19 | `bctrl`/`bclrl` dispatch through a direct native entry table published by the runtime | `native_fast_paths.h`, `build_direct_dispatch_table` |
| F21 | Gather-pipe stores append to the host's live 32-byte burst buffer in place | `GuestMemoryV1::wgpipe_gather_fast` |
| F22 (part) | Flip-discard swap chain by default | `runtime-env.json`, `render_config.h` |
| F24 | Idle-loop wait, on by default: with an empty guest run queue the simulation thread blocks on the deadline latch's event mask (`WaitOnAddress`) instead of spinning a core. Every latch publication (VI, AI, DEC, input, DSP, IOS) and every PE finish/token raised by the render thread wakes it; the timeout is bounded by the next VI deadline, the AI DMA deadline and a 4 ms slice with a 250 us guard, so a missed wake cannot delay a device edge beyond the existing polling. `GALAXY_IDLE_LOOP_SLEEP=0` restores the spin. The earlier timer-slice version was opt-in after a laptop session failed the exact VI cadence check; the event-driven wait wakes at publication, which is when the spinning loop would have observed the edge | `idle_loop_wait_for_next_deadline`, `PendingEventLatch::wake_idle_waiters` |
| F11 | Decoded packet-run cache entries own GPU-resident vertex blocks (`ResidentVertexArena`); replays no longer re-copy vertices into the ring; dependency snapshots are merged into contiguous spans for validation | `vertex_loader.cpp`, `gx_backend.cpp` |
| F13 | First-use texture decode on a worker pool against the sealed frame snapshot, joined before submission; SSE2/SSSE3 tile decoders for RGBA8, RGB565, RGB5A3, IA8, I8 | `texture_cache.cpp`, `texture_decode_pool.h` |
| F6 (FPU) | Eager FPU: no MSR[FP] test per region and no exception-7 retry entry per FP instruction. The runtime performs the lazy owner switch of RMGE01's exception-7 handler itself (translated 0x804A3604 save / 0x804A34E0 load, `__OSFPUContext`, SRR1 and MSR[FP]) at OSSetCurrentContext, the OSLoadContext boundary, exception entry and RFI. Interior entries 287,930 to 178,776 | `eager_fpu_context_switch`, translator entry discovery |
| F5 (integer) | Register residency pass (`crate::residency`): GPRs, LR and CTR are function locals between synchronization points. Every context-receiving call not proven to leave the integer file alone stores the written locals first and reloads after; `call_return_checkpoint` reloads; `call_return_checkpoint` is a full synchronization point as well (an inline wrapper can write a register right before it); backward-branch checkpoints split into an inline `branch_checkpoint_due` test and a cold stored/reloaded `branch_checkpoint_taken_counted`; interior entries reload through stubs; every `return` stores. lmw/stmw lower to one statement per register. Unknown helpers are full synchronization points. 25,937 of 41,941 functions resident; the module benchmark (FP-heavy) is unchanged (82.5 to 83.2 ns), the gain is in integer game logic | `residency.rs`, `native_api.h` |
| F1/F2 (fast float) | Fast float is the release float model (translator `fast_float`, `#define GALAXY_FAST_FLOAT 1` at the top of the generated `functions.h`, generation 14). Every `fast::scalar_*`/`paired_*`/`fctiwz` helper computes natively with no exact fallback: binary64 as written, single forms rounded once to binary32 (`cvtsd2ss`), 25-bit multiplier rounding kept, fmadd family fused, paired singles as one `__m128d` per operand, fctiwz saturating. FPSCR status, NaN payloads and non-nearest rounding are not reproduced. The module exports `galaxy_module_float_flags`; the runtime sets MXCSR FTZ+DAZ on the simulation thread (Gekko non-IEEE mode; `GALAXY_FAST_FLOAT_FTZ_DAZ=0` leaves it). Decided by the user on 2026-10-10 in favour of performance over bit-exactness. Module benchmark 83.2 to 33.4 ns mean per call; matrix routines 0x804B6F90 634 to 50 ns, 0x804B6008 73 to 15 ns, 0x804B6548 298 to 124 ns; 16 float operations compile to 82 instructions and no calls. The exact model stays available (`fast_float: false`); the whole module then carries 33 exact commits (fres/frsqrte) instead of one per float instruction | `native_fast_paths.h` (`fast_float_detail`), translator `apply_fast_float_mode`, `galaxy_native_fast_float_tests` |
| F9 (pointer) | Mouse pointer depth (`0x80385034`) answers from the previous scene's completed owned depth field (double-buffered `NativeMouseDepthFrame::ready`) instead of waiting for the current scene's GPU readback: 12,229 waits, 11.7 s total and 82 ms maximum in the five-minute A/B. The pointer's screen position is the current sample; its depth is one scene old. `GALAXY_NATIVE_MOUSE_ASYNC_DEPTH=0` restores the wait | `native_runtime.cpp` scene boundary 0x804B9EC8 / transaction 0x80385034 |
| F4 | Flush-based dirty tracking: translated stores (scalar, paired, stmw, dcbz) write RAM with no publication; `dcbf`/`dcbst` publish their 32-byte line to both trackers; display-list gather bursts publish on the host. `dcbi`/`icbi` lower to nothing | `native_fast_paths.h` `store`/`dcache_flush_line`, `flush_wgpipe_gather_burst` |

Measured on the build host (9950X3D) at a steady 60 fps on the title route:
the simulation thread fell from 95 to 46 percent of a core and process CPU from
1.29 to 0.81 cores (F24); 8,688 of 8,690 depth peeks were answered from the
asynchronous field (F9); the direct dispatch table published 326,540 entries
and withheld 3,331 (F19).

Module benchmark (`bench_module`, SDK matrix library, 36 functions, 9950X3D):
mean 103.4 ns per call at ABI 24, 82.5 at ABI 25 with F6 and F4. Store-heavy
routines 2 to 3x (PSMTXIdentity 32.7 to 12.2, PSMTXCopy 39.6 to 18.1,
0x804B6328 247.7 to 155.6); scalar routines 0x804B6F90 and 0x804B6DC4 remain
at 638 and 233 ns, bound by context memory traffic and the load/store slow
paths (see below).

Native ABI is 26; generation version is 17. Every module must be regenerated.

Hand-written native substitutions stay disabled for functions that execute FP
instructions: they use the exact out-of-line float helpers and measured 1.3 to
3.4x slower than the generic native lowering (0x804B6548 396 to 723 ns).

## Float models

Fast float (release): plain IEEE binary64 arithmetic on the host, single
precision as one rounding of the binary64 result to binary32, fused
multiply-add, 25-bit multiplier rounding, saturating fctiwz, both paired lanes
in one vector, FTZ+DAZ on the simulation thread. Not reproduced: FPSCR status
bits, NaN payload/sign propagation, non-nearest rounding modes, binary32
double-rounding corners. This is the accuracy class of Dolphin's JIT and
WiiCompiled. fres/frsqrte/ps_res/ps_rsqrte keep the exact table estimates.

Exact (`fast_float: false`), admission rules of the native path: inputs finite
and zero-or-normal, FPSCR rounding mode nearest-even. Results that are
non-finite, subnormal, or (single precision) exactly on a binary32 rounding
midpoint take the exact helper, as does every input the rules reject. The
native path does not maintain FPSCR status bits; the exact path and every
status consumer still do. Measured cost of this model: every float instruction
keeps an out-of-line exact call, and the host compiler spills guest floats
around each one (FPR residency on top of it measured 87.9 vs 83.2 ns).

## Direct dispatch rules

The runtime publishes one entry per callable address. Entries stay null for
every target in the translator's host-call list, the 0x8041xxxx range, every
address the cached-call service compares against, and any target the live
intercept predicate rejects at boot. Generated code also defers to the service
while a device event is pending or an external interrupt dispatch is live.
Call-tracing and profiling diagnostics disable the table.

## Laptop session 2026-10-10 (Core Ultra 185H, Arc iGPU, fast-float build, 9 min)

Findings from the user's detailed diagnostics recording and what changed:

- Idle-loop wait oversleep: 117 of the 128 worst checkpoint gaps were at the
  idle loop (pc 0x804D2668), p50 32.5 ms, minimum 16.9 ms: one or two system
  clock ticks. `WaitOnAddress` timeouts are quantized to the clock interrupt
  regardless of `timeBeginPeriod`. Every two-second window had a 50 ms frame
  production gap and AI DMA completions ran 18 to 32 ms late (54 recovery
  probes), which is the audio lag heard in cutscenes. Fixed: the idle wait now
  blocks on an auto-reset event (signaled by every latch publication and PE
  wake) plus a high-resolution waitable timer, the same mechanism the deadline
  broker already used; slice 16 ms, guard 500 us.
- Pointer depth mid-frame FIFO split: 5,687 scene boundaries cost 13.7 s of
  simulation time (2.4 ms each, 8.7 ms max) plus 15.2 s of render-worker time,
  because each boundary submitted the partial frame as its own chunk and
  queued a utility capture. Fixed: the boundary records the FIFO offset and
  the frame's own submission captures the field when the parser reaches it
  (`attach_pointer_depth_capture`, `record_pointer_depth_capture_in_frame`),
  completing at the frame fence.
- First-use pipeline compiles: 36 misses at 20 to 163 ms each (FXC level 1 is
  98 percent of it); one frame parsed for 798 ms, which backed the render
  queue up and stalled the simulation thread 784 ms. Changed: first use
  compiles with `D3DCOMPILE_SKIP_OPTIMIZATION` and publishes at once; a
  below-normal worker compiles the level-1 pair, builds and stores the
  optimized pipeline and replaces the published one (retired pipelines stay
  alive for frames in flight). Only optimized blobs reach `shaders.bin`.
  `GALAXY_PSO_FAST_FIRST_COMPILE=0` restores the synchronous level-1 compile.
  The installer seed cache was refreshed from the development machine's cache
  (349 keys).
- Input latency: KPAD sample to Present return measured 59 to 65 ms mean, p95
  75 to 99 ms, maximum 185 ms. The game's own double buffering accounts for
  20 to 33 ms; the rest was render queue backlog and the swap-chain queue.
  `GALAXY_RENDER_QUEUE_DEPTH` 4 to 2 and `GALAXY_SWAP_CHAIN_FRAME_LATENCY`
  2 to 1 in `installer/runtime-env.json`; to be re-measured.
- THP video decode ran 4 to 6 ms per 640x368 frame on the simulation thread
  (12,667 frames in the session). The bit reader assembled up to four bytes
  per call and the Huffman slow path read one bit at a time; it now keeps a
  64-bit cache with peek/skip and whole 8x8 blocks are written as 8-byte rows.
  Decoder tests (bounds, EOF, tiled image) unchanged. To be re-measured.
- First-use texture resource creation took 24 to 36 ms (`CreateCommittedResource`
  on the Arc driver), which is also what the 20 to 43 ms "texture-flush"
  stalls were. Retired decoded textures (GPU-idle at slot reuse, RGBA8 2D) now
  go to a pool capped at 256 MB (`GALAXY_TEXTURE_POOL_MB`) and a miss of the
  same size and mip count reuses one with a PSR-to-copy barrier.
- Simulation thread load on the 185H: 76 percent of one core on average,
  85 to 93 percent in gameplay windows, 98 percent peaks (threads.csv), so the
  laptop is close to CPU-bound on that thread even with fast float. Two
  changes follow from it. Budgeted backward-branch checkpoints (generation
  15): every translated function keeps a local countdown and runs the
  checkpoint gate once per `kBranchCheckpointBudget` (32) backward branches
  (`branch_checkpoint_taken_budgeted`, `branch_checkpoint_due_budgeted`); the
  gate advances the checkpoint counter by the budget so the count-based
  schedules keep their calibration. The other iterations cost a decrement and
  a predicted branch instead of four service loads, a counter store and an
  atomic load. A built-in sampling profiler (`GALAXY_SIM_PROFILE=1`, on in the
  shipped runtime-env.json and in the launcher's Detailed recording) suspends
  the simulation thread once per millisecond and prints `[sim-profile-guest]`
  (guest function, samples, percent) and `[sim-profile-host]` (runtime image
  offset, resolved offline against the PDB) at shutdown; the next recorded
  session says where the remaining time goes.
- Still open: one 31.7 ms EFB texture copy; F20 fibers (31 unwinds/s).

## Native9 session 2026-10-10 (laptop Native8 recording and desktop named profile)

The laptop recording with Native8 (9 minutes, Core Ultra 185H) and a desktop
session profiled with symbols decided this round. Native ABI is 26 and the
generation version is 16; every module must be regenerated.

What the recordings showed:

- The 32 ms stalls (126 of the last 128 checkpoint gaps, p50 32.5 ms, one
  every 1.3 s, 417 host-suspension recoveries) all ended at the return of the
  IPC register write 0x804D1F10: a /dev/di read copying from the
  memory-mapped game.pak on the simulation thread, page-ins included. They
  were never the idle loop. Frame production ran at 43.7 new frames per
  second with 24.5 percent duplicate presents, which is also the "pointer
  polling" feel.
- Per gameplay frame on the simulation thread: the render memory snapshot's
  dependency scan p50 0.98 ms, p90 1.9 ms, max 14 ms, and the snapshot copy
  p50 0.66 ms (3.2 MB), p90 1.3 ms (6.3 MB). Input polling p50 274 us, p90
  913 us per 10 ms report: XInputGetState on the three empty controller slots.
  The FIFO is 61 to 126 KB per frame, 2,000 to 4,000 gather-pipe bursts.
- Desktop profile by symbol: GetThreadTimes on every time-base read was 3.2
  percent of all simulation-thread samples, SetEvent per publication 1.1
  percent, QueryPerformanceCounter 0.4 percent, condition-variable wakes 0.4
  percent; C++ exception unwinds (GuestTransfer) about 0.6 percent; the FIFO
  dependency scan and memory snapshot on the simulation thread; the context
  poison diagnostic on every exception save.
- Generated code (functions_0300 compiled to assembly): every guest load or
  store spent about eight instructions on the alias-table range test plus a
  spilled base-pointer reload, and every call return stored and reloaded all
  resident registers around the checkpoint.

What changed:

| Item | Change | Where |
| --- | --- | --- |
| Disc reads | /dev/di reads copy on a worker (`AsyncDiscState`), the IPC reply posts from the simulation thread when the bytes have landed, and `PrefetchVirtualMemory` reads the next 4 MB ahead (`GALAXY_DISC_PREFETCH_BYTES`). `GALAXY_ASYNC_DISC_READS=0` restores the synchronous copy. Boot smoke: 1,222 reads, 29.9 MB, maximum copy 229 us off the critical thread | `native_host.cpp` |
| F3b fastmem | Modules compiled with `GALAXY_FASTMEM` (generation 16, clang) load and store through the flat view with no range test: fixed instruction forms (`movbe`/`movzx` loads, `movbe`/`mov` stores, `-mmovbe`) emitted by inline assembly in `native_api.h`. Accesses outside the mapped RAM aliases fault into a vectored handler that decodes those forms, completes the access through `GuestAddressSpace::fault_access` (devices first, then any region such as the locked cache) and resumes. The write-gather pipe keeps one masked compare in `store` and `psq_store` because every GX vertex passes through it. `[fastmem]` and `[fastmem-hot-page]` lines at exit report the fault counts by guest page | `native_api.h`, `native_fast_paths.h`, `flat_guest_memory.cpp` |
| F21 (batch) | The live gather buffer is 4,096 bytes (ABI 26, `kWgpipeGatherFastBytes`); generated stores append in place and the host runs once per buffer. `drain_wgpipe_gather_bursts` lands every complete 32-byte burst in the CPU FIFO as contiguous runs (one publication per run) before any CP/PE/VI/PI register access, the frame handoff (`take_gx_fifo`) and the frame-boundary tail flush, so nothing can observe a stale FIFO | `native_host.cpp` |
| F5 (call return) | Resident bodies test `call_return_checkpoint_due` inline and synchronize their registers only inside the cold `call_return_checkpoint_counted` branch (334,128 sites); the fast path performs no context traffic | `residency.rs`, `native_api.h` |
| THP | `THPSimplePlayerWrapper::videoDecode` hands the JPEG payload to one worker and returns; the planes land when the simulation thread joins the job before every `gx::render_frame` (`join_native_thp_video_decode`) or before the next decode. `GALAXY_NATIVE_THP_ASYNC_DECODE=0` restores the inline decode; `[thp-hle-video-join]` reports decode and wait times | `native_runtime.cpp` |
| Time base | The thread CPU-time sample (GetThreadTimes) is reused while the sampled QPC is less than 1 ms past it; this can only over-report elapsed CPU time, so it never manufactures a suspension candidate | `runtime_time_base_ticks` |
| Idle wake | The idle wait announces itself (`set_idle_waiting`) and re-tests the mask; publishers signal the wake event only while a waiter is announced, so a publication from a thread nobody waits on costs no kernel transition | `runtime_timeline.cpp`, `idle_loop_wait_for_next_deadline` |
| Diagnostics off the hot path | The context poison check on every exception save is opt-in (`GALAXY_TRACE_CONTEXT_POISON`). Disconnected XInput slots are retried every 2 s instead of every report | `save_exception_context`, `poll_native_hid_controller` |
| Profiler | Samples are running or blocked (RIP inside a wait system call), stack-walked through a private copy of every image's unwind table (RtlLookupFunctionEntry deadlocked a boot while the target was suspended), and attributed to the guest caller, the runtime caller of foreign-module time (`[sim-profile-other-caller]`) and the wait site (`[sim-profile-blocked-site]`). `NebulaRuntime.pdb` is built (`/Zi`, `/DEBUG /OPT:REF /OPT:ICF`) and shipped beside the executable so names resolve in the recording itself | `native_runtime.cpp`, `CMakeLists.txt` |
| Live settings | The F1 overlay's video tab changes the internal resolution live (the render thread re-creates the EFB targets, conversion views and scaled copy destinations at the next frame boundary; `EfbCopyManager::set_efb_scale` re-sizes XFB textures on acquisition), cycles output size presets (854x480 to 7680x4320, applied by the window thread) and toggles borderless fullscreen | `renderer_d3d12.cpp`, `efb_copy.cpp`, `gx_backend.cpp` |

Second pass on the same desktop profile after the first build (120 s title
screen, 21,600 running samples): MMIO polling through the fault handler was
4.2 million faults, 91 percent of them the DSP driver spinning on the to-DSP
mailbox until the coprocessor takes a mail; the to-DSP mailbox read now waits
up to 200 us for the worker before answering (faults 3.8 to 2.6 million).
The deadline broker is woken only for a deadline earlier than the one its
worker sleeps toward (SetEvent samples halved). Environment flag reads are
cached by literal pointer once boot configuration is done (strchr samples
halved). The generated module measured 2x slower per call with the inline
assembly memory forms (60.5 against 31.4 ns) because they hid every guest
access from alias analysis; the 16/32/64-bit forms are the movbe intrinsics
again and the fault decoder accepts the resulting mov/movbe/movzx/immediate
forms.

`GALAXY_RENDER_LIVE_MEMORY=1` with `GALAXY_RENDER_MEMORY_SNAPSHOT=0` lets the
render thread read guest RAM directly while the simulation runs ahead, the
hardware's own GP model and Dolphin's dual-core model; it removes the
per-frame dependency scan and the 3 to 6 MB snapshot copy from the simulation
thread (10 to 19 percent of a laptop frame). The independent VI policy
accepts it as the memory-ownership proof. It relies on the game's GXDrawDone
discipline and triple-buffered frame data exactly as the hardware does.

Still open after this round, in order: GuestTransfer unwinding (the C++
throw per interrupt return was about 10 percent of running samples on the
title screen: plan is flag-based unwinding through the call-return
checkpoints instead of fibers), the periodic checkpoint body (two clock reads
and a decrementer sync per 2,048 counted checkpoints), latch lock contention
between the broker and the simulation thread, and the remaining register
faults (IPC, PI, VI pages at 1 to 2 thousand per second each).

## Native10 session 2026-10-10 (evening)

The laptop recording's thread table settled where the time goes: the
simulation thread used 80 percent of one core over nine minutes and the
render thread 20 percent, so the program stays on the simulation thread.
A `[host-census]` line at exit now counts time-base reads (guest and
runtime), checkpoint bodies and CPU-time syscalls; the title screen reads
the time base 250 thousand times per second, 225 thousand of them from the
runtime's own checkpoint body, which runs 33 thousand times per second.

| Item | Change | Where |
| --- | --- | --- |
| Static device lowering | Integer loads and stores whose effective address is a translation-time constant outside the flat RAM aliases (DSP mailbox, IPC, PI, VI, CP; `lis`/`addi`/`ori` chains tracked like the WGPIPE store runs) lower to the checked helpers and never take a host fault. 2,174 loads and 569 stores. Faults per two-minute title screen 3.2 million to 327 thousand; the DSP mailbox poll (2.5 million) is gone. What remains is pointer-based: IPC control 0xCD000004 at 1,250 per second, VI, CP and PE registers, EFB peeks | translator `discover_static_device_access_pcs` |
| Fastmem forms | Plain loads and stores (`GALAXY_FASTMEM_VOLATILE=1` restores the volatile forms; they measured no faster, the apparent 2x in the module benchmark was a stale harness). A census of nine compiled shards found only mov/movbe/movzx/movsx/immediate forms with two-register addressing plus rare `vmovsd`, `vmovd`, `vpinsrd`; the fault decoder covers those, `movsxd`, VEX and legacy SSE moves, pinsr/pextr, and completes a device access wider than the device accepts as ascending 4-byte pieces | `native_api.h`, `flat_guest_memory.cpp` |
| F20-lite | Flag-based interrupt return (generation 17). An RFI that resumes elsewhere sets `kPendingEventTransferBit` in the pending-event mask and returns; every checkpoint fast path then escalates, the cold blocks (`GALAXY_RETURN_IF_TRANSFER_PENDING`, emitted by the residency pass for resident bodies and by `apply_transfer_returns` for the rest, plus after `mtmsr` and `sc`) return without touching the context, every runtime dispatcher and the checkpoint body return early while the transfer is pending, and `run_guest_entry` consumes it where its `GuestTransfer` catch did. The C++ throw and two-phase unwind per interrupt return were about ten percent of the simulation thread's running samples on the title screen. A module without `galaxy_module_transfer_flags`, or `GALAXY_FLAG_TRANSFER=0`, keeps the exception path; `FpuRetryTransfer`, benchmark and quit stay exceptions. Opt-in (GALAXY_FLAG_TRANSFER=1) until validated; state and remaining steps in docs/PERFORMANCE_PLAN_NATIVE11.md P2. | `return_from_interrupt`, `take_pending_flat_transfer`, `native_api.h`, `residency.rs` |
| Live render memory | Shipped default (`GALAXY_RENDER_LIVE_MEMORY=1`, snapshot off): the draw-done and independent-VI policies accept `render_live_memory_reads` as memory ownership, the producer queue gate is the queue depth. Title-screen running share of the simulation thread 25.3 to 23.0 percent with the same module | `render_config.cpp`, `fifo_submission.h`, `gx_backend.cpp` |
| TSC clock | `invariant_tsc_counter_source`: rdtsc divided by 8, calibrated against QPC for 40 ms, 0.64 fixed-point tick scaling; one rdtsc per timeline read instead of a QueryPerformanceCounter call. `GALAXY_TSC_CLOCK=0` keeps QPC. The CPU-time cache threshold follows the timeline's counter frequency | `runtime_timeline.cpp`, `runtime_time_base_ticks` |
| Producer pools | Dirty-range vectors recycled with the FIFO chunks; drain scratch is a member | `gx_backend.cpp` |
| Config | VI poll checkpoints 8192, host pointer poll cache 4 ms | `installer/runtime-env.json` |

Still open after this round: the checkpoint body cadence (33 thousand per
second at the title screen because several pending-but-undeliverable
conditions schedule the next body at the very next checkpoint) and its
seven time-base reads; PE event classification still parses the FIFO on the
simulation thread (about 3 percent); the DSP mailbox spin waits for the
coprocessor worker (about 1 percent); the remaining pointer-based register
faults.

## Native11 session 2026-10-10 (overnight)

Measurement for this session comes from outside the runtime where possible:
`D:\NebulaWork\native\tools\thread_sampler.py` reads every thread's
`GetThreadTimes`, cycle count and `GetThreadDescription` once per second, and
`nrun.ps1` runs a package copy with it attached. PresentMon needs elevation on
this machine and was not usable. Title screen, 9950X3D, Native10, 60 s:
simulation thread 28.4 % of a core (peak 1 s window 47.6 %), render 13.5 %,
DSP worker 5.8 %.

| Item | Change | Evidence |
| --- | --- | --- |
| P0 crash dumps | The dump directory defaulted to the relative `generated\diagnostics`, which one `CreateDirectoryW` cannot create in an installed copy (the laptop crash of run 025441 failed with error 3). It is now resolved at startup: `GALAXY_CRASH_DUMP_DIR` (the launcher points it at the recording folder) or `%LOCALAPPDATA%\Nebula\crashdumps`, created component by component. The default dump keeps stacks, registers, referenced memory, module data, handles and the memory map; `GALAXY_CRASH_DUMP_FULL=1` writes full memory | `GALAXY_CRASH_TEST=1` (a worker thread writes to null 5 s after start): 100.7 MB dump written, `[crash]` lines carry the thread id |
| P0 thread names | Every runtime thread calls `name_current_thread` (SetThreadDescription plus a `[thread] name= id=` line): simulation, render, window, DSP, deadline broker, async disc, THP decode, texture decode, PSO prewarm/compile/disk writer, HID input/output, audio, telemetry, profiler. The launcher's `threads.csv` gains a `name` column | names read back by the external sampler |

Frame pipeline timeline (`GALAXY_FRAME_TIMELINE=1`, `frame_timeline.h`,
digest `tools/perf/timeline_digest.py`): every VI edge, FIFO submission and its
caller, render-thread start, guest-read completion, command-list submission,
GPU start/end (timestamps mapped to QPC through `GetClockCalibration`), fence
observation, PE delivery, token consumption, Present, DXGI frame statistics,
DWM vblank timing, idle-loop waits and the publication that woke each one. The
launcher records it in every recording (`timeline.bin` in the session folder).

What it showed on the title route (9950X3D, 5090, 480p, Native10 behaviour):
the game's `GXDrawDone` handed the whole frame (163 KB of FIFO) to the render
thread and slept until the frame's PE finish, which arrived 5.6 ms later on
average (p90 7.5 ms): 3.7 ms of render-thread parse and record, 0.15 ms of
GPU, and 1.5 ms until the render thread's 1 ms fence poll noticed completion.
The simulation thread was idle for 84 % of that window. On a slower CPU and
iGPU the same chain grows while the game's own work fills the rest of the
16.7 ms, which is the lag at every resolution with every unit partly idle.

| Item | Change | Measured (title route, 9950X3D) |
| --- | --- | --- |
| P1.5 A: PE at consumption | A capture's PE finish/token are delivered when its command list is submitted (parse done, texture decodes joined), and its token resolves there, instead of at the GPU fence. The CPU never reads GPU results through guest memory (EFB copies stay GPU resources, peeks are asynchronous fields). `GALAXY_PE_EVENTS_AT_CONSUMPTION=0` restores fence delivery | draw-done window beyond recording 1.87 to 0.17 ms |
| P1.5 B: streamed FIFO | When the live FIFO passes 32 KB inside a frame (`GALAXY_GX_FIFO_STREAM_KB`), the gather-pipe flush hands the completed bursts to the render thread through the ordered draw-done submission owner, so parse and recording overlap the game's own work as the hardware GP does. Never blocks: a full render queue or an active VI/sync capture or THP decode defers it by 8 KB | draw-done submit to PE finish 5.6 to 1.19 ms mean (p90 7.5 to 2.4 ms); about 5 chunks of 38 KB per frame; simulation thread submit cost 0.16 to 0.28 ms per frame |
| Profiler crash | The 1 kHz sampling profiler (on in Detailed recordings and in `runtime-env.json`) unwound the suspended simulation thread with `RtlVirtualUnwind`; a garbage RSP from one frame faulted the next unwind inside ntdll and killed the game (minidump: `SimulationProfiler::run` -> `RtlVirtualUnwind` -> `RtlpUnwindPrologue` reading address 9; same signature as the laptop crash of run 025441, a worker at 3 % of a core). Each unwind is now SEH-guarded and every RSP must rise strictly below the TEB stack base; `runtime-env.json` defaults it off | 3 x 120 s with the profiler on: no crash, walk-faults=0 |

Idle-loop wake sources (title route, per second): DSP worker 1,975,
decrementer 801, PE/IOS host wakes 85, input 78, IOS 4. The simulation thread
blocks and wakes about 3,000 times per second; two thirds of it is the DSP
coprocessor's mail traffic, which the native JAudio mixer removes.

Gameplay route for headless measurement: `D:\NebulaWork\native\routes\gameplay.tsv`
(built by `routes\gameplay.py` with `tools\make_route.py`) presses A+B at
the title, picks file 1 with the pointer, plays it and runs loops with jumps
and spins around the Comet Observatory from 45 s to 125 s. It needs a save
seeded with its `galaxy.isfs.meta` NTFS streams (`nrun.ps1 -Nand` copies
with robocopy; a save copied without them hangs boot before the first disc
read). `GALAXY_PRESENT_SNAPSHOT_EVERY/_DIR` save every Nth present as PPM so
a run without a display can be checked frame by frame.

| Item | Change | Measured (gameplay route, 9950X3D, OS thread times over 130 s) |
| --- | --- | --- |
| DSP main-RAM DMA | Every native DSP main-RAM transfer was a one-slot rendezvous: the worker published the span, woke the simulation thread and blocked until it copied the bytes. Reads and writes now copy on the worker through the immutable RAM mapping, as the hardware DMA engine reads RAM while the CPU runs; ordering comes from the mailbox protocol and x86 store order. Writes still mark the renderer's dirty pages atomically; a write touching the CPU-tracked audio backing keeps the rendezvous. `GALAXY_DSP_DIRECT_MRAM_READS/WRITES=0` restore it | rendezvous 5.45 M (42,000/s) to 0; simulation thread 37.9 to 29.9 % of a core, DSP worker 21.4 to 9.2 % (with the idle-timer change below in the same build); idle waits 8,467 to 1,063 per second; VI 59.88/s and new-frame cadence unchanged |
| Idle timer | The idle wait re-armed its high-resolution timer every wait; it stays armed while the target edge is unchanged (`GALAXY_IDLE_TIMER_KEEP_ARMED`) | included above |
| Environment reads | The input poll read replay, script, stick and deadzone settings through `getenv_s` (a scan of the whole environment block) on every report. `read_env_string` and the numeric readers are cached per literal name once configuration is done | ucrt `strchr` 301 to 0 profile samples |
| AI DMA stall | A masked AI DMA service more than 8 buffer periods (140 ms) behind ended the game ("AI DMA missed 8+ consecutive hardware buffer deadlines"; seen once in a headless route run with 10 VI edges pending). The missed periods are now dropped and the clock resumes at the newest boundary, logged as `[ai-dma-resync]` | unit test pins the behaviour |

Profiler caveat: the sampler suspends the simulation thread, and a
suspension lands on the return from a kernel call, so rows for syscall-heavy
functions are inflated (GetThreadTimes shows about 3 us per call in the
profile against 144 ns measured directly). Judge those rows by A/B runs.

Correction to plan P5: the runtime already calls `timeBeginPeriod(1)`
(`ScopedTimerResolution`), opts the process and the simulation thread out of
execution-speed and timer-resolution power throttling, raises the class to
ABOVE_NORMAL with the simulation thread at HIGHEST, and registers the
simulation thread with MMCSS Games, all at the top of `wmain` since the first
public snapshot. The laptop's 20 to 105 ms idle-loop gaps happened with all of
that active, so their cause is elsewhere.

## Native12/13 session 2026-10-11

All figures: 9950X3D, gameplay route, OS thread times from the external
sampler unless noted; same package and module on both sides of each A/B.

| Item | Change | Measured |
| --- | --- | --- |
| F20 flag transfer (Native12, default on) | `GALAXY_FLAG_TRANSFER=1` lost the Wii Remote (Nunchuk error screen). Bisected with `GALAXY_FLAG_TRANSFER_KINDS` (1 thread switch, 2 external, 4 decrementer): only external-interrupt flag returns broke it, and DEC dispatches fell from ~25,000 to 40 in 16 s. A DEC was delivered between `[flag-set]` and `[flag-take]`: `arbitrate_external_before_decrementer` ran the VI handler, whose RFI set the pending transfer, returned false, and `dispatch_decrementer` accepted DEC on the abandoned boundary; the alarm queue stopped re-arming DEC, so the 500 Hz BTU alarm never ran again. Arbitration now owns the boundary when a transfer is pending; SelectThread's RFI-return proof, the OSLoadContext RFI return and the yield retention catch return on a pending transfer; a guest RFI while one is pending is a RuntimeFailure | simulation thread 26.2 to 20.6 % of a core; 45 HCI commands in both modes; route, Home menu and 240 s title idle with exception-unwinds=0 |
| EFB fields in place | Every completed pointer-depth field (about 40/s) and GXPeek field was converted into a 1.35 MB array on the render thread so a reader could take one pixel: 24 % of its running samples, stalled on loads from the mapped readback (a pshufb conversion and a pinned mapping did not move it). Pointer fields rotate through four persistently mapped readbacks and GXPeek fields through three per kind; readers convert their pixel | render thread 13.89/14.64 to 12.42/10.11 cpu-s (2 rounds) |
| Hybrid uber pipelines (Native13, `GALAXY_GX_UBER=1`) | Every first-seen material compiled FXC shaders and a PSO on the render thread (30 to 376 ms record spans with a fresh cache: every VI gap above 25 ms on the route). `uber_vs.hlsl`/`uber_ps.hlsl` run the specialized generator's arithmetic with key decisions read at run time; the key rides in constant-buffer fields specialized shaders never read, so the root signature and cached pipelines are unchanged. A miss draws with its render state's uber pipeline while a worker compiles the specialization. Uber pipelines for the 64 RMGE01 render states seen in complete play caches, in every output variant, build in the background once the uber DXBC exists (cached in `uber.bin`). `force` draws everything with uber pipelines | fresh cache: VI gaps above 25 ms 10 to 1, worst record span 376 to 52 ms, synchronous misses 104 to 11 (8 before the uber DXBC was ready); second session: none. Force mode: route renders correctly with 59 pipelines; strap screens pixel-identical to specialized (0 of 409,920 pixels) |
| Profiler | `GALAXY_RENDER_PROFILE=1` samples the render thread; both profilers print per-function totals (`*-host-fn`, `*-other-fn`, `*-other-caller-fn`). The runtime's static libraries now carry private symbols: before, samples in their static functions resolved to the nearest public name (the vertex decoder appeared as an `unordered_map` method) | names verified against disassembly |

Simulation thread after F20 (profile, gameplay route, running samples):
guest code 54 %, runtime 31 %, system 15 %. Largest runtime items: the
sim-thread PE classifier's FIFO parse (about 5 %), time-base reads (about
4 %: two CAS loops on shared atomics per read, about 144,000 reads/s),
MMIO reads and fastmem faults, guest-write drain, checkpoints, DSP mailbox
wakes, deadline-broker wakes on every DEC write. Render thread after the
readback change: system/driver 47 % (about a fifth of it first-use shader
compilation in fresh-cache runs), FIFO parse and display-list replay 17 %,
CPU vertex decode 13 %, draw-state building 12 %.

Measurement notes: present snapshots need an existing directory; nrun
`-ShaderCache` seeds a session with a pipeline cache (a returning player);
gameplay frames are not deterministic across runs, so pixel comparisons
are limited to static screens. Headless runs have no audio endpoint (the
runtime keeps starting audio-recovery threads) and no DXGI frame
statistics; display pacing needs a recording from a real display.

## Native14 session 2026-10-11: host JAudio mixer

`DspBoot` (0x804961E0) and `DSPSendCommands2` (0x804962C0) are host calls
(translator host-call list; `GALAXY_JAUDIO_HOST_MIXER=1`, default on). No
DSP task boots, so the DSP worker thread, mailbox handshakes, MRAM/ARAM
transfers and DSP interrupts are gone. The game's audio thread, JASDriver
and every JASChannel update still run as translated code; the command words
it would mail become calls into `JAudioHostMixer`
(`runtime/src/jaudio_host_mixer.cpp`): 0x81 setup copies DSPRES_FILTER and
the ADPCM table, 0x8E sets the wave base (VARAM), 0x82 starts a frame and
latches FX_BUF, and the release of channel group 3 mixes one subframe and
posts AUDIOMSG_DSP to the JASAudioThread queue as `DSPCallback` does. The
setup and VARAM callbacks run at once.

What a subframe computes was read from the game's DSP program (0x1F00 bytes
at 0x805D8460, disassembled with the translator's decoder) and restated as
host integer code: AFC 4-bit/2-bit and PCM8/16 decode with the program's
record bookkeeping (`_64` leftover samples, `_70` next block, `_74` samples
left; JASAramStream reads `_74 + _64` and `_70` to refill its ring), the
64-phase 4-tap DSPRES_FILTER resampler with the 4-sample tail in `_78`,
FIR (`mode & 0x1F` taps) and the one-sample-late IIR, six bus sends ramped
over 64 samples with per-bus delay from the `_E` history, the auto mixer's
quarter-sine pan/surround gains and effect sends, four FX feedback delay
lines with optional 8-tap FIR, the surround fold-down (0x6784, 0xB820,
0x47E0 matrix), center at 0x5A82, the inverted-right bus, and output =
bus x mixer level / 4096. The oscillator wave tables 11/12 are regenerated
every subframe as the program does. Gain chain: channel volume / 65536 on
the bus, x4 at mixer level 0x4000; the Dusk HLE's /0x2EE0 was the 1.37x
level error measured earlier.

| Measure (9950X3D, gameplay route, same runtime and module) | Native DSP | Host mixer |
| --- | --- | --- |
| Simulation thread, % of a core (3 runs each) | 21.7, 23.6, 22.8 | 19.2, 20.0, 21.6 |
| DSP worker thread | 5.8, 7.0, 6.2 | none |
| Process total (2 rounds; a 1 s uber-compile thread excluded) | 51.1, 47.1 | 37.9, 37.9 |
| Mix time inside the host calls | | 247 ms over 88 s (0.28 % of a core) |
| AI buffer misses (`late-over-buffer`, `missed-buffer-estimate`) | 0 | 0 |

Audio against the native DSP on the same route (`audcmp.py`): level ratio
1.001 (L) / 1.003 (R), mean log-spectrum correlation 0.9999, band
differences at most 0.008 log10 (below 0.1 dB) from 0 to 16 kHz; the Dusk
HLE measured 0.9945, 1.37x louder and +3.4 dB treble. 240 s title idle:
216 s of continuous PCM16 stream music, no gaps, the 27 s title loop
repeating. Home menu opens and closes with gameplay continuing.

Route coverage (counters in the `[jaudio]` shutdown line): AFC, PCM16,
auto mixer and FX lines ran; FIR, IIR, oscillators, PCM8 and the
main-memory PCM formats (0x20/0x21) did not occur on the route or at the
title and are covered by unit tests only (`galaxy_jaudio_mixer_tests`:
gain chain, ramp, AFC bookkeeping and loop, FX delay, pan, FIR delay, IIR,
square oscillator, stray releases). Oscillator type 10's phase modulation
from the surround bus is played as a plain sine. `GALAXY_JAUDIO_HOST_MIXER=0`
restores the native DSP on the same module.

## Native15/16 session 2026-10-11: integrated GPU, display pacing, slow-CPU emulation

`GALAXY_GPU_PREFERENCE=low` selects the minimum-power adapter (the
desktop's AMD integrated GPU, 2 RDNA2 compute units, the HD 520 class) and
`nrun.ps1 -Affinity` limits the process to chosen logical CPUs.

| Item | Change | Measured |
| --- | --- | --- |
| GPU page fault with uber pipelines (Native15) | Hybrid mode removed the AMD iGPU device (DEVICE_HUNG) about 40 s into the route; force mode every run. DRED: page fault at a fixed VA 0x13f800000, no live or recently freed allocation, before op 0 of a new command list. A draw whose specialized shader samples no map left texture and sampler root tables unbound (the renderer skips null tables), so after a command-list reset they were undefined; the uber pixel shader samples every enabled map even when the combiner ignores it. Such draws now get a frame-cached table of null SRVs and the default samplers. Refuted on the way: an unbound matrix palette (always binding it changed nothing) | force mode 3 of 3 runs removed before, 0 after (4.86 M uber draws in 124 s); hybrid 480p: 59.96 new frames/s, no VI interval above 25 ms |
| VSYNC ON intervals | VSYNC ON presents with interval round(refresh / 60) when the output runs at a multiple of 60 Hz within 1.5 % (120, 180, 240; 119/239 qualify), so each frame stays the same number of refreshes; other rates keep 1. VSYNC OFF (default) unchanged | not measurable headless (no output; DWM reports a virtual 60 Hz) |
| Display stats | `[present-stats]` adds `refresh-hz` (EnumDisplaySettings of the swap chain's output) and `shown-refreshes(0/1/2/3/4+)` (how many refreshes each displayed frame stayed on screen, from DXGI frame statistics) | headless: refresh-hz=0, no statistics |
| Clock reads | `now_ticks` keeps one shared CAS (the raw-counter CAS was redundant with the output clamp); the checkpoint body's sample decides "DEC not yet due"; AI-DMA dispatch reuses its fresh first sample | reads by site before: DEC fallback 37 %, AI dispatch 23 %, VI/DEC arbitration 13 %, VI waits 9 %, guest mftb 11 % of 177,000/s; one A/B pair 14.1 M to 11.0 M reads; CPU change within noise |
| Census | `[host-census]` adds broker replacements and wakes | 3,800 and 1,630 per second; a waking SetEvent costs 1.34 us on this host, GetThreadTimes 162 ns, QueryThreadCycleTime 201 ns (the profiler's syscall rows overstate these) |

Slow-processor emulation on the gameplay route (AMD iGPU, 480p, frame
timeline): `GALAXY_DIAG_SIM_BURN_US` adds busy time to the simulation
thread at each VI delivery, `GALAXY_DIAG_RENDER_BURN_US` to each render
chunk.

| CPUs | Added burn | Simulation thread | Render thread | New frames/s | Frames repeated |
| --- | --- | --- | --- | --- | --- |
| 4 logical | none | 22.3 % | 7.6 % | 59.958 | 0 |
| 2 logical (one core) | none | 25.5 % | 10.6 % | 59.961 | 0 |
| 4 logical | 6 ms/VI, 1.5 ms/chunk | 53.3 % (8.9 ms/frame) | 37.7 % | 60.000 | 0 |
| 4 logical | 10 ms/VI, 2.5 ms/chunk | 78.6 % (13.1 ms/frame) | 56.0 % (9.3 ms/frame) | 59.861 | 8 in 85 s |

Simulation and render work overlap: frames are lost only as a thread's
own work approaches the 16.7 ms frame, not when their sum exceeds it.

Tried and not adopted (A/B within run-to-run noise of about 2 points of
simulation-thread CPU):
- `GALAXY_PE_PRECLASSIFY=0` (skip the simulation thread's second FIFO
  parse): simulation -0.75, render +1.9 points. The knob stays as a
  diagnostic.
- Turning adaptive call-return checkpoint counting off: checkpoint bodies
  -23 %, no CPU change; reverted.
- A module built without call-return checkpoints is 247 MB instead of
  352 MB, but it cannot run: the same inline test carries the pending
  flag-transfer check (boot failed with an RFI during a pending
  transfer). Generation 18 has to keep a one-load transfer test after
  calls.

## Known simulation-thread stalls (from the 9950X3D A/B telemetry)

- (Resolved, F9 pointer) Keyboard/mouse pointer mode waited for the current
  frame's owned pointer depth field before the pointer transaction
  (`0x80385034`) completed; one 82 ms checkpoint gap during a load was this
  wait. The transaction now reads the previous scene's field.
- (Resolved, F20) Thread switches and interrupt returns unwound the native
  stack through `GuestTransfer`; generation-17 modules now return by flag.

## Not yet shipped

| Item | State |
| --- | --- |
| F5 (float) | FPR/PS1 residency is implemented (`guest_resident_float`, `_r` helper overloads on `fpr[32]`/`ps1[32]` locals) and off: under the exact model it measured 86.8 (store-all) and 87.9 ns (written-only) against 83.2 without it, because the exact fallback calls force spills. To be re-measured on top of fast float, where the float helpers make no calls |
| F6 (checkpoints) | Branch and call-return checkpoints remain: counter, watermark and event-mask loads through the services table at every backward branch and call return |
| F8 | Superseded: the DSP no longer runs by default (host JAudio mixer, Native14) |
| F10 | PE tokens at FIFO consumption instead of GPU fence |
| F12, F14 | Compact per-layout vertex stride; non-blocking pipeline misses (needs a real uber pipeline). The remaining texture spikes are resource creation, not decode; placed heaps or recycled resources are the fix |
| F16 | Working-set investigation (2.8 to 5.4 GB observed) |
| F17, F18 | Present phase lock to vblank; compile-time removal of remaining runtime trace gates |
| F20 | Flag returns shipped (Native12). Guest threads as fibers remain the next step of the SDK replacement plan |
| F23 | Native SIMD Yaz0 |

## Verification

`cargo test --workspace` for the translator (426 tests; the temp-directory
tests need a writable `%TEMP%`); the runtime test targets (60, including
`galaxy_native_fast_float_tests` built with `GALAXY_FAST_FLOAT=1`) compile
against ABI 26 (the fastmem forms are clang-only and inert under MSVC); a smoke run of the intro route on the build host boots,
publishes the direct dispatch table, and answers depth peeks from the field.
Differential register tracing against Dolphin remains the gate for any change
to the admission rules above.
