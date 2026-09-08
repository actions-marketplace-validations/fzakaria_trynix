// emubench: time the emulator's distinct execution paths one at a time.
// Each test is inline assembly with a known translation-block shape, so a
// ratio between two engines names a mechanism rather than a workload.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

// Generated straight-line code (gen_cold.py): N blocks of 7 ALU ops + jmp.
extern uint64_t cold_50k(uint64_t, uint64_t);   // 50,000 blocks
extern uint64_t cold_2k(uint64_t, uint64_t);    // 2,000 blocks

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static volatile uint64_t sink;

static void report(const char *name, uint64_t iters, uint64_t insns_per_iter, uint64_t ns) {
    double per = iters ? (double)ns / iters : 0;
    double mips = ns ? (double)iters * insns_per_iter / ns * 1000.0 : 0;
    printf("emubench: %-10s iters=%llu ns=%llu ns/iter=%.1f mips=%.1f\n",
           name, (unsigned long long)iters, (unsigned long long)ns, per, mips);
    fflush(stdout);
}

// One block looping on itself: pure generated-code quality.
static void t_alu(uint64_t n) {
    uint64_t a = 1, b = 2, c = n;
    uint64_t t0 = now_ns();
    __asm__ volatile(
        "1:\n\t"
        "add %%rbx, %%rax\n\t"
        "xor %%rax, %%rbx\n\t"
        "lea 1(%%rax,%%rbx), %%rax\n\t"
        "dec %%rcx\n\t"
        "jnz 1b\n\t"
        : "+a"(a), "+b"(b), "+c"(c) :: "cc");
    sink = a + b;
    report("alu", n, 5, now_ns() - t0);
}

// Two blocks joined by a direct jump: the cost of one chained transition.
static void t_alu2(uint64_t n) {
    uint64_t a = 1, b = 2, c = n;
    uint64_t t0 = now_ns();
    __asm__ volatile(
        "1:\n\t"
        "add %%rbx, %%rax\n\t"
        "xor %%rax, %%rbx\n\t"
        "jmp 2f\n\t"
        ".p2align 4\n\t"
        "2:\n\t"
        "lea 1(%%rax,%%rbx), %%rax\n\t"
        "dec %%rcx\n\t"
        "jnz 1b\n\t"
        : "+a"(a), "+b"(b), "+c"(c) :: "cc");
    sink = a + b;
    report("alu2", n, 6, now_ns() - t0);
}

// Four blocks per iteration, same work, to check the transition cost scales.
static void t_alu4(uint64_t n) {
    uint64_t a = 1, b = 2, c = n;
    uint64_t t0 = now_ns();
    __asm__ volatile(
        "1:\n\t"
        "add %%rbx, %%rax\n\t"
        "jmp 2f\n\t"
        ".p2align 4\n\t"
        "2:\n\t"
        "xor %%rax, %%rbx\n\t"
        "jmp 3f\n\t"
        ".p2align 4\n\t"
        "3:\n\t"
        "lea 1(%%rax,%%rbx), %%rax\n\t"
        "jmp 4f\n\t"
        ".p2align 4\n\t"
        "4:\n\t"
        "dec %%rcx\n\t"
        "jnz 1b\n\t"
        : "+a"(a), "+b"(b), "+c"(c) :: "cc");
    sink = a + b;
    report("alu4", n, 8, now_ns() - t0);
}

// call/ret: one indirect jump (ret) per iteration through the TB lookup.
static void t_call(uint64_t n) {
    uint64_t a = 1, b = 2, c = n;
    uint64_t t0 = now_ns();
    __asm__ volatile(
        "1:\n\t"
        "call 2f\n\t"
        "dec %%rcx\n\t"
        "jnz 1b\n\t"
        "jmp 3f\n\t"
        ".p2align 4\n\t"
        "2:\n\t"
        "add %%rbx, %%rax\n\t"
        "xor %%rax, %%rbx\n\t"
        "ret\n\t"
        "3:\n\t"
        : "+a"(a), "+b"(b), "+c"(c) :: "cc", "memory");
    sink = a + b;
    report("call", n, 6, now_ns() - t0);
}

// Computed jump through a table with a varying target: interpreter dispatch.
static void t_indirect(uint64_t n) {
    static void *table[8];
    void **tbl = table;
    uint64_t a = 1, idx = 7, c = n;
    uint64_t t0 = now_ns();
    __asm__ volatile(
        "lea 10f(%%rip), %%rax\n\t" "mov %%rax, 0(%[t])\n\t"
        "lea 11f(%%rip), %%rax\n\t" "mov %%rax, 8(%[t])\n\t"
        "lea 12f(%%rip), %%rax\n\t" "mov %%rax, 16(%[t])\n\t"
        "lea 13f(%%rip), %%rax\n\t" "mov %%rax, 24(%[t])\n\t"
        "lea 14f(%%rip), %%rax\n\t" "mov %%rax, 32(%[t])\n\t"
        "lea 15f(%%rip), %%rax\n\t" "mov %%rax, 40(%[t])\n\t"
        "lea 16f(%%rip), %%rax\n\t" "mov %%rax, 48(%[t])\n\t"
        "lea 17f(%%rip), %%rax\n\t" "mov %%rax, 56(%[t])\n\t"
        "jmp 20f\n\t"
        "10: add $1, %%rax\n\t" "jmp 1f\n\t"
        "11: add $2, %%rax\n\t" "jmp 1f\n\t"
        "12: add $3, %%rax\n\t" "jmp 1f\n\t"
        "13: add $4, %%rax\n\t" "jmp 1f\n\t"
        "14: add $5, %%rax\n\t" "jmp 1f\n\t"
        "15: add $6, %%rax\n\t" "jmp 1f\n\t"
        "16: add $7, %%rax\n\t" "jmp 1f\n\t"
        "17: add $8, %%rax\n\t" "jmp 1f\n\t"
        "20:\n\t"
        "mov $1, %%rax\n\t"
        "1:\n\t"
        "dec %%rcx\n\t"
        "jz 2f\n\t"
        "imul $1103515245, %%rdx, %%rdx\n\t"
        "add $12345, %%rdx\n\t"
        "mov %%rdx, %%rbx\n\t"
        "shr $16, %%rbx\n\t"
        "and $7, %%rbx\n\t"
        "jmp *(%[t],%%rbx,8)\n\t"
        "2:\n\t"
        : "+a"(a), "+d"(idx), "+c"(c) : [t] "r"(tbl) : "rbx", "cc", "memory");
    sink = a;
    report("indirect", n, 10, now_ns() - t0);
}

// Loads and a read-modify-write on a small buffer: the TLB fast path.
static void t_mem(uint64_t n) {
    static uint64_t src[8192], dst[8192];
    uint64_t a = 0, i = 0, c = n;
    uint64_t t0 = now_ns();
    __asm__ volatile(
        "1:\n\t"
        "mov (%[s],%%rdx,8), %%rax\n\t"
        "add %%rax, (%[d],%%rdx,8)\n\t"
        "add $1, %%rdx\n\t"
        "and $8191, %%rdx\n\t"
        "dec %%rcx\n\t"
        "jnz 1b\n\t"
        : "+a"(a), "+d"(i), "+c"(c) : [s] "r"(src), [d] "r"(dst) : "cc", "memory");
    sink = a + dst[7];
    report("mem", n, 6, now_ns() - t0);
}

// Strided loads across many pages: the softmmu slow path and page walks.
static void t_memstride(uint64_t n) {
    size_t size = 64u << 20;
    uint8_t *buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) { perror("mmap"); exit(1); }
    memset(buf, 1, size);
    uint64_t a = 0, off = 0, c = n;
    uint64_t t0 = now_ns();
    __asm__ volatile(
        "1:\n\t"
        "movzbq (%[b],%%rdx), %%rbx\n\t"
        "add %%rbx, %%rax\n\t"
        "add $4160, %%rdx\n\t"
        "and $67108863, %%rdx\n\t"
        "dec %%rcx\n\t"
        "jnz 1b\n\t"
        : "+a"(a), "+d"(off), "+c"(c) : [b] "r"(buf) : "rbx", "cc", "memory");
    sink = a;
    report("memstride", n, 6, now_ns() - t0);
    munmap(buf, size);
}

// getpid: the guest kernel's syscall entry and exit.
static void t_syscall(uint64_t n) {
    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < n; i++) {
        sink = syscall(SYS_getpid);
    }
    report("syscall", n, 1, now_ns() - t0);
}

// Touch fresh anonymous pages: page faults and page-table work in the guest kernel.
static void t_pagefault(uint64_t n) {
    size_t size = (size_t)n * 4096;
    uint64_t t0 = now_ns();
    uint8_t *buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) { perror("mmap"); exit(1); }
    for (size_t i = 0; i < size; i += 4096) {
        buf[i] = 1;
    }
    munmap(buf, size);
    report("pagefault", n, 1, now_ns() - t0);
}

// Straight-line code run pass by pass: pass 1 pays translation, later
// passes show what an engine does with code below its compile threshold.
static void t_cold(const char *name, uint64_t (*fn)(uint64_t, uint64_t), uint64_t blocks, int passes) {
    char label[32];
    uint64_t acc = 1;
    for (int p = 1; p <= passes; p++) {
        uint64_t t0 = now_ns();
        acc = fn(acc, p);
        uint64_t ns = now_ns() - t0;
        if (p <= 4 || p == passes || p % 100 == 0) {
            snprintf(label, sizeof label, "%s/p%d", name, p);
            report(label, blocks, 8, ns);
        }
    }
    sink = acc;
}

int main(int argc, char **argv) {
    const char *which = argc > 1 ? argv[1] : "all";
    uint64_t scale = argc > 2 ? strtoull(argv[2], NULL, 10) : 1;
    int all = strcmp(which, "all") == 0;
#define WANT(x) (all || strcmp(which, x) == 0)
    if (WANT("alu")) t_alu(20000000 * scale);
    if (WANT("alu2")) t_alu2(10000000 * scale);
    if (WANT("alu4")) t_alu4(5000000 * scale);
    if (WANT("call")) t_call(5000000 * scale);
    if (WANT("indirect")) t_indirect(5000000 * scale);
    if (WANT("mem")) t_mem(10000000 * scale);
    if (WANT("memstride")) t_memstride(2000000 * scale);
    if (WANT("syscall")) t_syscall(200000 * scale);
    if (WANT("pagefault")) t_pagefault(16384 * scale);
    if (WANT("cold")) t_cold("cold50k", cold_50k, 50000, 3);
    if (WANT("warm")) t_cold("cold2k", cold_2k, 2000, 1600);
    printf("emubench: done\n");
    fflush(stdout);
    return 0;
}
