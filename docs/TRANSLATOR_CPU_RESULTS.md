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

Instruction counts in this document were measured on x86-64 Linux, not
estimated:

- The emitted C++ for a representative sequence is compiled with clang 18
  using the module's flags (`-O2 -mavx2 -mfma -mmovbe -fwrapv
  -fno-strict-aliasing`, `GALAXY_FASTMEM=1`, `GALAXY_FAST_FLOAT=1`) and
  linked into a driver.
- The driver runs each body 1,000 times with realistic state: a mapped flat
  view, normal float data, a published inline gate with adaptive call
  returns, and an empty event mask.
- `valgrind --tool=callgrind` counts the instructions each body retires,
  divided by the 1,000 runs.

The module itself is built with clang-cl, so expect small differences.

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

Retired x86 instructions for `lfs f1,8(r3); lfs f2,16(r3); fmuls f3,f1,f2;
stfs f3,12(r3)` (the body of a translated function, prologue included):
**79 to 45**. Each `lfs` is now `movbe` + `vmovd` + `vcvtss2sd` + two
`vmovsd` stores (fpr, ps1). The `stfs` is the `vcvtsd2ss` that `fmuls`
already produced plus one `movbe` store, behind the gather-pipe test. Before,
each `lfs` was 15 instructions with two branches, and the `stfs` slice was
about 20 with a variable shift.

`psq_l f3,0(r5); ps_add f3,f3,f3; psq_st f3,8(r5)`: **114 to 78**.

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

Retired x86 instructions for a body of two direct calls, each followed by
its call-return checkpoint (prologue, calls and epilogue included):
non-resident **69 to 41**, resident **74 to 41**. Per call-return site,
read from the same assembly:

| Path | Before | After |
| --- | --- | --- |
| Adaptive (gameplay, AI DMA playing) | 26 instructions: 10 loads, 9 branches | 13 instructions: 6 loads (3 RIP-relative), 4 branches |
| Non-adaptive | 14 instructions | 9 instructions |

The non-resident macro form compiles to the same two sequences. Its
transfer test now runs only on the cold path, because a fast-path pass
implies an empty event mask.

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

---

## 1.3 Flat-view-only fastmem modules (`--flat-view-only`)

### Design

Generation 25's `fast::flat_base` lets the compiler assume the flat view is
non-null in a fastmem module, which removed the duplicate null-view block
around every translated load and store. Three kinds of sites never went
through `flat_base` and still carried a null test plus a region-table
fallback (`resolve_guest`), or always took the checked path:

| Site | Before | With the flag |
| --- | --- | --- |
| Lean host bodies, force-inlined at their call sites (TVec3, JMath copies, SDK math, GD writers, strings) | `lean_guest_span`: null and struct-size test, then `resolve_guest`. `cstring::view`: null test, then the checked byte loads. Three of these per inlined TVec3 operation | `assumed_flat_view(memory) + address`, with `__builtin_assume(base != nullptr)` as in `flat_base` |
| `stmw rS,d(rA)` with rS <= 24 | `guest_store_multiple_gprs<rS>` on every build: trace gates, a host-overlap test, `resolve_guest_fast`, a per-word fallback, and a call that the residency pass treats as a full sync point | One flat store per register (`store_stack` from r1), as rS > 24 already did |
| `dcbz` | `guest_zero`: `resolve_guest`, then a byte loop | `fast::zero_line`: one alias-table test, then a 32-byte `memset` through the view. A line outside RAM (the locked cache) still goes to `guest_zero` |

`dcbz_l` (locked cache), `lwbrx`/`stwbrx` (rare) and the checked device
sites are unchanged. The flag defines `GALAXY_FLAT_VIEW_ONLY 1` in
`functions.h`. It needs `--fast-float`, which makes the module fastmem, and
both flat RAM directions.

### Emitted code (stmw r22,8(r3); dcbz r3,r4)

Before:
```cpp
    const std::uint32_t ea_80004000 = context->gpr[3] + 0x00000008u;
    galaxy::guest_store_multiple_gprs<22u>(context, memory, ea_80004000, services, 0x80004000u);
    ...
    galaxy::guest_zero(memory, ea_80004008 & 0xFFFFFFE0u, 32u, services, 0x80004008u);
```
After:
```cpp
    galaxy::fast::store<std::uint32_t>(flat_guest_base, memory, ea_80004000 + 0x00000000u, context->gpr[22], services, 0x80004000u);
    ... one per register to r31 ...
    galaxy::fast::zero_line(flat_guest_base, memory, ea_80004008, services, 0x80004008u);
```

Retired instructions per execution:

| Sequence | Before | After |
| --- | --- | --- |
| `stmw r22` to the stack, including the checked helper | 75 | 33 |
| `dcbz` | 28 | 17 |
| Inlined `TVec3::operator+=` (`lean_tvec3_add_assign`) | 51 | 20 |

In resident bodies, `stmw` is also no longer a full synchronization point.

### Risks

- The view is assumed in the inlined host bodies exactly as translated code
  already assumes it. The runtime refuses to start a fastmem module without
  the view (`kModuleMemoryFlagRequiresFlatView`). An access outside RAM
  faults into the vectored handler, as a translated load or store would.
- `stmw` loses its trace gates (`trace_fileloader_temp_writes`,
  `trace_u32_store`). These are diagnostics, and the flat `stw` path never
  had them.
- `dcbz` through the view does not trace audio-control zeroing
  (`trace_audio_control_writes`), a diagnostic.
- Per-register `stmw` adds inline stores. clang merges consecutive
  byte-swapped stores, so the measured `stmw r22` path is 33 instructions
  with no call.

### Validation (Windows)

1. `ctest -R native_flat_view_only`. Under a clang-cl runtime build the
   flat-view-only arms are active, and the test prints "active".
2. `$RELEASE --flat-view-only`. The module builds with clang-cl. Grep checks:
   - `grep -c "guest_store_multiple_gprs" functions_*.cpp` is 0.
   - `grep -c "galaxy::guest_zero(memory, ea" functions_*.cpp` is 0.
   - `grep -c GALAXY_FLAT_VIEW_ONLY functions.h` is 1.
3. `diff_tvec3`, `diff_strings`, `diff_sdkmath`, `diff_tier2`, `diff_gx`:
   0 mismatches. The lean bodies run their flat-view-only arms in the module
   under test.
4. Routes (gameplay, Home menu, boot, save, prologue movie) and the
   `[fastmem]` fault counts at exit. These must not grow: no new site faults,
   because every switched site addresses RAM.
5. A/B on the 32 MB CCD steady scene (3 rounds paired), plus `.text` size.

---

## 1.4 Stack stores through r11 (`--stack-r11-stores`)

### Design

Gap item 1 says to skip the gather-pipe test for r1- and r11-relative
stores. Generation 25 did r1 (`fast::store_stack`). CodeWarrior's register
save helper is called as:

```
addi r11, r1, d
bl   _savegpr_N
```

The helper stores rN..r31 at `r11 - 0x80 + 4N`, and the inline `_savegpr`
expansion (11,719 call sites) emitted those stores with `fast::store`, which
includes the gather-pipe test. With the flag, a save run uses
`fast::store_stack` when r11 is proven to point at the current frame. The
proof: the instruction before the `bl` is `addi r11,r1,d`, and the `bl` is
neither a branch target nor an interior entry, so control can only arrive
from that `addi`. Otherwise the stores keep the test. Restores are loads,
which never had the test. Requires `--experimental-flat-ram-writes`.

### Emitted code (addi r11,r1,0x60; bl _savegpr_25)

Before:
```cpp
    galaxy::fast::store<std::uint32_t>(flat_guest_base, memory, context->gpr[11] + 0xFFFFFFE4u, context->gpr[25], services, 0x80004004u);
```
After:
```cpp
    galaxy::fast::store_stack<std::uint32_t>(flat_guest_base, memory, context->gpr[11] + 0xFFFFFFE4u, context->gpr[25], services, 0x80004004u);
```
The seven stores of `_savegpr_25`: **75 to 26** retired instructions. Each
store was a masked compare and branch plus the store; it is now one `movbe`.

### Risks

None for guest state. `store_stack` writes the same bytes, and an address
that is not RAM still faults into the device path. Only the in-place
gather-pipe append is skipped, and a stack frame can never be the pipe.

### Validation (Windows)

1. `$RELEASE --stack-r11-stores`. The module builds. `grep -c "store_stack"
   functions_*.cpp` grows by about the number of `_savegpr` sites times
   their register counts.
2. Routes as above. A/B on the 32 MB CCD steady scene, 3 rounds paired.

---

## 1.5 Paired singles always on (`--assume-paired-singles`)

### Design

Gap item 1 notes that the second lane is always written, because HID2.PSE
never changes in RMGE01. Generation 25 applied this to `lfs` in fast-float
modules. Three other paths still load and test `context->hid2`:

- every psq load and store (HID2[PSE], plus [LSQE] for the d-forms);
- every single-precision result, before it fills ps1 (`write_single`);
- the exact-model paired admission (`paired_mode`).

RMGE01 sets both bits in `__init_hardware`, before any game code runs, and
never clears them. On the hardware a paired or quantized instruction with
either bit clear raises an illegal-instruction exception. A running game
therefore only executes these instructions with the bits set, and the test
can never fail.

With the flag, `functions.h` defines `GALAXY_ASSUME_PAIRED_SINGLES 1`. Every
fast-path HID2 test then goes through `fast::hid2_admits`/`duplicates_single`
and folds to `true`. GQR type dispatch and the exact fallbacks for the other
types are unchanged.

### Emitted code

The C++ text is unchanged apart from the define in `functions.h`.

| Sequence | Retired instructions (off to on) |
| --- | --- |
| lfs, lfs, fmuls, stfs | 79 to 76 |
| Same, with `--hw-float-conversions` | 45 to 43 |
| psq_l, ps_add, psq_st | 114 to 105 |
| Same, with `--hw-float-conversions` | 78 to 69 |

### Risks

The flag changes behaviour only for code that runs a paired or quantized
instruction, or reads ps1, with HID2[PSE]/[LSQE] clear. On the hardware that
is an exception (paired), or ps1 of a register last written before
`__init_hardware` (scalar single results). The boot path runs no float code
before `__init_hardware`.

### Validation (Windows)

1. `ctest -R native_assume_paired_singles`.
2. `$RELEASE --assume-paired-singles`: the module builds, and
   `functions.h` carries the define.
3. `diff_sdkmath`, `diff_tier2`, `diff_tvec3`, `diff_gx`: 0 mismatches.
4. Boot and routes. A boot run with `GALAXY_TRACE_*` off must reach the title
   exactly as `$RELEASE` does, since the boot path is where HID2 is set. A/B
   on the 32 MB CCD steady scene.

---

## 2.1 EABI register synchronization (`--abi-call-sync`, `--abi-exit-sync`)

### Design

Register residency (`crate::residency`) keeps GPRs, LR, CTR and, with
`--experimental-guest-resident-float`, the FPR/PS1 file in host locals. It
treated every translated call as an opaque boundary. Before the call it
stored every local the function writes anywhere; after it, it reloaded every
local the function uses. A `return;` stored every written local. Measured
on the probe corpus: 5 stores and 5 reloads around each direct call.

The two flags apply the PowerPC EABI contract instead, at the boundaries
where it holds:

- **Calls** (`--abi-call-sync`). This covers a `bl` to a translated function
  entered at its start, and a `bctrl`/`blrl` through
  `fast::call_guest_indirect`. Before the call, only written locals a callee
  may read are stored: r3-r10, f1-f8, r1/r2/r13, the non-volatile r14-r31 and
  f14-f31, and LR. After the call, only the results r3/r4 and f1/f2 are
  reloaded. Volatile non-arguments (r0, r11, r12, CTR, f0, f9-f13) are
  neither stored nor reloaded, and nor are non-volatile registers, which the
  callee preserves.
- **Returns** (`--abi-exit-sync`). A `blr` stores only what a caller may
  observe: the results, the non-volatile registers and LR. It no longer
  stores volatile scratch such as r0 (`mflr r0`/`mtlr r0` in every non-leaf
  function).

The "safer variant" is kept on purpose: dirty non-volatile registers are
still stored before a call and at a return. The callee's prologue saves the
true values to the stack frame, so guest memory is unchanged. The context
also stays correct for non-resident functions (about 38 % of the module,
see ledger T-RESIDENCY-FALLBACK), which read registers straight from it.

The translator marks eligible sites in the lowered text:
`GALAXY_ABI_CALL(0x<pc>u);` before a linked call and `GALAXY_GUEST_RETURN;`
for a `blr`. Both are no-op macros in `galaxy/native_api.h`, so a body the
residency pass leaves alone compiles as before. The pass replaces them with
the contract's sequences. A marker whose next synchronizing call is not a
translated or indirect guest call is dropped, so that call keeps full
synchronization. This covers hooks, `call_guest_resolved`, natives and
wrappers. A marker is also dropped at a label or when its block closes.

**Analysis** (`crate::ppc_dataflow`, `ModuleAbi::analyze`) runs before
lowering, over every translated function:

1. Effect decoder for every lowered form, including the implicit operands:
   LR, CTR, CA/SO, CR0/CR1 of record forms, and the lmw/stmw runs. The
   `bl _savegpr_N`/`_restgpr_N` helpers, which the translator lowers in
   place, are modelled as their register effects (read r11 and rN..r31, or
   write rN..r31), not as calls.
2. Backward liveness per function. A call uses the EABI model, a branch to
   another function is a tail call, and `bctr` makes everything live.
3. Per function: volatile non-argument registers read at entry
   (`entry_extra`, stored by its callers), and non-volatile registers written
   without being saved (`clobbers`). `clobbers` is transitive over direct
   calls, tail calls and indirect calls (which may reach any address-taken
   function), and hidden by registers the function itself saves and
   restores.
4. Per call site: volatile registers the caller reads after the call before
   rewriting them. These are computed with a precise argument model, a
   fixed point over the call graph of the argument registers each callee
   really reads, so `bl A; bl B` does not count B's unused argument
   registers. The union over the module is `always_synced`: those registers
   are stored and reloaded at every call and stored at every return.
5. **Opt-out list.** These functions keep full synchronization at their own
   calls and returns and when called directly:
   - host substitutions and pure helpers (`native`);
   - functions with an instruction the decoder does not model
     (`undecodable`);
   - functions with a `bl` into their own body (`internal-link`; `bl $+4` is
     modelled exactly).
6. **Indirect calls.** They use the union of `entry_extra` and `clobbers`
   over the address-taken functions: start addresses found as a word in any
   DOL section, or built by `lis` with `addi`/`ori`. If an opted-out
   function is address-taken, every indirect call stays fully synchronized.

The opt-out list and every non-ABI fact are written to `abi_contract.json`
next to the shards (the module manifest for this feature). The CLI prints
its path. It holds:

- counts: functions, opted out, functions with non-ABI facts, call sites
  using the contract;
- `alwaysSynced`;
- the indirect-call extras;
- one row per opted-out function with its reason;
- one row per function with `entryExtra`/`clobbers`/`readsAfterCalls`.

### Emitted code (call_direct probe: prologue, two `bl` to a frame callee, epilogue)

Before (`$RELEASE`, each call):

```
glr = 0x8070F018u;
context->pc = 0x80701000u;
{ context->gpr[0] = gr0; context->gpr[1] = gr1; context->gpr[3] = gr3; context->gpr[31] = gr31; context->lr = glr; rmge01::fn_80701000(context, memory, services); gr0 = context->gpr[0]; gr1 = context->gpr[1]; gr3 = context->gpr[3]; gr31 = context->gpr[31]; glr = context->lr; }
...
{ context->gpr[0] = gr0; context->gpr[1] = gr1; context->gpr[3] = gr3; context->gpr[31] = gr31; context->lr = glr; return; }
```

After (`--abi-call-sync --abi-exit-sync`):

```
glr = 0x8070F018u;
context->pc = 0x80701000u;
{ context->gpr[1] = gr1; context->gpr[3] = gr3; context->gpr[31] = gr31; context->lr = glr; rmge01::fn_80701000(context, memory, services); gr3 = context->gpr[3]; }
...
{ context->gpr[1] = gr1; context->gpr[3] = gr3; context->gpr[31] = gr31; context->lr = glr; return; }
```

The call-return checkpoint's cold block, the interior-entry stubs and every
unmarked boundary keep full synchronization.

### Measured (asm loop, x86 instructions)

Measured with the asm loop (see "Measurement loop" below). The base is
`$RELEASE` plus the item 1 flags. Static is the Win64 assembly; dynamic is
callgrind per call.

| Probe | Static before | Static after | Dynamic before | Dynamic after |
| --- | --- | --- | --- | --- |
| call_direct (2 calls) | 175 | 172 | 96 | 86 |
| call_virtual (bctrl) | 162 | 158 | 98 | 90 |
| int_alu (exit stores) | 53 | 31 | 46 | 28 |
| int_load (exit stores) | 42 | 11 | 35 | 8 |
| carry | 42 | 35 | 35 | 28 |
| dcbz_line | 66 | 58 | 44 | 38 |
| all 21 measured probes | 1943 | 1858 | 1034 | 948 |

Float residency on top (`--experimental-guest-resident-float`) now pays,
because floats no longer sync at every call and return: float_single 58 to
52, paired 110 to 98, all probes 948 to 929.

### Risks

All failure modes are the same class: a guest register's value differs from
the hardware without a crash. Wrong physics, animation or menu state is the
symptom.

- **A callee that breaks the EABI without the analysis seeing it.** Examples:
  hand-written assembly that keeps a value in a volatile register across a
  call, or reads a caller's non-volatile register without saving it. The
  analysis reports a non-volatile write as "saved" when the function also
  reads the register at entry. A function that takes an input in r31 and
  returns it modified would be missed. Mitigation: the opt-out list and the
  `nonAbi` rows. Check them against the RMGE01 symbol map. Unknown targets
  and aliases (calls into a function's interior) always keep full
  synchronization.
- **Opted-out callers.** An opted-out function (local subroutine, unmodelled
  form) that reads a volatile register after calling an ABI function would
  see the callee's stale value, because the callee's return no longer
  stores it. Only hand-written assembly does this.
- **Indirect calls.** These rely on the address-taken scan. A function
  pointer computed some other way, for example base plus offset arithmetic,
  is not seen. The scan over-approximates: every data word equal to a
  function start counts.
- **Runtime readers of the context during a call.**
  `call_guest_indirect_boundary` (an event pending or a withheld target)
  reads LR, r2 and the arguments, all stored. Hooks reading other volatile
  registers would see stale values.

### Validation (Windows)

1. `$RELEASE --abi-call-sync --abi-exit-sync`: the module builds with
   clang-cl `/W4 /WX`.
2. Read the generation output:
   - `abi_contract.json`: record `optedOut`, `nonAbiFacts`, `abiCallSites`,
     `alwaysSynced` and `indirectCall`;
   - `alwaysSynced` should be `-` or a few registers; if it is large, attach
     the file to the report;
   - `indirectCall` must not be null unless an opted-out function is
     address-taken;
   - every `optOut` row should be a known assembly or OS routine
     (`OSSaveContext`, `__OSPSInit`, `PPCMtmsr`, ...).
3. Differential harnesses against `$RELEASE`: `diff_sdkmath`, `diff_tier2`,
   `diff_tvec3`, `diff_strings`, `diff_gx`.
4. Routes: boot to title, gameplay route, Home menu, save, prologue movie.
   Compare frame dumps.
5. Bisect a miscompile:
   - `NEBULA_RESIDENCY_ADDRESS_RANGE=lo-hi` limits residency, and so these
     flags, to a function range;
   - each flag also works alone (`--abi-exit-sync` without
     `--abi-call-sync`).
6. A/B `ab-affinity` on the 32 MB CCD, 3 rounds: simulation-thread share,
   module and `.text` size. Run it with and without
   `--experimental-guest-resident-float`.

---

## 2.2 Float arithmetic on typed locals (`--single-float-arithmetic`, `--fast-float-forms`)

### Design

"Typed locals" combines three flags. Each is A/B-testable alone.

1. **Locals.** `--experimental-guest-resident-float` already keeps the
   FPR/PS1 file in host locals (`fpr[32]`/`ps1[32]` arrays with constant
   indices, which clang promotes to registers). It measured slower on its own
   (docs/NATIVE_EXECUTION.md:458): every call and return synchronized every
   used float register. Under `--abi-call-sync --abi-exit-sync` (2.1) a call
   synchronizes only f1-f8 and f14-f31 and a return stores only f1/f2 and
   f14-f31. Float residency now pays, and all the numbers below include it.
2. **Binary32 arithmetic** (`--single-float-arithmetic`; `functions.h`
   defines `GALAXY_SINGLE_FLOAT_ARITHMETIC`). The fast paths of the single
   and paired-single forms round their operands to binary32 and compute in
   binary32:
   - `mulss`/`addss`/`vfmadd*ss` for fadds/fsubs/fmuls/fdivs and the fmadds
     family;
   - `mulps`/`addps`/`vfmadd*ps` on two lanes for ps_add/ps_sub/ps_mul/
     ps_muls*/ps_div and the ps_madd family;
   - binary32 for ps_sum0/ps_sum1.

   The Gekko computes in binary64, rounds the multiplier of a single multiply
   to 25 bits, and rounds the result to binary32. For binary32 operands,
   which is every operand produced by lfs, psq_l and single arithmetic, the
   results are bit-identical for +, -, x and /:
   - binary64 holds every exact binary32 sum and product, and rounds a
     quotient finely enough that the second rounding cannot change the
     binary32 result;
   - the 25-bit rounding is the identity on a binary32 value.

   The fused forms may differ by one binary32 ulp (double rounding). A
   binary64 operand (from lfd or a double op) is rounded to binary32 first.
   No bit-exactness is required. The payoff:
   - the 25-bit rounding goes: 4 instructions per multiply, moving the value
     to a GPR and back;
   - a chain of single operations stays in binary32 registers, because clang
     folds every `cvtsd2ss(cvtss2sd(x))` pair;
   - the float_loop body below became a single `vmulss` between the load and
     the store.
3. **Fast forms** (`--fast-float-forms`). Several forms still went through
   exact FPSCR or commit paths even in a fast-float module:
   - `fres`, `frsqrte`, `ps_res` and `ps_rsqrte` called SoftFloat estimate
     helpers and `ppc_commit_*` (ledger T-FEST);
   - `fcmpu`, `fcmpo` and `ps_cmp*` updated FPSCR[FPCC] and tested for
     signalling NaNs (T-FCMP);
   - `fsel` and `ps_sel` were integer NaN and sign tests (T-FSEL).

   With the flag they lower to `galaxy::fast` forms that write the register
   file directly, so residency keeps the operands in locals:
   - the reciprocals are correctly rounded rather than the 12-bit table
     estimate, and game code's Newton-Raphson refinement converges from
     either;
   - the selects test `value >= 0.0`: NaN selects the second operand and -0
     the first, as on the hardware;
   - the compares set the CR field only.

   With `--assume-paired-singles`, `require_paired_single_mode` is also gone
   from every paired form (ledger T-PSMOV, header-only).

### Emitted code (float_select probe: fsel, frsqrte, fres, fcmpo cr1)

Before:

```
const std::uint64_t selector = context->fpr_bits[1];
const bool select_nonnegative = !galaxy::f64_is_nan(selector) && ((selector & 0x8000000000000000ull) == 0 || (selector & 0x7FFFFFFFFFFFFFFFull) == 0);
context->fpr_bits[3] = select_nonnegative ? context->fpr_bits[2] : context->fpr_bits[4];
const galaxy::PpcFloatResult result = galaxy::ppc_f64_reciprocal_sqrt_estimate(context->fpr_bits[2], context->fpscr);
galaxy::ppc_commit_scalar_result(context, 5u, result, false, false, services, 0x8070C004u);
const std::uint64_t source = context->fpr_bits[2];
const galaxy::PpcFloatResult result = galaxy::ppc_f64_reciprocal_estimate(source, context->fpscr);
galaxy::ppc_commit_scalar_result(context, 6u, result, true, false, services, 0x8070C008u);
galaxy::compare_f64(context, 1u, context->fpr_bits[5], context->fpr_bits[6], true);
```

After (`--fast-float-forms`):

```
context->fpr_bits[3] = galaxy::fast::select_nonnegative(context->fpr_bits[1]) ? context->fpr_bits[2] : context->fpr_bits[4];
context->fpr_bits[5] = galaxy::fast::reciprocal_sqrt_bits(context->fpr_bits[2]);
{ const std::uint64_t estimate = galaxy::fast::reciprocal_single_bits(context->fpr_bits[2]); context->fpr_bits[6] = estimate; if (galaxy::fast::duplicates_single(context)) context->ps1_bits[6] = estimate; }
galaxy::fast::compare_f64_cr(context, 1u, context->fpr_bits[5], context->fpr_bits[6]);
```

`--single-float-arithmetic` changes no C++ text. Only `functions.h` gains the
define.

### Measured (asm loop, x86 instructions per executed guest instruction)

The base is `$RELEASE`, the item 1 flags and `--abi-call-sync
--abi-exit-sync`. "+res" adds `--experimental-guest-resident-float`, "+b32"
adds `--single-float-arithmetic`, and "+forms" adds `--fast-float-forms`.
Dynamic counts are from callgrind.

| Probe | base | +res | +res +b32 | +res +b32 +forms |
| --- | --- | --- | --- | --- |
| float_single (lfs, fmuls, fadds, fmadds, stfs) | 6.4 | 5.8 | 5.1 | 5.1 |
| float_double | 5.1 | 5.0 | 5.0 | 5.0 |
| float_loop (lfs, fmuls f0,f0,f1, stfs in a loop) | 5.0 | 4.4 | 3.9 | 3.9 |
| paired (psq_l, ps_mul, ps_madd, ps_sum0, psq_st) | 13.8 | 12.2 | 12.0 | 12.0 |
| paired_mtx (PSMTXMultVec row) | 14.4 | 12.8 | 12.8 | 12.8 |
| float_compare (fcmpu, fmr, fctiwz) | 11.5 | 11.5 | 11.5 | 9.5 |
| float_select (fsel, frsqrte, fres, fcmpo) | 45.9 | 47.5 | 47.5 | 8.1 |
| all 24 measured probes | 5.7 | 5.5 | 5.5 | 4.3 |

### Risks

- Binary32 arithmetic: a result differs from the hardware when an operand is
  binary64 (lfd, double arithmetic, fctiwz output reinterpreted) or in a fused
  double-rounding corner. Each differs by at most one binary32 ulp of the
  result per operation, in the Dolphin/WiiCompiled accuracy class the fast-
  float module already accepted. Deterministic replays that compare float
  state bit for bit against `$RELEASE` will diverge; physics and animation
  should not visibly change.
- Estimates: games that use fres/frsqrte without refinement (rare) get a more
  accurate value than the hardware.
- Compares: FPSCR[FPCC] and the invalid-operation sticky bits are no longer
  updated by compares. Only code that reads FPSCR (`mffs`) after a compare
  would notice; RMGE01 reads FPSCR in OS context save/restore only.

### Validation (Windows)

1. `ctest -R native_single_float_arithmetic` (and the existing
   `native_fast_float`, `native_assume_paired_singles`).
2. `$RELEASE --abi-call-sync --abi-exit-sync --experimental-guest-resident-float
   --single-float-arithmetic --fast-float-forms` builds; `functions.h` defines
   `GALAXY_SINGLE_FLOAT_ARITHMETIC`; `grep -c "ppc_f64_reciprocal" functions_*.cpp`
   is 0.
3. `diff_sdkmath`, `diff_tier2`, `diff_tvec3`: expect last-bit float
   differences only; review any larger one. `diff_gx` (vertex streams written
   through psq_st) likewise.
4. Routes and frame dumps: gameplay (camera, Mario physics, gravity fields),
   Home menu, prologue movie, save. Visual comparison, not bit comparison.
5. A/B each of the three flags alone and together, 3 rounds, 32 MB CCD.
