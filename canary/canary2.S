/*
 * Log-pipeline canary for the Mi Pad 3. Loaded like an arm64 kernel (MMU off), it
 *  1. fills framebuffer 0 with green (so it can be seen to have run),
 *  2. writes a valid ramoops console zone at 0x9007f000 (header "DBGC", start 0,
 *     size 4096, then 64 copies of a 64-byte text line), the zone LineageOS 3.18
 *     reads with ramoops.mem_address=0x90000000 ramoops.mem_size=0xc0000,
 *  3. arms the TOPRGU watchdog the way stock does, external (PMIC) reset on, 5 s,
 *     and halts. LineageOS then shows the zone as /sys/fs/pstore/console-ramoops.
 */
	.text
	.globl	_start
_start:
	b	real_start
	.word	0
	.quad	0
	.quad	0x10000
	.quad	0xa
	.quad	0, 0, 0
	.ascii	"ARM\x64"
	.word	0
real_start:
	/* 1: framebuffer 0 green */
	ldr	x0, =0xfcf00000
	ldr	x2, =0xc00000
	ldr	w1, =0xff00ff00
1:	str	w1, [x0], #4
	subs	x2, x2, #4
	b.ne	1b

	/* 2: ramoops console zone */
	ldr	x0, =0x9007f000
	ldr	w1, =0x43474244		/* PERSISTENT_RAM_SIG ^ 0 */
	str	w1, [x0]
	str	wzr, [x0, #4]		/* start */
	mov	w1, #4096
	str	w1, [x0, #8]		/* size */
	add	x0, x0, #12		/* data */
	mov	x4, #64			/* copies */
2:	adr	x5, line
	mov	x6, #64
3:	ldrb	w7, [x5], #1
	strb	w7, [x0], #1
	subs	x6, x6, #1
	b.ne	3b
	subs	x4, x4, #1
	b.ne	2b
	dsb	sy

	/* 3: watchdog, stock style: enable + external reset, 5 s */
	ldr	x0, =0x10007000
	ldr	w1, =0x2808		/* WDT_LENGTH: ((5 << 6) << 5) | key */
	str	w1, [x0, #4]
	ldr	w1, =0x1971		/* WDT_RST reload */
	str	w1, [x0, #8]
	ldr	w1, =0x22000005		/* WDT_MODE: key | EXRST_EN | EN */
	str	w1, [x0]
	dsb	sy
halt:	wfe
	b	halt

	.ltorg
line:	.ascii	"CANARY2 the quick brown fox jumps over the lazy dog 01234567890\n"
