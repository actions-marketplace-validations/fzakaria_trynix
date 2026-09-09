# Where the emulator's time goes, and what moved it

This is the execution-speed counterpart to the startup investigation in
[opencode-startup.md](./opencode-startup.md). That work kept asking for
one thing it never had: an accounting of guest work that separates "the
browser runs more instructions" from "the browser runs the same
instructions slower." This measures the second directly, names the parts
of the gap, and reports what a rewritten backend did to each.

Short version. `opencode --version` took 408 seconds in the browser and
about 11 seconds on a native build of the same QEMU fork. A rewritten
wasm backend ([patches/0006-wasm32-batch-chain-locals.patch](../patches/0006-wasm32-batch-chain-locals.patch))
brings the browser to 171 seconds, 2.4x, and passes the full CPU probe.
On the microbenchmark it is 1.5x to 3.3x over the pinned engine
depending on instruction class, and its single hot block now runs faster
than the native backend's. What is left is a working-set problem: the
remaining time is generated code spread over tens of thousands of
blocks, where instruction-cache misses dominate and code shape no longer
helps. The engine is not the path to subsecond execution of a cold big
binary; the native service in
[experiments/native-exec](../experiments/native-exec) already does that
in 0.65 seconds.

## The instrument

`nix/emubench` is a static binary, one store path, built like the CPU
probe. Each test is a fixed block of inline assembly with a known
translation-block shape, so a ratio between two engines names a
mechanism instead of a workload. Run it on the native fork and in the
browser guest against the same package and machine and diff the numbers.

- Native: resume the guest under `vendor/qemu-native/qemu-system-x86_64`
  with a share holding the emubench closure, then run `emubench all`.
- Browser: serve a site whose `qemu/` holds the engine under test, open
  `?path=<emubench store path>&cache=<url key>&boot=1`, and type
  `emubench all` at the shell. This is the serve-a-store-path-from-a-cache
  trick `tools/cpu-test.py` uses; the closure is one path so it needs no
  network.

Raw numbers, all engines, are in
[experiments/wasm-batch-engine-results.json](../experiments/wasm-batch-engine-results.json).

## The gap by mechanism, before and after

Millions of guest instructions per second, higher is better. "stock" is
the pinned engine; "final" is the patch as committed.

| test         | native | stock | final | final / stock | native / final | what it isolates                  |
| ------------ | -----: | ----: | ----: | ------------: | -------------: | --------------------------------- |
| alu          |    862 |   349 |  1163 |          3.3x |           0.7x | one compiled block, no boundary   |
| alu2         |    558 |   119 |   173 |          1.5x |           3.2x | two blocks per iteration          |
| alu4         |    461 |    98 |   182 |          1.9x |           2.5x | four blocks per iteration         |
| call         |    258 |    68 |   102 |          1.5x |           2.5x | call/ret through the TB lookup    |
| indirect     |    353 |   103 |   159 |          1.5x |           2.2x | computed jump, varying target     |
| mem          |   1274 |   450 |   550 |          1.2x |           2.3x | TLB fast path                     |
| syscall (ns) |    544 |  4378 |  3003 |          1.5x |           5.5x | guest syscall entry/exit          |
| cold50k p2   |    147 |    18 |    21 |          1.2x |           7.1x | run-a-few-times code: interpreter |
| cold2k p1600 |   2739 |    16 |    53 |          3.4x |            51x | 2000 compiled blocks, dispersed   |

Three readings.

The single block (alu) went from 2.5x behind native to 1.35x ahead of
it. That is generated-code quality: guest registers moved from instance
globals to function locals, so V8's baseline compiler stops reloading
the globals base for every access and keeps values in registers.

Multi-block loops (alu2, alu4, call, indirect) went from ~4.7x behind
native to ~2.5x. That is the block boundary: each compiled block used to
return to the C dispatcher, which looked up the successor and called it
through the table; now it tail-calls the successor directly.

Dispersed compiled code (cold2k p1600, two thousand distinct blocks
visited once per pass) improved 3.4x and is still 51x behind native.
This is the regime real programs live in. Bun's blocks have a median of
about 8 guest instructions, so a JS runtime's start-up touches tens of
thousands of them, and the browser's machine code for a block is about
1.7 KB against native TCG's ~150 bytes. That is an instruction-cache
working set an order of magnitude larger, and no per-block code shape
changes it.

## What the pinned engine spent its time on

CPU profile of the vCPU worker across the whole 408 s command, busy time
only (about 353 s; the rest is idle and proxied-syscall wait):

| part                            | share |
| ------------------------------- | ----: |
| TCI bytecode interpreter        |   33% |
| C dispatcher `tcg_qemu_tb_exec` |   15% |
| generated code                  |   22% |
| softmmu loads/stores            |    6% |
| compile and instantiate         |    4% |
| TB lookup helper                |    3% |

Compilation is 4%. That single number settles the direction the startup
investigation kept circling: making module construction cheaper works on
4% of the time and cannot win on its own. The interpreter is 33% because
the stock compile threshold is 1500 executions and the 15000-instance
cache evicts under churn, so most of Bun's moderately-hot code never
runs compiled.

## The rewritten backend, in three stages

Each stage boots the pinned snapshot and passes `nix run .#cpu-test`
(every instruction family, the signed and unsigned multiply regressions,
self-modifying code). Each was measured on emubench and on opencode.

**Stage 1: batches with per-block import tables.** Up to 64 blocks share
one WebAssembly module. A block runs in the interpreter until it has run
32 times, then joins the pending batch; the batch compiles when full, or
when a queued block has waited 384 more runs so a lone hot block does not
wait for company. Cold code is never compiled. The cache holds about
256000 blocks with explicit eviction instead of waiting on the garbage
collector.

The part that makes this correct is that each block carries its own
helper-import table next to its body, and the assembler unions those
tables and rewrites every body's call immediates into the merged
module's import space. Without that, a batch can only hold blocks
translated consecutively, because the translator's helper registry is
reset per batch and a body's call indices are only valid against the
registry state at its translation. The earlier chaining prototype and a
first version of this one both tripped on exactly that: gate compilation
by hotness with a shared registry and blocks link to the wrong helpers.
It shows up as a machine that never boots.

Result: interpreter share 33% to 0.1%. opencode 408 s to 163 s.

**Stage 2: tail-call chaining.** `goto_tb` and `goto_ptr` read the
successor's function index from its block header and `return_call_indirect`
through the main module's table, instead of returning to C. A block's
prologue routes Asyncify rewinds: the dispatcher rewinds into the
function it originally called, which may have tail-called on before the
unwind, so a re-entered function forwards to whichever block `ctx.tb_ptr`
names. The dispatcher's share fell from 27% to 8% of busy time, but the
transition cost moved into the generated code and the opencode wall time
did not move (168 s). On emubench it is worth 1.3x to 1.6x on every
multi-block case.

**Stage 3: registers in locals.** The sixteen guest registers and the
block index are wasm locals instead of 25 mutable i64 globals per
instance. TCG spills every global to env at block boundaries, so nothing
needs to outlive the function. The one case that does, an Asyncify unwind
inside a helper call, leaves through one shared exit block that spills
the locals to a save area in ctx, and the rewind path reloads them.
Single-block throughput went 686 to 1163 mips; syscalls and page faults
each about 1.5x. opencode 171 s, within noise of the other stages.

## What is left, and what it would take

The final engine's profile on opencode: generated code 63%, dispatcher
11%, TB lookup 7%, translation 4%, softmmu 4%. The generated-code time is
spread over about 48000 distinct compiled blocks; the top 1000 account
for 40% and the top 5000 for 62%. The hot blocks are already tiered up by
V8 and run near native speed. The long tail runs V8 baseline code with
cache misses on every transition: the successor's block header, its
table entry and its machine code.

What could still move it, with honest expectations:

- Store the successor's function index in the source block's jump slot
  and back-patch predecessors through QEMU's jump lists when a block
  compiles or is evicted, removing one cache-missing load per transition.
  Perhaps 10-20% of the tail.
- A jump cache for `goto_ptr` in generated code, so returns and indirect
  calls skip `helper_lookup_tb_ptr` (7%) most of the time.
- Trim the per-block prologue and the block-index guard chain. Small.
- Bigger translation units would cut transitions but QEMU ends a block
  at every branch, and a region compiler is the multi-week project the
  startup investigation already scoped.

None of these change the picture: the browser is a 2.5x-per-instruction
JIT target with a working-set penalty on top. Stacked, they are a further
1.2x to 1.5x at most.

## Browsers without tail calls

The chaining stage needs WebAssembly tail calls: Chrome 112, Firefox
121, Safari 18.2 and later. The engine checks once at start-up whether
`WebAssembly.validate` accepts a `return_call`, and if not it emits every
block exit as a return to the dispatcher instead, logs one warning on the
console, and otherwise runs the same batches and locals. That path was
verified by forcing it in a test build: it boots, passes the CPU probe,
and measures at about stage-1 speed (alu2 132 mips, call 83; the pinned
engine 119 and 68). Nothing else in the engine or the page depends on a
newer browser than the pinned engine already did.

## Measuring a change

Two apps, both driving a headless browser the way `cpu-test` does:

- `nix run .#emubench -- --site <site> [--engine <dir>] [--json f]`
  prints the per-mechanism table above for the site's engine, or for a
  locally built engine overlaid on it. CI runs it as a smoke test after
  the CPU probe: every row has to print, the numbers go in the log.
- `nix run .#exec-bench -- --site <site> [--engine <dir>] --json new.json [--baseline old.json]`
  boots a fresh guest per package for a small suite (hello, ripgrep,
  jujutsu, python, opencode), runs each command cold then warm, and
  reports wall time, the guest's own user/sys time, and the browser
  process group's CPU and peak RSS; with a baseline it prints ratios.
  This is the regression check across sizes; run it before publishing an
  engine. It is too slow and too noisy for CI.

Run the CPU probe before either; a wrong engine can be fast.

- `nix run .#bench-history -- --site <site> --out site/bench/history.json`
  runs both of the above against every engine this repository has ever
  pinned, on one machine in one sitting: it builds the site at each
  commit that repinned, so nix verifies that engine, snapshot and guest
  image by hash, puts them under today's page, and records emubench
  (median of three, corrected by the guest's clock ratio) and exec-bench
  per release. The site's benchmark page, [site/bench/](../site/bench/),
  draws the file. Rerun it after publishing an engine; the tags are
  immutable, so the history can always be regenerated from scratch.

## Across sizes: the suite, pinned engine against patch 0006

`exec-bench` on the same site, single runs, Chromium 152 on a 16-core
host. Boot times were within noise of each other (about 5 to 9 s).

| package  | exec | wall stock | wall final | browser CPU stock | browser CPU final | peak RSS stock | peak RSS final |
| -------- | ---- | ---------: | ---------: | ----------------: | ----------------: | -------------: | -------------: |
| hello    | cold |     1.28 s |     1.28 s |            1.64 s |            1.77 s |       2644 MiB |       2691 MiB |
| hello    | warm |     0.77 s |     0.77 s |            0.90 s |            0.89 s |       2646 MiB |       2697 MiB |
| ripgrep  | cold |     1.28 s |     1.02 s |            1.54 s |            1.50 s |       2667 MiB |       2669 MiB |
| ripgrep  | warm |     0.52 s |     0.51 s |            0.62 s |            0.51 s |       2669 MiB |       2675 MiB |
| jujutsu  | cold |     3.05 s |     2.55 s |            4.25 s |            3.82 s |       2759 MiB |       2806 MiB |
| jujutsu  | warm |     1.27 s |     1.02 s |            1.75 s |            1.46 s |       2765 MiB |       2820 MiB |
| python   | cold |     5.33 s |     4.58 s |            7.32 s |            6.70 s |       2843 MiB |       2871 MiB |
| python   | warm |     3.30 s |     2.55 s |            4.43 s |            3.63 s |       2847 MiB |       2886 MiB |
| opencode | cold |   461.12 s |   157.45 s |          527.78 s |          219.04 s |       3037 MiB |       3528 MiB |
| opencode | warm |   453.36 s |   153.16 s |          488.65 s |          210.84 s |       3073 MiB |       3490 MiB |

Small and medium binaries gain 1.0x to 1.3x; nothing regresses in wall
time or CPU. opencode gains 2.9x cold and 3.0x warm here (the 2.4x quoted
above was against an earlier, cleaner stock run; the stock engine varies
run to run by tens of seconds on this command). The cost is memory on
the big binary: peak browser RSS on opencode is 14-16% higher, the
larger block cache and the machine code for the many more blocks that
now compile. On the small ones it is flat.

## Known problem carried over

One of four final-engine opencode runs died at 158 s with
`Segmentation fault at address 0x8` and the same bun.report hash as the
race recorded in the startup investigation, which the pinned engine hits
about one run in three. The rate is not obviously different; it is not
fixed. The new engine does not make the race worse on the evidence here,
and does not explain it.

## The product-level truth

| path                                     | opencode --version |
| ---------------------------------------- | -----------------: |
| native service (experiments/native-exec) |             0.65 s |
| native QEMU fork over 9p                 |               11 s |
| browser, this patch                      |              171 s |
| browser, pinned engine                   |              408 s |

For a CI product the browser is still the wrong place to run a big cold
binary; 2.4x does not change two orders of magnitude. The native service
already sandboxes the closure with bubblewrap and returns in under a
second. The browser engine is the zero-install demo, and this patch makes
that demo 2.4x faster on the workload that hurt most.
