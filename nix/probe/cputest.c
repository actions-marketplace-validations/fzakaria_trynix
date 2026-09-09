/*
 * Check the scalar instructions a Haswell guest executes that a baseline
 * one cannot, each against a plain-C implementation of the same
 * operation. Run in the guest, this says whether the emulator's JIT
 * computes what the hardware would.
 *
 * The engine is a JIT, and a JIT can be wrong about an instruction
 * rather than merely slow at it. POPCNT was: the wasm backend read the
 * operands of `ctpop` one slot along, so the instruction answered with
 * whatever its destination register already held. Nothing in the guest
 * image emits these instructions, so the VM booted to a shell either
 * way and every other check in the tree passed.
 *
 * Scope is x86-64-v2 and v3 scalar, which is what raising the CPU model
 * in nix/guest/machine.json newly reached, plus the older arithmetic a
 * JIT is equally free to get wrong: the flag-carrying and conditional
 * instructions compilers reach for constantly. The vector half is left
 * out: TCG lowers most of it through helpers shared with every backend,
 * rather than through the per-op code that was wrong here.
 *
 * Worth knowing while reading this: the engine has TWO implementations
 * of every opcode. A translation block is interpreted by TCI for its
 * first 1500 executions and compiled to WebAssembly after that, and a
 * compiled block falls back to the interpreter when the engine runs out
 * of instances. The two can disagree -- ctpop did, TCI being the correct
 * one -- so an instruction is only really checked once it has run both
 * ways. Hence REPEATS below.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <x86intrin.h>

/* Enough passes to carry each block past the engine's compile threshold
 * (INSTANTIATE_NUM, 1500) so the compiled implementation is exercised
 * and not just the interpreter. */
#define REPEATS 25

/* A failing instruction fails on most inputs and on every pass, so the
 * first handful of lines carry the diagnosis and the rest are noise.
 * Counting continues past the cap. */
#define MAX_REPORTS 40

static int failures = 0;

/* Report one disagreement. The input is printed because which inputs
 * fail is the first clue about what the emulator got wrong -- a single
 * bad answer for every input reads very differently from one that only
 * fails on zero. */
static void check(const char *what, uint64_t input, uint64_t got, uint64_t want)
{
    if (got == want) {
        return;
    }
    failures++;
    if (failures <= MAX_REPORTS) {
        printf("FAIL %-12s in=0x%016llx got=0x%llx want=0x%llx\n", what,
               (unsigned long long)input, (unsigned long long)got,
               (unsigned long long)want);
    }
}

/* Reference implementations: plain C, no instruction newer than the
 * baseline the compiler is told to target for this file. */

static uint64_t ref_ctz(uint64_t x)
{
    if (x == 0) {
        return 64;
    }
    uint64_t n = 0;
    while ((x & 1) == 0) {
        x >>= 1;
        n++;
    }
    return n;
}

static uint64_t ref_clz(uint64_t x)
{
    if (x == 0) {
        return 64;
    }
    uint64_t n = 0;
    while ((x & (1ULL << 63)) == 0) {
        x <<= 1;
        n++;
    }
    return n;
}

static uint64_t ref_popcount(uint64_t x)
{
    uint64_t n = 0;
    for (; x != 0; x >>= 1) {
        n += (x & 1);
    }
    return n;
}

/* Deposit the low bits of `x` into the set bit positions of `mask`. */
static uint64_t ref_pdep(uint64_t x, uint64_t mask)
{
    uint64_t result = 0;
    for (int bit = 0; bit < 64; bit++) {
        if ((mask & (1ULL << bit)) == 0) {
            continue;
        }
        if (x & 1) {
            result |= (1ULL << bit);
        }
        x >>= 1;
    }
    return result;
}

/* Gather the bits of `x` at the set positions of `mask` into the low
 * bits of the result: the inverse of ref_pdep. */
static uint64_t ref_pext(uint64_t x, uint64_t mask)
{
    uint64_t result = 0;
    int out = 0;
    for (int bit = 0; bit < 64; bit++) {
        if ((mask & (1ULL << bit)) == 0) {
            continue;
        }
        if (x & (1ULL << bit)) {
            result |= (1ULL << out);
        }
        out++;
    }
    return result;
}

/*
 * The instructions themselves. Written as inline asm rather than as
 * builtins so that the file tests the instruction the name promises,
 * whatever the compiler would have chosen on its own.
 */

#define UNARY(name, mnemonic)                                                 \
    static uint64_t name(uint64_t x)                                          \
    {                                                                         \
        uint64_t r;                                                           \
        __asm__ volatile(mnemonic "\t%1, %0" : "=r"(r) : "r"(x) : "cc");                \
        return r;                                                             \
    }

#define BINARY(name, mnemonic)                                                \
    static uint64_t name(uint64_t a, uint64_t b)                              \
    {                                                                         \
        uint64_t r;                                                           \
        __asm__ volatile(mnemonic "\t%2, %1, %0" : "=r"(r) : "r"(a), "r"(b) : "cc");     \
        return r;                                                             \
    }

UNARY(asm_popcnt, "popcnt")
UNARY(asm_tzcnt, "tzcnt")
UNARY(asm_lzcnt, "lzcnt")
UNARY(asm_blsi, "blsi")
UNARY(asm_blsr, "blsr")
UNARY(asm_blsmsk, "blsmsk")

BINARY(asm_andn, "andn")
BINARY(asm_bextr, "bextr")
BINARY(asm_bzhi, "bzhi")
BINARY(asm_shlx, "shlx")
BINARY(asm_shrx, "shrx")
BINARY(asm_sarx, "sarx")

/*
 * tzcnt and lzcnt set the carry flag when the source is zero, and that
 * flag is a separate result from the count. mimalloc reads exactly this
 * to answer "did this word have any bit set at all", so a backend that
 * gets the count right and the flag wrong still breaks an allocator.
 */
static uint64_t asm_tzcnt_carry(uint64_t x, uint64_t *idx)
{
    uint64_t is_zero;
    __asm__ volatile("tzcnt\t%2, %1" : "=@ccc"(is_zero), "=r"(*idx) : "r"(x) : "cc");
    return is_zero;
}

static uint64_t asm_lzcnt_carry(uint64_t x, uint64_t *idx)
{
    uint64_t is_zero;
    __asm__ volatile("lzcnt\t%2, %1" : "=@ccc"(is_zero), "=r"(*idx) : "r"(x) : "cc");
    return is_zero;
}

/* popcnt sets the zero flag when the source is zero, and clears every
 * other flag. */
static uint64_t asm_popcnt_zero(uint64_t x, uint64_t *count)
{
    uint64_t is_zero;
    __asm__ volatile("popcnt\t%2, %1" : "=@ccz"(is_zero), "=r"(*count) : "r"(x) : "cc");
    return is_zero;
}

static uint64_t asm_rorx(uint64_t x)
{
    uint64_t r;
    __asm__ volatile("rorx\t$13, %1, %0" : "=r"(r) : "r"(x));
    return r;
}

/* mulx returns the full 128-bit product in two registers and touches no
 * flags. */
static uint64_t asm_mulx(uint64_t a, uint64_t b, uint64_t *high)
{
    uint64_t low;
    __asm__ volatile("mulx\t%3, %0, %1" : "=r"(low), "=r"(*high) : "d"(a), "r"(b));
    return low;
}

static uint64_t asm_pdep(uint64_t x, uint64_t mask)
{
    uint64_t r;
    __asm__ volatile("pdep\t%2, %1, %0" : "=r"(r) : "r"(x), "r"(mask));
    return r;
}

static uint64_t asm_pext(uint64_t x, uint64_t mask)
{
    uint64_t r;
    __asm__ volatile("pext\t%2, %1, %0" : "=r"(r) : "r"(x), "r"(mask));
    return r;
}

/* movbe loads and stores byte-reversed, and only between a register and
 * memory, so it needs a real address rather than a register. */
static uint64_t asm_movbe_load(const uint64_t *from)
{
    uint64_t r;
    __asm__ volatile("movbe\t%1, %0" : "=r"(r) : "m"(*from));
    return r;
}

static uint64_t ref_bswap(uint64_t x)
{
    uint64_t r = 0;
    for (int byte = 0; byte < 8; byte++) {
        r = (r << 8) | ((x >> (byte * 8)) & 0xFF);
    }
    return r;
}


/*
 * The older arithmetic, which a JIT is no less free to get wrong and
 * which compilers emit far more of than they do BMI. These carry a flag
 * in or out, or choose between values on one -- the shapes where a
 * backend has two things to get right rather than one.
 */

/* Compare-and-swap: the instruction every lock-free structure is built
 * on, and the one whose failure looks like memory corruption rather than
 * arithmetic. Returns the zero flag, which says whether the swap took. */
static int asm_cmpxchg64(uint64_t *cell, uint64_t expected, uint64_t desired,
                         uint64_t *seen)
{
    unsigned char took;
    __asm__ volatile("lock cmpxchgq %[des], %[mem]"
                     : "+a"(expected), [mem] "+m"(*cell), "=@ccz"(took)
                     : [des] "r"(desired)
                     : "cc", "memory");
    *seen = expected;
    return took;
}

/* The 128-bit form, which x86-64-v2 is what makes available at all. */
static int asm_cmpxchg16b(__int128 *cell, __int128 expected, __int128 desired)
{
    return __sync_bool_compare_and_swap(cell, expected, desired);
}

/* Fetch-and-add, and the unconditional swap. */
static uint64_t asm_xadd(uint64_t *cell, uint64_t addend)
{
    __asm__ volatile("lock xaddq %[val], %[mem]"
                     : [val] "+r"(addend), [mem] "+m"(*cell)
                     :
                     : "cc", "memory");
    return addend;
}

static uint64_t asm_xchg(uint64_t *cell, uint64_t value)
{
    __asm__ volatile("xchgq %[val], %[mem]"
                     : [val] "+r"(value), [mem] "+m"(*cell)
                     :
                     : "memory");
    return value;
}

/* The bit-test family: the answer is a flag, and three of the four also
 * write the word back. */
static int asm_bt(uint64_t x, uint64_t bit)
{
    unsigned char c;
    __asm__ volatile("btq %[b], %[v]" : "=@ccc"(c) : [v] "r"(x), [b] "r"(bit) : "cc");
    return c;
}

static uint64_t asm_bts(uint64_t x, uint64_t bit, int *was_set)
{
    unsigned char c;
    __asm__ volatile("btsq %[b], %[v]"
            : [v] "+r"(x), "=@ccc"(c)
            : [b] "r"(bit)
            : "cc");
    *was_set = c;
    return x;
}

static uint64_t asm_btr(uint64_t x, uint64_t bit)
{
    __asm__ volatile("btrq %[b], %[v]" : [v] "+r"(x) : [b] "r"(bit) : "cc");
    return x;
}

static uint64_t asm_btc(uint64_t x, uint64_t bit)
{
    __asm__ volatile("btcq %[b], %[v]" : [v] "+r"(x) : [b] "r"(bit) : "cc");
    return x;
}

/* Add and subtract carrying a flag between words: how every bignum and
 * every 128-bit add is built. */
static uint64_t asm_adc(uint64_t a, uint64_t b, int carry_in, int *carry_out)
{
    unsigned long long sum;
    *carry_out = _addcarry_u64((unsigned char)carry_in, a, b, &sum);
    return sum;
}

static uint64_t asm_sbb(uint64_t a, uint64_t b, int borrow_in, int *borrow_out)
{
    unsigned long long diff;
    *borrow_out = _subborrow_u64((unsigned char)borrow_in, a, b, &diff);
    return diff;
}

/* Choosing a value on a flag rather than branching on it. */
static uint64_t asm_cmovg(uint64_t a, uint64_t b, uint64_t x, uint64_t y)
{
    uint64_t r = y;
    __asm__ volatile("cmpq %[b], %[a]\n\tcmovgq %[x], %[r]"
            : [r] "+r"(r)
            : [a] "r"(a), [b] "r"(b), [x] "r"(x)
            : "cc");
    return r;
}

static uint64_t asm_setb(uint64_t a, uint64_t b)
{
    unsigned char r;
    __asm__ volatile("cmpq %[b], %[a]\n\tsetb %[r]"
            : [r] "=r"(r)
            : [a] "r"(a), [b] "r"(b)
            : "cc");
    return r;
}

/* The double-width shifts, which read one register and shift bits in
 * from another. */
static uint64_t asm_shld(uint64_t high, uint64_t low, int count)
{
    __asm__ volatile("shldq %%cl, %[lo], %[hi]"
            : [hi] "+r"(high)
            : [lo] "r"(low), "c"((unsigned char)count)
            : "cc");
    return high;
}

static uint64_t asm_shrd(uint64_t low, uint64_t high, int count)
{
    __asm__ volatile("shrdq %%cl, %[hi], %[lo]"
            : [lo] "+r"(low)
            : [hi] "r"(high), "c"((unsigned char)count)
            : "cc");
    return low;
}

static uint64_t asm_bswap(uint64_t x)
{
    __asm__ volatile("bswapq %0" : "+r"(x));
    return x;
}

/* The one-operand multiply, whose two halves land in fixed registers --
 * a different code path from mulx, which names its own. */
static uint64_t asm_mul_full(uint64_t a, uint64_t b, uint64_t *high)
{
    uint64_t low;
    __asm__ volatile("mulq %[b]" : "=a"(low), "=d"(*high) : "a"(a), [b] "r"(b) : "cc");
    return low;
}

/* The signed one-operand multiply: the 128-bit signed product, high
 * half in rdx. The emulator computes that half separately from the
 * unsigned one, so mul passing says nothing about this. */
static uint64_t asm_imul_full(uint64_t a, uint64_t b, uint64_t *high)
{
    uint64_t low;
    __asm__ volatile("imulq %[b]" : "=a"(low), "=d"(*high) : "a"(a), [b] "r"(b) : "cc");
    return low;
}

/* The two-operand signed multiply keeps the low half and reports in OF
 * whether the high half was anything but the sign extension of it; the
 * emulator derives that flag from the same high half. */
static uint64_t asm_imul_overflow(uint64_t a, uint64_t b, uint64_t *overflow)
{
    unsigned char of;
    __asm__ volatile("imulq %[b], %[a]\n\tseto %[of]"
                     : [a] "+r"(a), [of] "=q"(of)
                     : [b] "r"(b)
                     : "cc");
    *overflow = of;
    return a;
}

/* And the divide, which reads a 128-bit dividend from the same pair.
 * The high word is kept below the divisor so the quotient fits and the
 * CPU does not fault instead of answering. */
static uint64_t asm_div_full(uint64_t high, uint64_t low, uint64_t divisor,
                             uint64_t *remainder)
{
    uint64_t quotient;
    __asm__ volatile("divq %[d]"
            : "=a"(quotient), "=d"(*remainder)
            : "a"(low), "d"(high), [d] "r"(divisor)
            : "cc");
    return quotient;
}

/* CRC32C, the SSE4.2 checksum, which hash tables reach for. */
static uint64_t asm_crc32(uint64_t crc, uint64_t value)
{
    __asm__ volatile("crc32q %[v], %[c]" : [c] "+r"(crc) : [v] "r"(value));
    return crc;
}

/* Castagnoli, reflected: the polynomial the instruction implements. The
 * instruction is a raw update of the running value -- it does not invert
 * going in or coming out, the way a complete CRC32C of a message does --
 * so neither does this. */
static uint64_t ref_crc32c(uint64_t crc, uint64_t value)
{
    for (int byte = 0; byte < 8; byte++) {
        crc ^= (value >> (byte * 8)) & 0xFF;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0x82F63B78ULL) : (crc >> 1);
        }
    }
    return crc & 0xFFFFFFFFULL;
}


/*
 * Self-modifying code: what a JavaScript engine does all day and a
 * static binary never does at all.
 *
 * The emulator caches a translation of every block of guest code, and
 * the wasm engine goes further and compiles hot blocks into their own
 * WebAssembly module. Both caches have to be thrown away the moment the
 * guest writes over the instructions they came from. x86 asks for no
 * cache flush to make a rewrite take effect, so nothing in the guest
 * announces the change: the emulator has to notice the write itself.
 *
 * Miss that and a stale translation keeps running after the code under
 * it has changed -- rarely, because it needs the block to have been
 * compiled first, and only in a program that rewrites its own code.
 * Nothing else in this file can see that: every other check runs
 * instructions that were in the binary when it was linked.
 */

/* A stub is `mov eax, <value>; ret`, which is all this needs to tell one
 * generation of the code from the next. */
#define STUB_BYTES 6
#define STUB_OPCODE_MOV_EAX 0xB8
#define STUB_OPCODE_RET 0xC3

/* Past the engine's compile threshold, so each generation of the stub is
 * running as compiled WebAssembly before it is overwritten. */
#define STUB_HOT 1700
#define STUB_ROUNDS 40

typedef uint32_t (*stub_fn)(void);

static void write_stub(unsigned char *at, uint32_t value)
{
    at[0] = STUB_OPCODE_MOV_EAX;
    memcpy(at + 1, &value, sizeof(value));
    at[5] = STUB_OPCODE_RET;
}

/* Call it hot enough to be compiled, checking every answer. */
static void run_stub(const char *what, unsigned char *at, uint32_t want)
{
    stub_fn call = (stub_fn)(void *)at;
    for (int i = 0; i < STUB_HOT; i++) {
        check(what, want, call(), want);
    }
}

static void check_self_modifying_code(void)
{
    const size_t page = 4096;
    unsigned char *code = mmap(NULL, page, PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        printf("FAIL smc.mmap  could not map a writable, executable page\n");
        failures++;
        return;
    }

    /* Rewrite the same address over and over. Each generation is made
     * hot before the next overwrites it, so every round asks whether a
     * compiled block survived the code it came from being replaced. */
    for (uint32_t round = 0; round < STUB_ROUNDS; round++) {
        const uint32_t value = 0x5A5A0000u + round;
        write_stub(code, value);
        run_stub("smc.rewrite", code, value);
    }

    /* Two stubs sharing a page: rewriting one must not disturb the
     * other, and must not take the other's translation with it. */
    unsigned char *first = code;
    unsigned char *second = code + 64;
    write_stub(first, 0x11110000u);
    write_stub(second, 0x22220000u);
    run_stub("smc.first", first, 0x11110000u);
    run_stub("smc.second", second, 0x22220000u);
    for (uint32_t round = 0; round < STUB_ROUNDS; round++) {
        write_stub(first, 0x33330000u + round);
        run_stub("smc.rewritten", first, 0x33330000u + round);
        run_stub("smc.neighbour", second, 0x22220000u);
    }

    /* And the narrowest case: patch only the four immediate bytes, in
     * the middle of a block the emulator has already translated. This is
     * what a JIT does when it backpatches a constant, and it is the
     * write most easily missed, since the instruction boundaries do not
     * move and only the operand changes. */
    write_stub(code, 0x44440000u);
    run_stub("smc.patched", code, 0x44440000u);
    for (uint32_t round = 0; round < STUB_ROUNDS; round++) {
        const uint32_t value = 0x44440000u + round + 1;
        memcpy(code + 1, &value, sizeof(value));
        run_stub("smc.immediate", code, value);
    }

    munmap(code, page);
}

int main(void)
{
    /* Zero, every single-bit value, all ones, and a few values this
     * emulator has reason to compute on: a page size, an allocator's
     * reservation sizes, and a pattern with no symmetry to hide a
     * byte-order or shift mistake. */
    uint64_t inputs[72];
    size_t count = 0;
    inputs[count++] = 0;
    for (int bit = 0; bit < 64; bit++) {
        inputs[count++] = 1ULL << bit;
    }
    inputs[count++] = 0xFFFFFFFFFFFFFFFFULL;
    inputs[count++] = 0x0000000000001000ULL;
    inputs[count++] = 0x0000000000210000ULL;
    inputs[count++] = 0x0000000040000000ULL;
    inputs[count++] = 0x123456789ABCDEF0ULL;
    inputs[count++] = 0x8000000000000001ULL;
    inputs[count++] = 0xF0F0F0F0F0F0F0F0ULL;

    const uint64_t other = 0xF0F0F0F0F0F0F0F0ULL;

    /* Everything below runs REPEATS times. One pass checks that each
     * instruction is right; the repetition is what carries each block
     * past the engine's compile threshold, so the answer is checked
     * against both of the engine's implementations rather than only the
     * interpreter it starts in. */
    for (int pass = 0; pass < REPEATS; pass++) {
    for (size_t i = 0; i < count; i++) {
        uint64_t x = inputs[i];

        /* Counts. tzcnt and lzcnt are defined at zero on x86-64: both
         * answer 64, unlike the bsf/bsr they replace. */
        check("popcnt", x, asm_popcnt(x), ref_popcount(x));
        check("tzcnt", x, asm_tzcnt(x), ref_ctz(x));
        check("lzcnt", x, asm_lzcnt(x), ref_clz(x));

        /* The flags those three set, which are results in their own
         * right and are set from a different part of a backend. */
        uint64_t idx = 0;
        check("tzcnt.cf", x, asm_tzcnt_carry(x, &idx), (x == 0) ? 1 : 0);
        check("tzcnt.cf.idx", x, idx, ref_ctz(x));
        check("lzcnt.cf", x, asm_lzcnt_carry(x, &idx), (x == 0) ? 1 : 0);
        check("lzcnt.cf.idx", x, idx, ref_clz(x));
        check("popcnt.zf", x, asm_popcnt_zero(x, &idx), (x == 0) ? 1 : 0);
        check("popcnt.zf.n", x, idx, ref_popcount(x));

        /* BMI1's single-bit manipulations. */
        check("andn", x, asm_andn(x, other), (~x) & other);
        check("blsi", x, asm_blsi(x), x & (~x + 1));
        check("blsr", x, asm_blsr(x), x & (x - 1));
        check("blsmsk", x, asm_blsmsk(x), x ^ (x - 1));

        /* bextr takes start in the low byte of its control operand and
         * length in the next: here bits 8..39 of the source. */
        check("bextr", x, asm_bextr(x, 0x2008), (x >> 8) & 0xFFFFFFFFULL);

        /* BMI2's shifts, which unlike the classic ones do not touch
         * flags and take the count from any register. */
        check("shlx", x, asm_shlx(x, 5), x << 5);
        check("shrx", x, asm_shrx(x, 5), x >> 5);
        check("sarx", x, asm_sarx(x, 5), (uint64_t)(((int64_t)x) >> 5));
        check("rorx", x, asm_rorx(x), (x >> 13) | (x << 51));

        /* The full-width multiply, both halves. */
        uint64_t high = 0;
        uint64_t low = asm_mulx(x, other, &high);
        __uint128_t product = (__uint128_t)x * (__uint128_t)other;
        check("mulx.lo", x, low, (uint64_t)product);
        check("mulx.hi", x, high, (uint64_t)(product >> 64));

        /* The bit scatter/gather pair, the two most intricate things a
         * backend has to get right here. */
        check("pdep", x, asm_pdep(x, other), ref_pdep(x, other));
        check("pext", x, asm_pext(x, other), ref_pext(x, other));

        /* And the byte-reversing load. */
        check("movbe", x, asm_movbe_load(&x), ref_bswap(x));
    }

    /* The rest, over the same inputs. These are checked in their own
     * pass so a failure names which family broke rather than burying it
     * among the counts. */
    for (size_t i = 0; i < count; i++) {
        uint64_t x = inputs[i];
        const uint64_t bit = i % 64;

        /* Compare-and-swap, both outcomes: one where the cell holds what
         * was expected and the swap takes, one where it does not. */
        uint64_t cell = x;
        uint64_t seen = 0;
        check("cmpxchg.hit", x, asm_cmpxchg64(&cell, x, other, &seen), 1);
        check("cmpxchg.new", x, cell, other);
        check("cmpxchg.seen", x, seen, x);

        cell = x;
        check("cmpxchg.miss", x, asm_cmpxchg64(&cell, ~x, other, &seen), 0);
        check("cmpxchg.kept", x, cell, x);
        check("cmpxchg.read", x, seen, x);

        __int128 wide = (__int128)x << 64 | other;
        check("cmpxchg16b", x, asm_cmpxchg16b(&wide, wide, 0), 1);
        check("cmpxchg16b.no", x, asm_cmpxchg16b(&wide, ~(__int128)0, wide), 0);

        cell = x;
        check("xadd", x, asm_xadd(&cell, other), x);
        check("xadd.sum", x, cell, x + other);

        cell = x;
        check("xchg", x, asm_xchg(&cell, other), x);
        check("xchg.new", x, cell, other);

        /* The bit-test family, against a shift and a mask. */
        check("bt", x, (uint64_t)asm_bt(x, bit), (x >> bit) & 1);
        int was_set = 0;
        check("bts", x, asm_bts(x, bit, &was_set), x | (1ULL << bit));
        check("bts.cf", x, (uint64_t)was_set, (x >> bit) & 1);
        check("btr", x, asm_btr(x, bit), x & ~(1ULL << bit));
        check("btc", x, asm_btc(x, bit), x ^ (1ULL << bit));

        /* Carry in and carry out, both directions. */
        int carry = 0;
        check("adc", x, asm_adc(x, other, 1, &carry), x + other + 1);
        check("adc.cf", x, (uint64_t)carry,
              (x + other + 1 < x) || (other == ~0ULL));
        check("sbb", x, asm_sbb(x, other, 1, &carry), x - other - 1);
        check("sbb.cf", x, (uint64_t)carry, (x < other + 1) || (other == ~0ULL));

        /* Conditional move and conditional set, signed and unsigned. */
        check("cmovg", x, asm_cmovg(x, other, 1, 2),
              ((int64_t)x > (int64_t)other) ? 1 : 2);
        check("setb", x, asm_setb(x, other), (x < other) ? 1 : 0);

        /* The double-width shifts, at a count that crosses the word. */
        check("shld", x, asm_shld(x, other, 13), (x << 13) | (other >> 51));
        check("shrd", x, asm_shrd(x, other, 13), (x >> 13) | (other << 51));

        check("bswap", x, asm_bswap(x), ref_bswap(x));

        /* The fixed-register multiply, against the same 128-bit product
         * mulx is checked on. */
        uint64_t high = 0;
        uint64_t low = asm_mul_full(x, other, &high);
        __uint128_t product = (__uint128_t)x * (__uint128_t)other;
        check("mul.lo", x, low, (uint64_t)product);
        check("mul.hi", x, high, (uint64_t)(product >> 64));

        /* The signed product, both halves, and the overflow flag the
         * two-operand form derives from the high half. */
        __int128 sproduct = (__int128)(int64_t)x * (__int128)(int64_t)other;
        uint64_t shigh = 0;
        uint64_t slow = asm_imul_full(x, other, &shigh);
        check("imul.lo", x, slow, (uint64_t)sproduct);
        check("imul.hi", x, shigh, (uint64_t)(sproduct >> 64));
        uint64_t overflow = 0;
        uint64_t narrow = asm_imul_overflow(x, other, &overflow);
        check("imul2.lo", x, narrow, (uint64_t)sproduct);
        check("imul2.of", x, overflow,
              sproduct != (__int128)(int64_t)(uint64_t)sproduct);

        /* Divide a 128-bit value whose high word is small enough that the
         * quotient fits in 64 bits. */
        const uint64_t divisor = (other | 1);
        uint64_t remainder = 0;
        uint64_t quotient = asm_div_full(0, x, divisor, &remainder);
        check("div.q", x, quotient, x / divisor);
        check("div.r", x, remainder, x % divisor);

        check("crc32", x, asm_crc32(0, x), ref_crc32c(0, x));
    }

    /* bzhi clears the bits from index n upward, and leaves the value
     * alone once n reaches the register width. */
    for (uint64_t n = 0; n <= 70; n++) {
        const uint64_t all = 0xFFFFFFFFFFFFFFFFULL;
        const uint64_t want = (n >= 64) ? all : (all & ((1ULL << n) - 1));
        check("bzhi", n, asm_bzhi(all, n), want);
    }
    }

    check_self_modifying_code();

    if (failures == 0) {
        printf("cputest: ok\n");
        return 0;
    }
    printf("cputest: %d mismatches\n", failures);
    return 1;
}
