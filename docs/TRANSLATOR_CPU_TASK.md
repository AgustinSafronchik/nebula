# Translator work: less CPU per guest instruction (gap report items 1, 3, 4)

Scope of this task: the Rust translator in `crates/nebula-recomp-core` (and its CLI in
`crates/nebula-recomp-cli`), developed and unit-tested without the game. The generated module is
compiled, run and measured elsewhere (Windows, MSVC/clang-cl, the real `main.dol`); this document is
the contract between the two sides.

## Read first
1. `docs/CPU_GAP_REPORT.md`: where the simulation thread's time goes and the ranked plan. This task is
   items 1 (lean memory paths), 3 (registers that trust the ABI) and 4 (checkpoint-free generation).
2. `docs/PERFORMANCE_PLAN_NATIVE11.md` section P10 and section 3 (tooling, pitfalls), then
   `docs/NATIVE_EXECUTION.md` (what already shipped: lean SDK math bodies, CFG device-access
   classification, inline savegpr/restgpr, CR compare+branch fusion, leaf call-return elision, shared
   call-return cold blocks, flat-base assume, `store_stack`, lfs ps1 unconditional).
3. `docs/SDK_REPLACEMENT_PLAN.md` section 1 (cooperative scheduler; the runtime's `coop_scheduler` is
   on by default: guest threads are fibers, the host runs SelectThread, events are delivered at
   checkpoints today).
4. Code: `crates/nebula-recomp-core/src/translate.rs` (emission; `ModuleTranslationOptions`,
   `is_checkpoint_free_leaf`, `flat_store_helper`, `lean_sdk_math_symbol`, the lfs/stfs/psq paths),
   `residency.rs` (register residency, call-return cold blocks), `guest_cfg_liveness.rs`,
   `guest_abi_effect.rs`, `guest_ppc_effect.rs` (liveness and effect analyses the ABI work builds on),
   `cr0_fusion.rs`; `runtime/include/galaxy/native_fast_paths.h` (the helpers generated code calls:
   `flat_base`, `store_stack`, loads/stores, float paths) and `runtime/include/galaxy/native_api.h`
   (module ABI 27: `NativeServicesV1`, `kNativeAbiVersion`, module flags).
5. The public Petari symbol map for RMGE01 (SMGCommunity/Petari on GitHub, `config/RMGE01/symbols.txt`)
   names every function; the runtime's addresses match it.

## Rules
- Every feature behind its own CLI flag on `translate-module` (default off), e.g.
  `--hw-float-conversions`, `--checkpoint-globals`, `--abi-trusting-registers`,
  `--checkpoint-free`, so each one can be A/B-measured alone. Flags compose.
- The runtime contract stays ABI 27 unless unavoidable. If a feature needs a runtime-side change
  (a new helper in `native_fast_paths.h`, a new module flag, a services-table field), implement the
  header side in the same commit, keep the old path intact when the flag is off, and list the change
  in the "Runtime changes" section below with the exact file and line.
- Correctness bar: no bit-exactness requirement (hardware float conversions are fine, NaN payloads
  and denormal flushing may differ from the PowerPC), but guest-visible control flow, memory contents
  and the stack discipline must be unchanged. The four flat transfers (OSInit RealMode BAT setup) keep
  full register synchronisation. Functions on the ABI opt-out list keep today's code.
- Tests: `cargo test --workspace`, `cargo clippy --workspace --all-targets -- -D warnings`,
  `cargo fmt --check` must pass. Pin emitted text with unit tests on synthetic PowerPC sequences
  (the existing tests in `translate.rs` show the style). Add a test per feature per flag state.
- Commits: small, one feature step each, subject in the style of `git log`. Never commit generated
  modules or any game-derived data (`docs/CONTENT_POLICY.md`).
- Write `docs/TRANSLATOR_CPU_RESULTS.md` as you go: per feature, the design, the flag, what the
  emitted code looks like before/after for one representative sequence, the risks, and the exact
  validation steps for the Windows side (see "Validation elsewhere").

## Work items (in order)
### 1. Lean memory paths (gap item 1)
1. `--hw-float-conversions`: `lfs` = load 32 bits, `cvtss2sd`; `stfs` = `cvtsd2ss`, store 32 bits;
   `psq_l`/`psq_st` float (non-quantised) lanes likewise; the integer widen/narrow helpers stay for
   the flag-off path. Keep the ps1 fill rule (lfs fills ps1; `HID2[PSE]` never changes in RMGE01).
2. `--checkpoint-globals`: the checkpoint counters/gate words the generated code tests after calls
   become module globals (one `extern` per module, defined in the module's prelude shard, pointed to
   by a new optional services field or filled at module load through an existing hook), so the hot
   path stops re-reading the services table. Document what the runtime must write and when.
3. Remove the remaining duplicate non-flat blocks in fastmem modules (`flat_base` is assumed
   non-null since generation 25; find emission sites that still emit the null-base fallback).

### 2. Registers that trust the ABI (gap item 3; the big one)
Design, then implement behind `--abi-trusting-registers`:
- Non-volatile r14-r31 (and f14-f31, CR2-CR4) become C++ locals for the function's lifetime: no
  store to the context before a call and no reload after; the prologue loads them from the context
  at entry, the epilogue stores the ones the function wrote (PowerPC EABI: callee-saved).
- Volatile non-argument registers (r0, r5-r12 beyond set arguments, f0, f3-f13, CR0/1/5-7, XER,
  CTR) are never written back around a call.
- A call stores only the argument registers it set (r3-r10, f1-f8 by the callee's signature from
  the liveness analysis, else all argument registers) and reloads r3, r4, f1, f2 after it.
- Indirect calls (`bctrl`) and calls into non-translated code (host helpers, RSO sidecars, the Home
  Button module) use the conservative contract.
- Opt-out list: functions that read a volatile register after a call (4), read scratch registers at
  entry (14: exception vectors, `OSSaveContext`, the pointer-to-member thunk), or may read a caller's
  non-volatile register (76) per the census in `docs/CPU_GAP_REPORT.md`; derive the list from the
  analyses in `guest_cfg_liveness.rs` / `guest_abi_effect.rs` rather than hard-coding addresses, and
  emit it into the module manifest so the runtime can print it.
- Flat transfers and the Home Button sidecar's flag returns keep full synchronisation
  (`docs/NATIVE_EXECUTION.md`, `rso.rs` `generate_home_button_sidecar`).
- Interaction with residency (`residency.rs`): the shared call-return cold block exists because
  every call site synchronises; with trusted registers most sites need no cold block. Keep the
  cold-block machinery for conservative sites.

### 3. Checkpoint-free generation (gap item 4; plan item 5)
Behind `--checkpoint-free`: emit event delivery only at cooperative scheduler points (calls into the
host, `OSDisableInterrupts`/`OSRestoreInterrupts`/`mtmsr` sites, syscalls) and keep loop checkpoints
only in loops that wait on memory without any call (found statically: a backward branch whose body
has loads and no `bl`/`bctrl`). Document precisely which guest behaviours depend on timely delivery
(decrementer alarms, VI retrace, AI DMA, DSP mail) and how each remains served by the scheduler.

### 4. If time remains
Float registers as typed locals (gap "Later" list); small-leaf inlining across shards.

## Validation elsewhere (what the Windows side runs for each flag)
1. `nebula-recomp translate-module --count 0 --experimental-inline-scalar-single-binary
   --shard-source-kib 512 <flags>` on the real `main.dol`; the module builds with clang-cl `/W4 /WX`.
2. Differential harnesses (load `main.dol`, run two modules side by side on the same inputs):
   `diff_sdkmath`, `diff_tier2`, `diff_tvec3`, `diff_strings`, `diff_gx` must report 0 mismatches.
3. Routes: gameplay, Home menu open/close, boot, save, prologue movie; frame dumps compared; flag-return
   counts unchanged.
4. A/B: simulation-thread share on the 32 MB-L3 CCD (`ab-affinity`), 3 rounds paired, plus module
   size and `.text` size.

## Runtime changes
(filled in by the task: file, line, what, why)

Module ABI stays 27 for every change below. Each is header-only and additive:
a module built without the new flag never names the new helpers.

1. `--hw-float-conversions` (work item 1.1)
   - `runtime/include/galaxy/native_fast_paths.h`:
     - Line 259: new section "Hardware float conversions": `widen_f32_hw`
       (271), `narrow_f64_hw` (276), the lane selectors `widen_single_lane<Hw>`
       (284) and `narrow_paired_lane<Hw>` (293), `load_fpr_single_hw` (301)
       and `store_fpr_single_hw` (316).
     - `psq_load` and `psq_store` are now wrappers over `psq_load_impl<Hw>`
       (357) and `psq_store_impl<Hw>` (440), with new `psq_load_hw` (429)
       and `psq_store_hw` (530). With `Hw = false` the bodies are unchanged:
       the template parameter only selects the lane conversion.
     - Resident overloads: `load_fpr_single_hw_r` (1356),
       `store_fpr_single_hw_r` (1370), `psq_load_r_impl<Hw>` (1381),
       `psq_load_hw_r` (1455), `psq_store_r_impl<Hw>` (1466) and
       `psq_store_hw_r` (1514). `psq_load_r`/`psq_store_r` are wrappers over
       these, in the same way.
     - Why: the generated code calls these helpers. The old helpers keep the
       flag-off path.
   - `runtime/src/native_hw_float_conversion_tests.cpp` (new) and
     `CMakeLists.txt` lines 886-888, 1109, 1157: the
     `galaxy_native_hw_float_conversion_tests` target (`ctest -R
     native_hw_float_conversion`).
2. `--checkpoint-globals` (work item 1.2). This one needs runtime code to
   take effect; until that code exists, the module is correct but slower.
   - `runtime/include/galaxy/native_api.h`:
     - Line 15027: new section "Module checkpoint gate".
       `ModuleCheckpointGateV1` (15057) is the module's snapshot of the
       inline-gate decision. `refresh_module_checkpoint_gate` (15079)
       derives it from the service table.
     - Under `GALAXY_MODULE_CHECKPOINT_GLOBALS`: the module global
       `g_module_checkpoint_gate` (15112), the snapshot tests
       `branch_checkpoint_due_g` (15132),
       `branch_checkpoint_due_budgeted_g` (15142),
       `call_return_checkpoint_due_g` (15152) and `transfer_pending_g`
       (15167), and the macros `GALAXY_RETURN_IF_TRANSFER_PENDING_G`,
       `GALAXY_CALL_RETURN_CHECKPOINT_G`,
       `GALAXY_BRANCH_CHECKPOINT_TAKEN_BUDGETED_G` and
       `GALAXY_BRANCH_CHECKPOINT_TAKEN_G` (15175-15210).
     - Line 16303: declaration of the optional export
       `galaxy_module_checkpoint_gate()`.
   - Runtime work to do (not in this branch). The runtime calls
     `galaxy::refresh_module_checkpoint_gate(gate, &services)` in these
     places:
     - `runtime/src/native_runtime.cpp`, after `init_fn(&services)` succeeds
       (about line 33495): look up the optional `galaxy_module_checkpoint_gate`
       export (`load_export`), keep the pointer in the runtime state, and
       refresh. The gate is already bound at line 33489.
     - `runtime/src/checkpoint_gate.cpp`, `publish_full_unchecked` and
       `publish_inline_unchecked`: after each publication. These cover bind,
       unbind, activate, scope entry and exit, and `exchange_chain` on fiber
       switches. Suggested shape: an optional `void (*)(void*)` observer that
       `GateController` calls at the end of both functions.
     - `runtime/src/native_runtime.cpp` lines 29064-29068: after the
       checkpoint callback sets or clears
       `kNativeServiceFlagAdaptiveCallReturnCheckpoints`.
     - Before the module is unloaded or the service table is destroyed:
       refresh with `nullptr`.
     - Every one of these runs on the simulation thread, so no
       synchronisation is needed. Translated code re-reads the snapshot after
       every call, and every refresh happens inside a call.
   - `runtime/src/native_module_checkpoint_gate_tests.cpp` (new) and
     `CMakeLists.txt` lines 889-890, 1110, 1158: the
     `galaxy_native_module_checkpoint_gate_tests` target. It covers 576
     service-table scenarios: every flag, pointer, event and kind
     combination, each through eight state changes. In all of them the
     snapshot helpers return the same counted value, advance the counter the
     same way and give the same transfer test as the service-table helpers.
3. `--flat-view-only` (work item 1.3)
   - `runtime/include/galaxy/native_api.h`:
     - Line 8713: `GALAXY_FLAT_VIEW_ONLY_ACTIVE` (fastmem plus the
       module's `GALAXY_FLAT_VIEW_ONLY`, unless
       `GALAXY_FASTMEM_KEEP_NULL_FALLBACK`) and `assumed_flat_view` (8726).
     - `lean_guest_span` (8736) and `cstring::view` (10147) use the assumed
       view in that mode. Every other build keeps the checked arms unchanged.
   - `runtime/include/galaxy/native_fast_paths.h` line 228: new
     `fast::zero_line` for `dcbz`. A line in a RAM alias goes through the
     flat view; any other line goes to `guest_zero`.
   - `runtime/src/native_flat_view_only_tests.cpp` (new) and
     `CMakeLists.txt` lines 891-894, 1115, 1164: the
     `galaxy_native_flat_view_only_tests` target.
   - No runtime code changes. The module already exports
     `galaxy_module_memory_flags` (requires the flat view) in every fastmem
     build, and that is the guarantee this mode relies on.
4. `--stack-r11-stores` (gap item 1: r1/r11 accesses skip the gather-pipe
   test): no runtime change. It uses the existing `fast::store_stack`.

