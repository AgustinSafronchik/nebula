# Performance program: where to start

Read in this order.

1. `PERFORMANCE_PLAN_NATIVE11.md`: the measurements (desktop and laptop
   recordings of Native10), where every percent of the simulation thread
   goes, and the prioritized items P0 to P12 with evidence, steps and
   validation. Section 3 lists the scripts, work directories, log lines
   and pitfalls.
2. `SDK_REPLACEMENT_PLAN.md`: the end state. The Nintendo SDK becomes host
   code, guest threads become fibers, scheduling is cooperative and polled,
   and no hardware below the SDK is emulated. Per-subsystem design with
   every function address, work order and validation.
3. `FIBER_STEP1A_GUIDE.md`: the first implementation step in detail
   (guest contexts as fibers under the existing scheduler, removing the
   exception unwinding).
4. `sdk/RMGE01_SDK_FUNCTIONS.txt`: the 1,125 SDK functions of RMGE01 with
   address and size, from the public Petari symbol table
   (`D:\NebulaWork\ref\symbols\RMGE01-symbols.txt` holds all 42,060 `.text`
   symbols).
5. `NATIVE_EXECUTION.md`: the history and status table of everything
   already shipped.

Work order, biggest gain first:

| Order | Item | Where | Expected gain |
| --- | --- | --- | --- |
| 1 | Crash dumps, thread names | Plan P0 | debuggability |
| 2 | Timer resolution and power-throttling opt-out | Plan P5, SDK plan 2.5 | removes the 20 to 105 ms laptop stutters |
| 3 | Guest contexts as fibers | Step 1a guide | 10 to 14 % of the simulation thread |
| 4 | Polled interrupt delivery, alarms, VI clock | SDK plan 1b, 2 | broker, decrementer and VI emulation gone |
| 5 | Generation 18 without checkpoints | SDK plan 1c, 8 | checkpoint bodies (33k/s) and generated-code tests gone |
| 6 | Host scheduler and sync primitives | SDK plan 1d | translated scheduler gone |
| 7 | Zelda ucode HLE, AI clock, ARAM as memory | SDK plan 4, Plan P3 | DSP worker thread and mailbox spin gone |
| 8 | DVD, IOS, NAND, SC as host calls | SDK plan 5 | DI/IPC emulation and pak working set gone |
| 9 | WPAD/KPAD as host calls | SDK plan 6 | Bluetooth/USB stack and keyboard polling gone |
| 10 | GX stage A then B | SDK plan 3 | PE emulation, FIFO pre-classification, then the FIFO parse |
| 11 | THP decode-ahead and SIMD | Plan P6 | video stutter |
| 12 | EFB copy scale cap and identity aliasing | Plan P7 | 1080p on integrated GPUs |
| 13 | Working set under 2 GB | Plan P8 | 8 GB target |
| 14 | Syscall leftovers, translator gains, render thread | Plan P9 to P11 | the remaining percent |

Measure every step with the smoke scripts and a laptop Detailed recording
(`diag_digest.py`), and record the result in `NATIVE_EXECUTION.md`.
