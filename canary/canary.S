/*
 * Display canary for the Mi Pad 3 LK. Loaded like an arm64 kernel (MMU off, EL2), it fills
 * the three 1536x2048x32 buffers at 0xfcf00000 with different colours and halts:
 *   buffer 0 = 0xffff0000  (red in a8r8g8b8, blue if the panel path is a8b8g8r8)
 *   buffer 1 = 0xff00ff00  (green either way)
 *   buffer 2 = 0xffffff00  (yellow in a8r8g8b8, cyan in a8b8g8r8)
 * The colour on screen shows that LK jumped here, which buffer is scanned out, and the
 * pixel order.
 */
	.text
	.globl	_start
_start:
	b	real_start		/* code0 */
	.word	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	0x10000			/* image_size */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0, 0, 0			/* res2-res4 */
	.ascii	"ARM\x64"		/* magic */
	.word	0			/* res5 */
real_start:
	ldr	x0, =0xfcf00000
	ldr	x2, =0xc00000		/* one buffer: 1536 * 2048 * 4 */
	ldr	w1, =0xffff0000
	bl	fill
	ldr	w1, =0xff00ff00
	bl	fill
	ldr	w1, =0xffffff00
	bl	fill
	dsb	sy
halt:	wfe
	b	halt
fill:	mov	x3, x2
1:	str	w1, [x0], #4
	subs	x3, x3, #4
	b.ne	1b
	ret
	.ltorg
