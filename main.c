typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define PAGE 4096UL
#define TEST_VA 0x40000000UL
#define ENTRY (TEST_VA + PAGE - 2)
#define V  (1UL << 0)
#define R  (1UL << 1)
#define W  (1UL << 2)
#define X  (1UL << 3)
#define A  (1UL << 6)
#define D  (1UL << 7)
#define RX (V | R | X | A)
#define RWX (RX | W | D)

static u64 root_a[512] __attribute__((aligned(PAGE)));
static u64 root_b[512] __attribute__((aligned(PAGE)));
static u64 level1[512] __attribute__((aligned(PAGE)));
static u64 level0[512] __attribute__((aligned(PAGE)));
static u8 first[PAGE] __attribute__((aligned(PAGE)));
static u8 second_a[PAGE] __attribute__((aligned(PAGE)));
static u8 second_b[PAGE] __attribute__((aligned(PAGE)));
static u8 csr_original[PAGE] __attribute__((aligned(PAGE)));
static u8 csr_satp[PAGE] __attribute__((aligned(PAGE)));
static u8 csr_stvec[PAGE] __attribute__((aligned(PAGE)));

static void putc_uart(char c)
{
    volatile u8 *uart = (volatile u8 *)0x10000000UL;
    while (!(uart[5] & 0x20)) { }
    uart[0] = (u8)c;
}

static void puts_uart(const char *s)
{
    while (*s) {
        if (*s == '\n') putc_uart('\r');
        putc_uart(*s++);
    }
}

static void hex(u64 n)
{
    puts_uart("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        putc_uart("0123456789abcdef"[(n >> shift) & 15]);
}

static void field(const char *name, u64 n)
{
    puts_uart(name); hex(n); puts_uart("\n");
}

static u64 satp_read(void)
{
    u64 n;
    __asm__ volatile("csrr %0, satp" : "=r"(n) :: "memory");
    return n;
}

static void translation_sync(void)
{
    __asm__ volatile("sfence.vma zero, zero" ::: "memory");
}

static void instruction_sync(void)
{
    __asm__ volatile("fence.i" ::: "memory");
}

static void stop(u64 failed) __attribute__((noreturn));
static void stop(u64 failed)
{
    /* SBI SRST: system shutdown, reason 0=normal / 1=system failure. */
    register u64 a0 __asm__("a0") = 0;
    register u64 a1 __asm__("a1") = failed;
    register u64 a6 __asm__("a6") = 0;
    register u64 a7 __asm__("a7") = 0x53525354;
    __asm__ volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a6), "r"(a7) : "memory");
    for (;;) __asm__ volatile("wfi");
}

void experiment_trap(u64 cause, u64 epc, u64 tval)
{
    puts_uart("FAIL: unexpected S-mode trap\n");
    field("scause=", cause); field("sepc=", epc); field("stval=", tval);
    stop(1);
}

static void check(int ok, const char *label)
{
    puts_uart(ok ? "PASS: " : "FAIL: "); puts_uart(label); puts_uart("\n");
    if (!ok) stop(1);
}

static u64 pte(const void *pa, u64 flags)
{
    return (((u64)pa >> 12) << 10) | flags;
}

static u64 root_satp(const u64 *root)
{
    return (8UL << 60) | ((u64)root >> 12); /* Sv39, ASID 0 */
}

static void half(u8 *p, u16 value)
{
    *(volatile u16 *)p = value;
}

static void word(u8 *p, u32 value)
{
    /* Instructions following the straddling instruction are also 2-byte aligned. */
    half(p, (u16)value);
    half(p + 2, (u16)(value >> 16));
}

static void show_instruction(void)
{
    u16 lo = *(volatile u16 *)ENTRY;
    u16 hi = *(volatile u16 *)(TEST_VA + PAGE);
    field("instruction VA=", ENTRY);
    field("low16 @ page0+4094=", lo);
    field("high16 @ page1+0=", hi);
    field("encoding32=", (u32)lo | ((u32)hi << 16));
    field("next-page PTE=", level0[1]);
}

/* External call through the ABI: a0 is both argument and result, ra is return PC. */
typedef u64 (*probe_fn)(u64);

extern u64 call_code_page(u64 input, u64 address);
extern u64 call_middle_entry(u64 input, u64 address, u64 *a6_after);
extern void alternate_trap_entry(void);

static u64 stvec_read(void)
{
    u64 n;
    __asm__ volatile("csrr %0, stvec" : "=r"(n) :: "memory");
    return n;
}

static u64 sscratch_read(void)
{
    u64 n;
    __asm__ volatile("csrr %0, sscratch" : "=r"(n) :: "memory");
    return n;
}

static void select_next(u8 *page)
{
    level0[1] = pte(page, RX);
    translation_sync();
    instruction_sync();
}

static void remap_csr_experiment(u64 sa, u64 sb)
{
    const u64 entry_page = TEST_VA + PAGE;
    u64 saved_stvec = stvec_read(), saved_sscratch = sscratch_read();
    u8 *pages[] = { csr_original, csr_satp, csr_stvec };
    u16 heads[] = { 0x1405, 0x1805, 0x1055 };
    puts_uart("BEGIN: immutable code pages, remap-only CSR synthesis\n");
    field("original code page PA=", (u64)csr_original);
    field("satp code page PA=", (u64)csr_satp);
    field("stvec code page PA=", (u64)csr_stvec);

    /* Each candidate is legal code entered at its own page start:
     * 1405 = c.addi s0,-31; 1805 = c.addi a6,-31;
     * 1055 = c.nop -11 (architectural HINT).
     * All continue with sfence.vma x0,x0; ret. No CSR write in this stream.
     * From ENTRY, the first halfword instead completes a 32-bit CSR write.
     */
    for (unsigned i = 0; i < 3; ++i) {
        half(pages[i], heads[i]);
        word(pages[i] + 2, 0x12000073);
        word(pages[i] + 6, 0x00008067);
    }
    instruction_sync();
    for (unsigned i = 0; i < 3; ++i) {
        select_next(pages[i]);
        field("standalone page first16=", heads[i]);
        u64 result = call_code_page(0x55, entry_page);
        check(result == 0x55 && satp_read() == sb &&
              stvec_read() == saved_stvec && sscratch_read() == saved_sscratch,
              "candidate next page executes independently without CSR writes");
    }

    /* Negative control: the unchanged 0513 low half retains the ADDI opcode.
     * 18050513 = addi a0,a0,384; 10550513 = addi a0,a0,261.
     */
    half(first + PAGE - 2, 0x0513);
    instruction_sync();
    for (unsigned i = 1; i < 3; ++i) {
        select_next(pages[i]);
        show_instruction();
        u64 result = call_code_page(0x55, ENTRY);
        field("negative-control a0 after=", result);
        check(result == 0x55 + (i == 1 ? 384UL : 261UL) &&
              satp_read() == sb && stvec_read() == saved_stvec &&
              sscratch_read() == saved_sscratch,
              "ADDI low half cannot become a CSR write by remapping");
    }

    /* Freeze low half at 1073. After this store, only the next-page PTE changes.
     * Original: 14051073 = csrw sscratch,a0, not a satp/stvec write.
     */
    half(first + PAGE - 2, 0x1073);
    instruction_sync();
    select_next(csr_original);
    show_instruction();
    field("original sscratch before=", saved_sscratch);
    call_code_page(0x55, ENTRY);
    field("original sscratch after=", sscratch_read());
    check(sscratch_read() == 0x55 && satp_read() == sb && stvec_read() == saved_stvec,
          "original stream writes sscratch only");

    /* Same first page and low half; only map the satp code page below. */
    select_next(csr_satp);
    show_instruction();
    field("remapped satp before=", satp_read());
    field("remapped satp operand=", sa);
    call_code_page(sa, ENTRY);
    field("remapped satp after=", satp_read());
    check(satp_read() == sa && stvec_read() == saved_stvec && sscratch_read() == 0x55,
          "PTE-only replacement synthesizes csrw satp from csrw sscratch");

    /* Return to the original mapping and execute it again before stvec remap. */
    select_next(csr_original);
    call_code_page(0x66, ENTRY);
    check(sscratch_read() == 0x66 && satp_read() == sa && stvec_read() == saved_stvec,
          "original mapping still writes sscratch only");
    select_next(csr_stvec);
    show_instruction();
    u64 target = (u64)alternate_trap_entry;
    field("remapped stvec before=", stvec_read());
    field("remapped stvec operand=", target);
    call_code_page(target, ENTRY);
    field("remapped stvec after=", stvec_read());
    check(target != saved_stvec && stvec_read() == target &&
          satp_read() == sa && sscratch_read() == 0x66,
          "PTE-only replacement synthesizes csrw stvec from csrw sscratch");
    check(*(volatile u16 *)(first + PAGE - 2) == 0x1073,
          "first-page low half unchanged throughout CSR remapping");
    __asm__ volatile("csrw stvec, %0\ncsrw sscratch, %1"
                     :: "r"(saved_stvec), "r"(saved_sscratch) : "memory");
    puts_uart("PASS: remap-only CSR experiment complete; stvec/sscratch restored\n");
}

static void middle_entry_experiment(u64 sa, u64 sb)
{
    puts_uart("BEGIN: same bytes and PTEs, enter ordinary instruction at +2\n");
    check(satp_read() == sa && sa != sb, "middle-entry test starts at root A");

    /* Prepare once. No code stores or PTE changes between the two calls.
     * Normal entry: 10730513 = addi a0,t1,263; 1805 = c.addi a6,-31.
     * Entry + 2:    18051073 = csrw satp,a0.
     * Common continuation flushes translations after the CSR write and returns.
     */
    word(first + PAGE - 4, 0x10730513);
    half(csr_satp, 0x1805);
    word(csr_satp + 2, 0x12000073);
    word(csr_satp + 6, 0x00008067);
    select_next(csr_satp);
    u64 pte0 = level0[0], pte1 = level0[1];
    field("normal entry VA=", ENTRY - 2);
    field("middle entry VA=", ENTRY);
    field("normal encoding32=", 0x10730513);
    field("following encoding16=", 0x1805);
    show_instruction();

    u64 a6_after;
    field("normal a0 before=", sb);
    field("normal t1 before=", 0);
    field("normal a6 before=", 64);
    field("normal satp before=", satp_read());
    u64 result = call_middle_entry(sb, ENTRY - 2, &a6_after);
    field("normal a0 after=", result);
    field("normal a6 after=", a6_after);
    field("normal satp after=", satp_read());
    check(result == 263 && a6_after == 33 && satp_read() == sa,
          "normal stream executes ADDI and C.ADDI without writing satp");

    field("middle a0 before=", sb);
    field("middle t1 before=", 0);
    field("middle a6 before=", 64);
    field("middle satp before=", satp_read());
    result = call_middle_entry(sb, ENTRY, &a6_after);
    field("middle a0 after=", result);
    field("middle a6 after=", a6_after);
    field("middle satp after=", satp_read());
    check(result == sb && a6_after == 64 && satp_read() == sb,
          "entry +2 executes synthesized csrw satp,a0 and switches to root B");
    check(level0[0] == pte0 && level0[1] == pte1 &&
          *(volatile u16 *)(first + PAGE - 4) == 0x0513 &&
          *(volatile u16 *)(first + PAGE - 2) == 0x1073 &&
          *(volatile u16 *)csr_satp == 0x1805,
          "both streams used identical code bytes and leaf PTEs; no remapping");
}

void experiment_main(void)
{
    puts_uart("RVC-Straddle S-mode / Sv39 / single hart\n");
    u64 initial = satp_read();
    field("initial satp=", initial);
    check(initial == 0, "OpenSBI entered with Bare satp");

    /* Identity map MMIO [0,1GiB) and RAM [2GiB,3GiB) with 1GiB leaves.
     * The test range [1GiB,1GiB+8KiB) uses two independent 4KiB RX leaves.
     * U=0, A/D preset; backing pages are written via the RAM identity alias.
     */
    root_a[0] = RWX;
    root_a[2] = ((0x80000000UL >> 12) << 10) | RWX;
    root_a[1] = pte(level1, V);
    level1[0] = pte(level0, V);
    level0[0] = pte(first, RX);
    level0[1] = pte(second_a, RX);
    for (unsigned i = 0; i < 512; ++i) root_b[i] = root_a[i];
    u64 sa = root_satp(root_a), sb = root_satp(root_b);
    field("root A PA=", (u64)root_a); field("root B PA=", (u64)root_b);
    field("page0 PA=", (u64)first);
    field("page1 A PA=", (u64)second_a); field("page1 B PA=", (u64)second_b);

    /* addi a0,zero,1 = 00100513; addi a0,zero,2 = 00200513. */
    half(first + PAGE - 2, 0x0513);
    half(second_a, 0x0010); word(second_a + 2, 0x00008067); /* ret */
    half(second_b, 0x0020); word(second_b + 2, 0x00008067);
    translation_sync();
    __asm__ volatile("csrw satp, %0" :: "r"(sa) : "memory");
    translation_sync(); instruction_sync();
    check(satp_read() == sa, "Sv39 enabled with root A");

    probe_fn run = (probe_fn)ENTRY;
    show_instruction();
    u64 before = satp_read();
    u64 result = run(0x55);
    field("a0 before=", 0x55); field("a0 after=", result);
    field("satp before=", before); field("satp after=", satp_read());
    check(result == 1 && satp_read() == sa, "straddling addi returns 1");

    level0[1] = pte(second_b, RX);
    translation_sync(); instruction_sync();
    show_instruction();
    before = satp_read(); result = run(0x55);
    field("a0 before=", 0x55); field("a0 after=", result);
    field("satp before=", before); field("satp after=", satp_read());
    check(result == 2 && satp_read() == sa, "replacing only next page returns 2");

    /* csrrw x0,satp,a0 = 18051073 (csrw satp,a0), straddled at ENTRY.
     * Both roots share identical mappings, so the following fetch stays valid.
     * sfence.vma x0,x0; csrr a0,satp; ret captures the new CSR in a0.
     */
    half(first + PAGE - 2, 0x1073);
    half(second_b, 0x1805);
    word(second_b + 2, 0x12000073);
    word(second_b + 6, 0x18002573);
    word(second_b + 10, 0x00008067);
    instruction_sync();
    show_instruction();
    before = satp_read();
    field("satp before=", before); field("a0 write operand=", sb);
    result = run(sb);
    u64 after = satp_read();
    field("a0 readback=", result); field("satp after=", after);
    check(before == sa && result == sb && after == sb && sa != sb,
          "straddling csrw satp switches root A to root B in S-mode");
    remap_csr_experiment(sa, sb);
    middle_entry_experiment(sa, sb);
    puts_uart("ALL PASS: cross-page, remap-only CSR, and middle-entry satp\n");
    stop(0);
}
