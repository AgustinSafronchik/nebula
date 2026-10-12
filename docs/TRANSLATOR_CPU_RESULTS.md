# Translator CPU work: results

Companion to `docs/TRANSLATOR_CPU_TASK.md`. One section per feature: design,
flag, emitted code before and after for one representative sequence, risks,
and the exact validation the Windows side runs. Everything here was built and
unit-tested on Linux without the game. No module was generated, compiled
against `main.dol` or run.

## Baseline for every A/B

The release module is what `nebula-recomp generate` builds
(`release_module_options` in `crates/nebula-recomp-cli/src/generate.rs`). The
same options on `translate-module`:

```
nebula-recomp translate-module <main.dol or game folder> --output <dir> \
  --count 0 --shard-size 64 --shard-source-kib 512 \
  --experimental-inline-scalar-single-binary \
  --experimental-flat-ram-reads --experimental-flat-ram-writes \
  --experimental-native-float --fast-float \
  --experimental-direct-indirect-calls \
  --experimental-guest-resident-integer \
  --no-exact-psmtx-local-lanes
```

Call this `$RELEASE`. Each feature below is A/B-tested as `$RELEASE` against
`$RELEASE <flag>`. The flags compose, so the final combined run is `$RELEASE`
plus every flag. Every new flag is off by default, and with all of them off
the generated source is byte-identical to the base commit's output (pinned
by the flag-off tests).

Instruction counts in this document come from clang 18 on x86-64 Linux
(`-O2 -mavx2 -mfma -mmovbe -fwrapv -fno-strict-aliasing`, `GALAXY_FASTMEM=1`,
`GALAXY_FAST_FLOAT=1`). The module itself is built with clang-cl. The counts
are the fall-through path from entry to `ret`, cold blocks excluded.

---

## 1.1 Hardware float conversions (`--hw-float-conversions`)

### Design

`lfs`/`lfsu`/`lfsx`/`lfsux`, `stfs`/`stfsu`/`stfsx`/`stfsux` and the binary32
(type 0) lanes of `psq_l`/`psq_st` (all four forms each) now convert with the
host's `cvtss2sd`/`cvtsd2ss`. They used to go through the integer helpers:

- `widen_f32_bits`: shift, add the bias, plus a range test and an out-of-line
  path for zeros, subnormals, infinities and NaNs.
- `native_stored_single_bits`: a two-armed bit slice with a variable shift.
- `narrow_paired_single_ftz`.

New header helpers in `runtime/include/galaxy/native_fast_paths.h`:

| Helper | Replaces |
| --- | --- |
| `fast::load_fpr_single_hw` | `fast::load_fpr_single` |
| `fast::store_fpr_single_hw` | `fast::store_fpr_single` |
| `fast::psq_load_hw` | `fast::psq_load` |
| `fast::psq_store_hw` | `fast::psq_store` |
| `fast::*_hw_r` | the register-resident `_r` overloads (float residency) |

`psq_load`/`psq_store` and their `_r` forms became thin wrappers over
`psq_*_impl<Hw>`. With `Hw = false` the code is the same as before: the
template only picks the lane conversion. Quantized lanes (types 4 to 7), the
gather-pipe test, the HID2 admission and the exact fallbacks are shared and
unchanged.

The ps1 fill rule is kept: `lfs` writes ps1 without testing HID2 in a
fast-float module, and with HID2[PSE] set otherwise. RMGE01 sets PSE in
`__init_hardware` and never clears it.

The translator change is just the helper names, so the residency pass treats
the `_hw` helpers like their exact forms:

- They are not GPR synchronization points (`fpr_only_helper`).
- Under float residency they rewrite to `_hw_r` (`rewrite_float_helpers`).

The flag needs `--experimental-flat-ram-reads` and
`--experimental-flat-ram-writes`, because the conversions live in the
flat-view helpers. `validate_module_options` rejects it otherwise. The checked
helpers, used when there is no flat view, keep the exact conversions.

### Emitted code (lfs f1,8(r3); stfs f1,12(r3))

Before:
```cpp
    galaxy::fast::load_fpr_single(flat_guest_read_base, context, 1u, memory, context->gpr[3] + 0x00000008u, services, 0x80004000u);
    galaxy::fast::store_fpr_single(flat_guest_base, context, 1u, memory, context->gpr[3] + 0x0000000Cu, services, 0x80004004u);
```
After:
```cpp
    galaxy::fast::load_fpr_single_hw(flat_guest_read_base, context, 1u, memory, context->gpr[3] + 0x00000008u, services, 0x80004000u);
    galaxy::fast::store_fpr_single_hw(flat_guest_base, context, 1u, memory, context->gpr[3] + 0x0000000Cu, services, 0x80004004u);
```

x86 hot path of `lfs f1,8(r3); lfs f2,16(r3); fmuls f3,f1,f2; stfs f3,12(r3)`
(the body of a translated function, prologue included): **84 to 45
instructions**. Each `lfs` is now `movbe` + `vmovd` + `vcvtss2sd` + two
`vmovsd` stores (fpr, ps1). The `stfs` is the `vcvtsd2ss` that `fmuls`
already produced plus one `movbe` store, behind the gather-pipe test. Before,
each `lfs` was 15 instructions with two branches, and the `stfs` slice was
about 20 with a variable shift.

`psq_l f3,0(r5); ps_add f3,f3,f3; psq_st f3,8(r5)`: **117 to 78
instructions**.

### Risks

- **Not bit-exact, by design.** Three differences:
  - A signalling NaN is quieted on the way through.
  - `stfs` of a binary64 value outside binary32 rounds to nearest (and
    overflows to infinity) where the Gekko truncates the significand. This
    affects only double-precision results stored as single. Fast-float single
    results are already binary32, so they convert exactly.
  - With the fast-float MXCSR (FTZ+DAZ on the simulation thread), a binary32
    subnormal loads and stores as a signed zero. The exact `lfs` widens it
    exactly.

  All three are within the task's correctness bar.
- **Data that is not a float, moved through FPRs.** CodeWarrior uses
  `lfd`/`stfd` for 8-byte copies, and those are untouched. An `lfs`/`stfs`
  pair moving a non-float 32-bit word would flush an integer below 2^23 to
  zero (that bit pattern is a binary32 subnormal). CodeWarrior emits
  `lfs`/`stfs` only for `float`-typed values, and `psq_l`/`psq_st` copies
  already flush subnormals on hardware (`narrow_paired_single_ftz`).
  `diff_sdkmath` and `diff_tvec3` exercise the paired copies; a frame dump
  would show any texture or vertex data damaged this way.
- The compiler may fold `cvtss2sd`+`cvtsd2ss` around a plain copy into a
  32-bit move. That is a NaN-quieting difference in the safe direction: bits
  preserved.

### Runtime test

`galaxy_native_hw_float_conversion_tests`
(`runtime/src/native_hw_float_conversion_tests.cpp`, `ctest -R
native_hw_float_conversion`) checks:

- Ordinary values, zeros, the largest finite value, the smallest normal,
  infinities and a quiet NaN round-trip the same through both conversions.
- `stfs` rounds where the exact form slices.
- Both paired lanes and the one-element form; quantized lanes are unchanged.
- Every `_hw_r` overload.

It passes here with clang in three configurations: fast float, fast float
plus fastmem, and the exact model.

### Validation (Windows)

1. `$RELEASE --hw-float-conversions`. The module builds with clang-cl
   `/W4 /WX`. `grep -c "load_fpr_single_hw(" functions_*.cpp` is non-zero
   and `grep -c "fast::load_fpr_single(" functions_*.cpp` is 0.
2. `ctest -R native_hw_float_conversion` passes.
3. Differential harnesses against the `$RELEASE` module: `diff_sdkmath`,
   `diff_tier2`, `diff_tvec3`, `diff_strings`, `diff_gx` report 0 mismatches.
   `diff_gx` covers the psq-copied gather-pipe words.
4. Routes: gameplay, Home menu open/close, boot, save, prologue movie. Frame
   dumps on the static screens match `$RELEASE`, and flag-return counts are
   unchanged.
5. A/B with `ab-affinity` on the 32 MB-L3 CCD (`tools/ab-window.ps1` steady
   scene), 3 rounds paired: simulation-thread share, module size, `.text`
   size.

---

## 1.2 Checkpoint globals (`--checkpoint-globals`)

### Design

Every call-return checkpoint, and every budgeted backward-branch checkpoint
once its local countdown expires, decided from the service table. After an
opaque call the compiler has to re-read all of it:

- `services->branch_checkpoint`;
- `pending_event_mask`;
- `runtime_flags`, for the trace and adaptive bits;
- `checkpoint_counter` and `checkpoint_next_slow`, each null-tested;
- then the counter, the watermark and the event mask behind those pointers.

The service-table fields cannot simply be copied at load. The runtime's
`checkpoint::GateController` republishes the two gate pointers at every
host-callback scope entry and exit and at every fiber switch. The checkpoint
callback also toggles the adaptive call-return bit at run time.

With the flag, the module owns a snapshot of the gate decision,
`galaxy::g_module_checkpoint_gate` (`ModuleCheckpointGateV1`). It has three
pointer words and one flag:

- `branch_counter` is the counter while the inline gate is published, the
  table is complete and tracing is off.
- `call_return_counter` is the same pointer, but only while adaptive
  call-return checkpoints are on.
- `next_slow` and `pending_event_mask` point at the watermark and the event
  mask.
- `call_return_quiet` allows a non-adaptive call return to finish on an
  empty mask.

`refresh_module_checkpoint_gate` derives the snapshot from the service table,
and the runtime calls it whenever an input changes ("Runtime changes" in the
task document lists the four call sites). The snapshot reproduces each
existing decision exactly:

| Test | Snapshot helper | Replaces |
| --- | --- | --- |
| Backward branch | `branch_checkpoint_due_g` | `branch_checkpoint_due` |
| Budgeted backward branch | `branch_checkpoint_due_budgeted_g` | `branch_checkpoint_due_budgeted` |
| Call return | `call_return_checkpoint_due_g` | `call_return_checkpoint_due` |
| Interrupt transfer | `transfer_pending_g` | `transfer_pending` |

`galaxy_native_module_checkpoint_gate_tests` checks this over 576 scenarios:
the same counted value, the same counter advance and the same transfer
answer in every service-table state.

The module side:

- **Generated text.** A textual post-pass over the final sources
  (`apply_checkpoint_globals`) switches every checkpoint spelling to the
  snapshot form: the resident inline tests, the non-resident
  `GALAXY_*_CHECKPOINT*` macros, and the transfer tests in the cold blocks
  and after `mtmsr`/`sc`/`mtdec`. The cold-path policy calls are unchanged
  and still take `services`.
- **Macro.** `functions.h` defines `GALAXY_MODULE_CHECKPOINT_GLOBALS 1`
  before it includes the runtime header.
- **Definition and export.** `module.cpp` defines the snapshot and exports
  `galaxy_module_checkpoint_gate()`.
- **Safe without runtime support.** An empty snapshot (the initial state, and
  what a runtime without step 1 leaves) makes every test report
  `kBranchCheckpointNotCounted`. Every checkpoint then runs the complete
  service-table policy, which is correct and only slower. A partly
  integrated runtime can therefore never make a checkpoint decide
  differently, only slower.

### Emitted code (one call and its return checkpoint)

Non-resident body, before:
```cpp
    rmge01::fn_80004014(context, memory, services);
    GALAXY_CALL_RETURN_CHECKPOINT(services, 0x80004004u, 0x80004008u, context, memory);
```
After:
```cpp
    rmge01::fn_80004014(context, memory, services);
    GALAXY_CALL_RETURN_CHECKPOINT_G(services, 0x80004004u, 0x80004008u, context, memory);
```
Resident body, before and after:
```cpp
    if (const std::uint64_t counted__ = galaxy::call_return_checkpoint_due(services); counted__ != 0u) [[unlikely]] { shared_counted__ = counted__; shared_site__ = 0u; goto shared_call_return__; }
    if (const std::uint64_t counted__ = galaxy::call_return_checkpoint_due_g(services); counted__ != 0u) [[unlikely]] { shared_counted__ = counted__; shared_site__ = 0u; goto shared_call_return__; }
```

x86 per call-return site, read from the compiled resident form:

| Path | Before | After |
| --- | --- | --- |
| Adaptive (gameplay, AI DMA playing) | 26 instructions: 10 loads, 9 branches | 13 instructions: 6 loads (3 RIP-relative), 4 branches |
| Non-adaptive | 14 instructions | 9 instructions |

The non-resident macro form goes from 42 to 20 instructions per site
(two-call probe: 83 to 41). Its transfer test now runs only on the cold
path; a fast-path pass implies an empty event mask.

### Risks

- **Runtime integration.** A missed refresh site leaves a stale snapshot. A
  stale published counter keeps counting inline inside a full scope (a
  cadence change); a stale empty snapshot only costs speed. The four sites
  are exhaustive for the current runtime: every write to
  `checkpoint_counter`/`checkpoint_next_slow` goes through `GateController`,
  and `runtime_flags` changes after init only at the adaptive toggle. A new
  writer added later must refresh too. This is written in the header next to
  the struct.
- **Generated-code assumption.** The snapshot is re-read after every call.
  The compiler cannot keep it in a register across an opaque call, because
  the global escapes through the exported getter. Every refresh happens
  inside such a call (host callbacks, fiber switches in SelectThread), so
  translated code never sees a half-updated snapshot.
- **Compatibility.** Module ABI 27 is unchanged. The export is optional: an
  older runtime ignores it, and an older module has nothing to refresh.

### Validation (Windows)

1. Runtime first: apply the four refresh sites listed under "Runtime
   changes" in the task document. `ctest -R
   "native_module_checkpoint_gate|native_checkpoint_policy|checkpoint_gate"`
   passes.
2. `$RELEASE --checkpoint-globals`. The module builds with clang-cl. Grep
   checks:
   - `grep -c "call_return_checkpoint_due(services)" functions_*.cpp` is 0.
   - `grep -c "GALAXY_CALL_RETURN_CHECKPOINT(services" functions_*.cpp` is 0.
   - `dumpbin /exports RMGE01_game.dll` lists
     `galaxy_module_checkpoint_gate`.
3. Equivalence of the cadence: run the gameplay route with the
   `[host-census]` line on, `$RELEASE` against `$RELEASE
   --checkpoint-globals`, on the same runtime. Checkpoint bodies per run and
   time-base reads must match within the route's run-to-run spread.
   Flag-return counts are unchanged.
4. Run the same module on a runtime without the refresh sites (the
   fail-safe): it must boot and play the route with the same checkpoint body
   counts, only a higher simulation-thread share.
5. Routes: gameplay, Home menu, boot, save, prologue movie. A/B on the 32 MB
   CCD steady scene, 3 rounds paired.
