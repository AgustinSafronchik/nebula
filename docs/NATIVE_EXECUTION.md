# Native execution model

Status of the performance program recorded on 2026-10-09. Items are numbered
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

## Admission rules of the native float path

Inputs finite and zero-or-normal, FPSCR rounding mode nearest-even. Results
that are non-finite, subnormal, or (single precision) exactly on a binary32
rounding midpoint take the exact helper, as does every input the rules reject.
Gekko's 25-bit multiplier rounding is applied to every single-precision
multiply. The native path does not maintain FPSCR status bits (FPRF, FI, FR,
sticky exceptions); the exact path and every status consumer still do.

## Direct dispatch rules

The runtime publishes one entry per callable address. Entries stay null for
every target in the translator's host-call list, the 0x8041xxxx range, every
address the cached-call service compares against, and any target the live
intercept predicate rejects at boot. Generated code also defers to the service
while a device event is pending or an external interrupt dispatch is live.
Call-tracing and profiling diagnostics disable the table.

## Known simulation-thread stalls (from the 9950X3D A/B telemetry)

- Keyboard/mouse pointer mode waits for the current frame's owned pointer depth
  field before the pointer transaction (`0x80385034`) completes; one 82 ms
  checkpoint gap during a load was this wait. The asynchronous peek fields
  (one frame of staleness) are the model to move it to.
- Thread switches unwind the native stack through `GuestTransfer` and resume in
  flat continuation mode (about 31 per second in gameplay); F20 replaces this.

## Not yet shipped

| Item | State |
| --- | --- |
| F5 (float) | FPR/PS1 residency: the native float helpers still read and write `context->fpr_bits`/`ps1_bits` per operation (operand admission, narrowing and commit), which is what keeps the scalar matrix routines at 230-640 ns. Needs array-local overloads of the `fast::scalar_*`/`paired_*`/`psq_*`/`*_fpr_single` helpers with the exact path syncing through `context` |
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

`cargo test --workspace` for the translator; the runtime test targets compile
against ABI 24; a smoke run of the intro route on the build host boots,
publishes the direct dispatch table, and answers depth peeks from the field.
Differential register tracing against Dolphin remains the gate for any change
to the admission rules above.
