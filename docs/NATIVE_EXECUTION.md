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

Native ABI is 25; generation version is 13. Every module must be regenerated.

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
- Still open: THP video decode 4 to 6 ms per 640x368 frame on the simulation
  thread (scalar Huffman reader); texture resource creation 24 to 36 ms per
  first-use texture (`CreateCommittedResource`); texture-flush descriptor
  work 20 to 43 ms; one 31.7 ms EFB texture copy.

## Known simulation-thread stalls (from the 9950X3D A/B telemetry)

- (Resolved, F9 pointer) Keyboard/mouse pointer mode waited for the current
  frame's owned pointer depth field before the pointer transaction
  (`0x80385034`) completed; one 82 ms checkpoint gap during a load was this
  wait. The transaction now reads the previous scene's field.
- Thread switches unwind the native stack through `GuestTransfer` and resume in
  flat continuation mode (about 31 per second in gameplay); F20 replaces this.

## Not yet shipped

| Item | State |
| --- | --- |
| F5 (float) | FPR/PS1 residency is implemented (`guest_resident_float`, `_r` helper overloads on `fpr[32]`/`ps1[32]` locals) and off: under the exact model it measured 86.8 (store-all) and 87.9 ns (written-only) against 83.2 without it, because the exact fallback calls force spills. To be re-measured on top of fast float, where the float helpers make no calls |
| F6 (checkpoints) | Branch and call-return checkpoints remain: counter, watermark and event-mask loads through the services table at every backward branch and call return |
| F3b | Loads and stores still carry the alias-table range check with a cold checked-path call. Because `context` is captured, LLVM reloads context fields after every such merge; this, not the check itself, is what keeps guest registers out of host registers. Unconditional flat access with a fault handler for MMIO removes the slow paths |
| F8 | DSP module codegen at the same standard |
| F10 | PE tokens at FIFO consumption instead of GPU fence |
| F12, F14 | Compact per-layout vertex stride; non-blocking pipeline misses (needs a real uber pipeline). The remaining texture spikes are resource creation, not decode; placed heaps or recycled resources are the fix |
| F16 | Working-set investigation (2.8 to 5.4 GB observed) |
| F17, F18 | Present phase lock to vblank; compile-time removal of remaining runtime trace gates |
| F20 | Guest threads as fibers instead of exception unwinding |
| F23 | Native SIMD Yaz0 |

## Verification

`cargo test --workspace` for the translator (424 tests; the temp-directory
tests need a writable `%TEMP%`); the runtime test targets (60, including
`galaxy_native_fast_float_tests` built with `GALAXY_FAST_FLOAT=1`) compile
against ABI 25; a smoke run of the intro route on the build host boots,
publishes the direct dispatch table, and answers depth peeks from the field.
Differential register tracing against Dolphin remains the gate for any change
to the admission rules above.
