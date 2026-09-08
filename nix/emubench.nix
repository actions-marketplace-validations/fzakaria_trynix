# emubench: a per-instruction-class throughput benchmark for the CPU
# emulator, built the same way as the correctness probe (nix/probe.nix).
#
# Where cputest.c asks whether the emulator computes the right answer,
# emubench.c asks how fast it computes, one instruction shape at a time.
# Each test is a fixed block of inline assembly with a known translation-
# block layout, so the ratio between two engines (native TCG vs the
# browser wasm backend) names a mechanism rather than a workload: a hot
# single block, a two-block loop that pays one block-boundary transition,
# a call/ret, a computed jump, TLB traffic, guest syscalls, page faults,
# and cold straight-line code run pass by pass. docs/engine-execution.md
# reads the numbers.
#
# Statically linked against musl so the closure is one store path, which
# is what lets it be served from a binary cache beside the page and run
# in the browser guest with no network, exactly like the probe.
{ pkgs }:
pkgs.pkgsStatic.stdenv.mkDerivation {
  pname = "trynix-emubench";
  version = "1";

  dontUnpack = true;

  nativeBuildInputs = [ pkgs.python3 ];

  # -march=haswell to match nix/guest/machine.json, so the compiler may
  # emit the instructions that model promises. gen_cold.py writes the two
  # large straight-line functions (50000 and 2000 blocks) the cold tests
  # walk; -mno-red-zone keeps the hand-written asm honest.
  buildPhase = ''
    runHook preBuild
    python3 ${./emubench/gen_cold.py} cold_50k 50000 > cold_50k.S
    python3 ${./emubench/gen_cold.py} cold_2k 2000 > cold_2k.S
    $CC -O2 -static -march=haswell -mno-red-zone -o emubench \
      ${./emubench/emubench.c} cold_50k.S cold_2k.S
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 emubench $out/bin/emubench
    runHook postInstall
  '';
}
