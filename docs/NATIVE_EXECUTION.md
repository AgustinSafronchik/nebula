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
| F9 | GXPeekZ answered from an asynchronous 640x528 depth field; no FIFO drain or GPU round trip after the first peek | `gx_backend.cpp`, `renderer_d3d12.cpp`, `gx_efb_peek` |
| F15 | Simulation and render threads restricted to performance cores on hybrid CPUs; simulation thread at HIGHEST | `windows_power.h`, `configure_runtime_scheduling` |
| F19 | `bctrl`/`bclrl` dispatch through a direct native entry table published by the runtime | `native_fast_paths.h`, `build_direct_dispatch_table` |
| F21 | Gather-pipe stores append to the host's live 32-byte burst buffer in place | `GuestMemoryV1::wgpipe_gather_fast` |
| F22 (part) | Flip-discard swap chain by default | `runtime-env.json`, `render_config.h` |
| F24 | Idle-loop sleep: with an empty guest run queue the simulation thread waits on the high-resolution timer toward the next VI deadline (AI-capped, 2 ms slices, 250 us guard) instead of spinning a core | `idle_loop_wait_for_next_deadline` |

Measured on the build host (9950X3D) at a steady 60 fps on the title route:
the simulation thread fell from 95 to 46 percent of a core and process CPU from
1.29 to 0.81 cores (F24); 8,688 of 8,690 depth peeks were answered from the
asynchronous field (F9); the direct dispatch table published 326,540 entries
and withheld 3,331 (F19).

Native ABI is 24; generation version is 12. Every module must be regenerated.

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

## Not yet shipped

| Item | State |
| --- | --- |
| F4 | Flush-based dirty tracking: per-store page publication remains (inline, one atomic OR per new page) |
| F5 | Register residency beyond straight-line leaves: compiler-driven now that helper calls are inline; the text pass stays off |
| F6 | Checkpoint diet: branch and call-return counters remain; `ensure_fpu_available` remains one MSR test per region |
| F8 | DSP module codegen at the same standard |
| F10 | PE tokens at FIFO consumption instead of GPU fence |
| F11, F12, F13, F14 | Display-list batch cache, compact vertices, worker texture decode, non-blocking pipeline misses (needs a real uber pipeline) |
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
