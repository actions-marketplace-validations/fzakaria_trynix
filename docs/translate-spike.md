# Translating store binaries to wasm: a spike, shelved

On 2026-09-07 a day went into running nixpkgs' x86-64 binaries in the
page without a CPU emulator: translate the machine code of a store
path to WebAssembly, run it directly, and stand a Linux system-call
layer written in JavaScript in for the kernel. It works, for a
surprisingly wide set of programs, and it is not faster enough than
the QEMU guest to be worth carrying. This document is what was built,
what was measured, why it lost, what it would take to win, and the
things learned on the way that apply to the rest of trynix.

The code is 15 commits on the local bookmark `translate-spike`
(tip `83db157e`, change `rqrnywlo`), not merged and not pushed. Its
own design document, docs/translate.md on that bookmark, has the
detail this one leaves out.

## The idea

A first run in the guest was, at the time, mostly the emulator meeting
code it had never seen: the profile in performance.md puts the TCI
interpreter at up to 46% of a cold run and translation at 26%. A warm
run is still 24x to 64x off native because the wasm TCG backend keeps
every register in memory, returns to a C dispatch loop after every
block, and looks every memory access up in a software TLB.

Translating the binary itself to wasm removes all three. Each basic
block becomes a wasm function that tail-calls the next, registers are
wasm globals, and the guest's address space is one linear memory, so
a guest load is a wasm load. Store paths are immutable, so translated
modules can be cached by (store path, file, offset) and shared by
everything that maps the same glibc.

## What was built

- A table-driven decoder for x86-64 including x87, SSE through
  SSE4.2, BMI, AVX, AVX2 and FMA, checked against objdump over 12,635
  encodings.
- A translator with QEMU-style lazy flags, cmp-and-branch folding,
  position-independent modules, self-modifying-code detection through
  checksummed entry wrappers for writable mappings, x87 on f64 with
  exact remainders, and FMA through round-to-odd. Checked against this
  machine's CPU over 2,557 instruction forms by a harness that
  assembles each form, runs it natively from random states and
  compares registers, flags, vector state and memory.
- A process model: a kernel Worker owning files, pipes, the terminal,
  the process tree and signals, reached over a synchronous
  SharedArrayBuffer channel; a Worker per process and per thread;
  fork by copying memory, exec in place, clone sharing memory, futex
  as Atomics, signal frames with sigaltstack, about 110 syscalls.
- A translation cache in the Cache API, a page (`run.html`) that runs
  a closure's program directly, and a lane from the VM's shell: a
  static stub on the 9p share that hands a command to the page over
  console escape sequences, since the guest cannot write to the share.
- A benchmark suite of eight binaries run natively at build time and
  compared byte for byte against their translated output, gated in CI
  on block counts.

What ran: hello, jq, jj, python 3.14 with its stdlib and REPL, ruby
3.4, busybox sh with pipelines and scripts, Go programs (age, fzf with
threads), and the CPU probe with its AVX2 and self-modifying checks.
Every suite program reproduced its native output.

## What was measured

Headless Chromium, closure served locally. VM cold is the first run
after boot, VM warm the second. Lane cold is a first visit with an
empty translation cache, lane hot every visit after.

| program               | VM cold | VM warm | lane cold | lane hot |
| --------------------- | ------- | ------- | --------- | -------- |
| hello (C)             | 1.2 s   | 0.9 s   | 1.0 s     | 0.35 s   |
| jq --version (C)      | 0.9 s   | 0.9 s   | 1.0 s     | 0.36 s   |
| jj --version (Rust)   | 2.9 s   | 1.4 s   | 7.1 s     | 2.1 s    |
| age (Go, 2020 build)  | 1.0 s   | 0.8 s   | 3.5 s     | 2.2 s    |
| python3 -c 'print(1)' | 4.3 s   | 3.1 s   | 5.2 s     | 2.0 s    |
| ruby -e 'puts 1'      | 12.4 s  | 9.7 s   | 7.4 s     | 3.7 s    |

The node suite, which adds fzf and a busybox pipeline, tells the same
story with its counters: jj is 48,410 blocks and 15 MB of wasm, python
117,490 blocks and 17 MB, and a hot jj run spends 1.1 of its 2.4 s
compiling and instantiating 7,621 cached modules.

## Why it lost

Cold is worse almost everywhere. Translation runs in JavaScript on one
thread at about 22 µs per block and V8 then compiles every block; TCG
translates a block in microseconds and interprets it until it is hot.
The VM's cold column had also improved since the idea was formed: the
9p cache=loose mount fixed most of what made a first run slow, so the
gap the lane was meant to close was smaller than the profile that
motivated it.

Hot wins only on interpreters and small programs. The cache is per
region, a region being the code one run executed between two mapping
boundaries, so a big binary becomes thousands of modules under 100 KB.
Instantiating them costs more than compiling them, and Chrome keeps
compiled machine code only for modules over 128 KB fetched over HTTP,
so a hot visit recompiles everything from bytes.

Correctness is not guaranteed. The instruction layer is verified; the
syscall layer is written to what the programs tried needed, and an
unimplemented syscall fails a program outright rather than slowing it.
Shipping it would mean an allowlist of verified programs and a default-
off toggle, for a win of 2x on some of them.

## What it would have taken

Whole-file ahead-of-time translation: one module per store file from a
static sweep of the ELF, with the lazy path kept as a fallback for
indirect targets the sweep misses. Files are immutable and named by
hash, so those modules are static assets with a forever cache header,
buildable by nix for a curated set and publishable on GitHub Pages
next to the site, or translated on demand by a server with an LRU for
the long tail. Fetched with `compileStreaming` they are over the
128 KB threshold and V8's code cache applies, so cold becomes hot for
everyone and hot loses its 1.1 s. Estimated, not measured: jj around
0.8 to 1.0 s, python around 1.2 s, ruby around 2.5 s, against the VM's
1.4, 3.1 and 9.7 s warm. Two or three days of work, and a server or a
build step to own afterwards.

Against that, the engine's own standing candidates in performance.md
(batching blocks per module, direct chaining between blocks) improve
both the cold and the warm VM with no server and no allowlist.

## Assessments made along the way

- **CheerpX**: 32-bit x86 only, proprietary, not self-hostable. Out.
- **linux-wasm** (joelseverin): recompiles the kernel and programs to
  wasm from source; a NOMMU kernel with no fork. It is the recompile
  lane, and the multiverse is binaries, not sources.
- **A shared translation cache on exe.dev**: feasible because modules
  are position-independent and keyed by immutable store paths, but
  the server must translate files itself and never accept uploads,
  since a poisoned module executes in other people's tabs. Only
  worthwhile with whole-file modules, above.
- **Caching TCG's own blocks on exe.dev**: bounded to the interpreter
  and generation share of a cold run, none of the warm run. QEMU keys
  blocks by guest physical address, which the guest's page cache
  chooses per boot, so a shareable cache needs content-hash keys in
  the fork's translate core plus relocations for the runtime pointers
  the generated code embeds, and blocks are far too small for V8's
  code cache. Below the batching and chaining work on any list.
- **Guest hardening**: nearly everything is already off. The command
  line carries `mitigations=off`, KASLR and SMP are off, no LSMs, and
  the entropy pool is seeded by rdrand and virtio-rng. What remains
  (kernel stack protector, DEBUG_KERNEL forced by EXPERT, VMAP_STACK)
  is a percent or two, and nixpkgs' eager relocation costs about
  10 ms emulated. The fixed few hundred milliseconds per fork and
  exec in the VM is the software TLB and dispatch loop, which no
  kernel option touches.

## Learned, and still true

- V8 keeps a dispatch table per wasm instance that imports a function
  table, sized to the whole table. One instance per region made that
  quadratic and ran node out of heap; only one module may import a
  large table, and everything else should call through it.
- Browsers refuse a view of shared memory in TextDecoder,
  getRandomValues and the WebAssembly.Module constructor. Copy first.
- The kernel's termios is 36 bytes; glibc's is 60, and glibc 2.42
  asks for TCGETS2 before TCGETS. Writing glibc's size into the
  kernel's struct smashes `tcgetattr`'s stack, and the stack protector
  reports it as a crash a page away.
- Go needs a real sigaltstack and SA_ONSTACK delivery, preempts with
  SIGURG through tgkill, and a 2020 Go binary calls gettimeofday
  through the legacy vsyscall page at 0xffffffffff600000, which the
  multiverse will happily resolve `age` to.
- The engine's 9p backend refuses every create from the guest with
  EPERM, so a page cannot leave files for the guest on the share; the
  console is the only channel, and OSC escape sequences the page
  strips before the terminal draws them work as one.
- A semantics harness that runs native code must save callee-saved
  registers, run fninit and reset mxcsr between forms, keep 32-byte
  alignment for ymm, and never push on the stack it tests with.
- busybox's `time` writes to stderr, and a marker echoed in the typed
  command line matches before the command runs. Measure with the
  command's own output redirected, not the shell's, and with markers
  the typed line cannot contain.
- The x86run flake app had never worked through `nix run`: a single
  file copied into the store cannot import its siblings. Apps that
  import from the tree must run from `${self}`.
