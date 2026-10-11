# Step 1a implementation guide: guest contexts as fibers

Companion to `SDK_REPLACEMENT_PLAN.md` section 1 (step a). This is the
first implementation step of the SDK replacement and the one that removes
the C++ exception unwinding behind every context switch (14 percent of the
simulation thread on the 185H). It keeps the translated scheduler, the
checkpoints and the interrupt dispatch exactly as they are; it only
changes how control gets from one guest context to another.

## The key observation

Every place a guest context can be suspended today is one of two native
call sites, and resuming the context means returning from that call:

1. **Voluntary switch.** A thread calls `SelectThread` 804AB20C (from
   `OSSleepThread`, `OSYieldThread`, `OSSuspendThread`, `OSExitThread`,
   mutex/message waits via `__OSReschedule` 804AB434). SelectThread calls
   `OSSaveContext` 804A379C (returns 0), picks the next thread, and calls
   `OSLoadContext` 804A381C, whose `rfi` (at 804A38F0) resumes the other
   context. When this thread is later chosen, the hardware returns from
   its `OSSaveContext` call a second time with r3 = 1 (setjmp semantics;
   resume pc 804AB30C). The native frames of this thread at the moment it
   called OSLoadContext are: its caller chain, then SelectThread, then the
   OSLoadContext call. The "yield RFI return protocol" that exists today
   (`execute_yield_select_with_rfi_return`, `exact_yield_rfi_return_state`,
   the generated protocol-1 branch that tests the restored pc after the
   OSLoadContext call and jumps to the `call_return_804AB30C` label)
   already proves that returning from the OSLoadContext call and jumping
   to the 804AB30C label is a correct resume.
2. **Interrupt.** A checkpoint (or `mtmsr`/`sc`/`mtdec` boundary) calls into
   the runtime, which dispatches an external, decrementer or FPU exception
   by running the translated exception wrapper (`run_translated_exception_wrapper`).
   The handler may end in `OSLoadContext` of the same context (resume: the
   wrapper returns to the checkpoint, which continues; that is what
   `branch_checkpoint_retaining_same_context` does today) or of another
   context (a preemptive switch).

So: give each guest context a host fiber. A switch to context C at resume
pc P is `SwitchToFiber(fiber[C])`; C's fiber was parked at one of the two
call sites above and simply returns from it. Nothing unwinds.

## Data structures (runtime)

```cpp
struct GuestFiber {
    void* fiber = nullptr;          // CreateFiberEx / ConvertThreadToFiberEx
    std::uint32_t context = 0;      // OSContext guest address (the key)
    galaxy::PpcContext ppc{};       // this fiber's private register file
    enum class Park : std::uint8_t { New, SelectThread, Interrupt, Running } park{};
    std::uint32_t entry_pc = 0;     // for New: the translated entry to call
};
std::unordered_map<std::uint32_t, GuestFiber> g_fibers; // by OSContext address
GuestFiber* g_running = nullptr;
```

Each fiber owns its `PpcContext`. Today one `PpcContext` is shared and
rewritten by OSLoadContext; with fibers, `OSLoadContext`'s translated body
still writes the register image from the OSContext into the *target*
fiber's `ppc` (call the translated body with `&target.ppc`), and the
switch hands that pointer to the target fiber.

## Steps

1. **Bootstrap.** In `run_guest_entry` (native_runtime.cpp, the top loop
   with `initial_entry`), call `ConvertThreadToFiberEx(nullptr,
   FIBER_FLAG_FLOAT_SWITCH)` once and register the boot fiber with the
   boot OSContext (read 0x800000D4 after `__OSContextInit` 804A3D20; until
   then key it as "boot"). The simulation thread's MXCSR (FTZ/DAZ set by
   the fast-float setup) is per thread and survives fiber switches only
   with `FIBER_FLAG_FLOAT_SWITCH`; set it.
2. **Fiber creation.** Create a fiber lazily the first time a context is
   loaded that has no fiber: `CreateFiberEx(64 << 10, 256 << 10,
   FIBER_FLAG_FLOAT_SWITCH, &fiber_entry, record)`. `fiber_entry` sets
   `g_running`, then calls the translated function at the context's SRR0
   through the same path `run_guest_entry` uses for a flat continuation
   (`state.lookup_function(pc)` and the flat continuation chain), with
   `&record->ppc`. New threads' first load always resumes at their entry
   (`__OSThreadEntry`-style trampoline); the flat chain handles return to
   the thread exit path exactly as today.
3. **Switch site 1, voluntary.** In the `OSLoadContext` intercept
   (`guest_address == 0x804A381Cu` in `call_guest_impl_body`, today it
   sets up `ScopedContextTransfer` and calls the translated body), after
   the translated body has loaded the register image into the target's
   `ppc`, and instead of letting `return_from_interrupt` throw:
   - mark the current fiber `Park::SelectThread`,
   - `SwitchToFiber(target.fiber)`,
   - on return (someone switched back to us), restore `g_running`, set
     r3 = 1 in our `ppc`, `context->pc = 0x804AB30C`, finish the ownership
     token, and return; the generated protocol-1 code after the call jumps
     to `call_return_804AB30C`. Require the module to advertise protocol 1
     (`state.yield_rfi_return_protocol`) or fall back to the exception
     path.
   The `rfi` inside the translated OSLoadContext body calls
   `return_from_interrupt`; when a fiber switch is going to follow, that
   service only applies the RFI to the target's `ppc` (`apply_rfi`) and
   returns; it must not throw. Gate on a `state.fiber_switch_pending`
   flag set by the intercept before calling the body.
4. **Switch site 2, interrupt.** In `return_from_interrupt` when the RFI
   comes from an exception wrapper (`state.dispatching_external_interrupt`
   / `dispatching_decrementer`) and the resumed context differs from the
   interrupted one: mark the current fiber `Park::Interrupt` and
   `SwitchToFiber(target)`. When this fiber is later resumed, return
   normally from `return_from_interrupt`, from the wrapper (remove
   `[[noreturn]]` from `run_translated_exception_wrapper`; it already
   returns in the F20-lite code), from the dispatcher, and from the
   checkpoint, which continues the interrupted code. The dispatch flags
   (`dispatching_*`, `external_dispatch`) are per fiber: save them into
   the fiber record before switching and restore after, since another
   fiber will run its own dispatches meanwhile. Same-context RFIs keep
   the existing retention path (no switch at all).
5. **Remove the unwinds.** With both sites switching, `throw GuestTransfer`
   is unreachable; keep it behind `GALAXY_FIBERS=0` for one release. The
   `catch (const GuestTransfer&)` sites (`run_guest_entry`,
   `branch_checkpoint_retaining_same_context`,
   `execute_yield_select_retaining_same_context`, `fpu_unavailable`,
   `dispatch_decrementer`) stay for the fallback only.
6. **Per-fiber state audit.** Everything that is per "native call chain"
   must move into the fiber record: `g_live_ctx_stack`,
   `g_branch_checkpoint_callback_depth`, `g_active_branch_checkpoint_pc`,
   `state.checkpoint_gate` scope depths, `g_pending_os_load_context`,
   `state.yield_rfi_return_proof`, `state.checkpoint_rfi_proof`, the
   scene-nerve trace pending record, the profiler's guest caller stack.
   `grep -n "thread_local\|^std::.*g_\|^[a-zA-Z_:<>]* g_" runtime/src/native_runtime.cpp`
   lists the globals; each one either moves into `GuestFiber` or is proven
   switch-invariant (state that is only valid between a dispatch and its
   RFI belongs to the fiber).
7. **Exit and deletion.** When a thread exits, its fiber parks forever in
   `OSExitThread`'s SelectThread call; delete it the next time any other
   fiber runs (`DeleteFiber` cannot delete the running fiber).
8. **Profiler.** `SuspendThread`/`GetThreadContext` sampling works
   unchanged on fibers (it samples the OS thread); the stack walker sees
   only the current fiber's frames, which is correct.

## Validation

1. Title smoke 120 s (`smoke-app.ps1 -Set GALAXY_FIBERS=1`): boot, VI
   cadence 60.0, `[transfer] exception-unwinds=0`.
2. Gameplay smoke with the replay input log; THP intro; a stage load.
3. Profile: the `return_from_interrupt` and `dispatch_decrementer`
   other-caller rows and the `KiUserExceptionDispatcher`,
   `RtlLookupFunctionEntry`, `RtlVirtualUnwind2`, `VCRUNTIME140_1` rows must
   be gone; simulation-thread running share on the title screen should
   fall by roughly the unwinder's share (10 to 14 percent on the laptop).
4. Laptop Detailed recording digest with `diag_digest.py`.

## Pitfalls

- `SwitchToFiber` to the current fiber is undefined: same-context RFIs must
  never reach it (they take the retention path).
- Fiber stacks: the deepest native chain (`stack-frames-max=24` in the
  profiler) is small; translated functions can have large frames (resident
  locals, spills); 256 KiB reserve with guard pages is safe, measure with
  `GetCurrentThreadStackLimits` in a debug build.
- Structured exception handling across fibers: the fastmem vectored
  handler is process-wide and keyed by thread id; fibers share the thread
  id, so it keeps working. C++ exceptions thrown inside a fiber must be
  caught inside the same fiber (RuntimeFailure is caught at the fiber
  entry and forwarded to the boot fiber, which rethrows).
- FLS: Windows fibers have their own FLS slots; the CRT uses FLS for
  `errno` and locale, which is why `FIBER_FLAG_FLOAT_SWITCH` and
  `ConvertThreadToFiberEx` (not plain `ConvertThreadToFiber`) are used.
