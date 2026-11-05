/* crt0.S — _start uClibc pour lsc_onvif (ARMv5, uClibc 1.0.28 sur la camera).
 * uClibc n'exporte pas __libc_start_main mais __uClibc_main(main, argc, argv,
 * app_init, app_fini, rtld_fini, stack_end). */
	.text
	.global _start
	.type _start, %function
_start:
	mov	r12, sp
	ldr	r1, [r12]		/* argc */
	add	r2, r12, #4		/* argv */
	mov	r3, #0			/* app_init */
	sub	sp, sp, #16
	mov	r0, #0
	str	r0, [sp]		/* app_fini */
	str	r0, [sp, #4]		/* rtld_fini */
	str	r12, [sp, #8]		/* stack_end */
	ldr	r0, =main
	bl	__uClibc_main
1:	b	1b
	.size _start, .-_start
