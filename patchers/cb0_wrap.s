@ cb0 wrapper (vL) : prend le mutex video 0x4D2AD8, appelle le cb0 usine
@ (0x8F41C) puis relache. But : serialiser le tx session1 (cb0, thread chn0)
@ avec le tx session2 (cb1, thread chn1 qui tient deja 0x4D2AD8).
@ Place a VA 0x49A6E0 (gap libre 0x48A6E0..0x48A73F cote usine).
@ Les 3 .word 0xeb000000 sont patchees par build_vL.py dans l'ordre :
@   lock 0x11A58C, factory_cb0 0x8F41C, unlock 0x11A5A0.
@ ABI cb : r0=data, r1=len, r2=priv, usec64 a [sp] (a l'entree). On sauve lr
@ (2 bl), on recopie le usec a [sp] pour l'appel factory, sp restaure au retour.
	.arm
	.text
	.global cb0_wrap
cb0_wrap:
	push {r4,r5,r6,r7,lr}
	sub sp,sp,#8
	mov r4,r0
	mov r5,r1
	mov r6,r2
	ldr r7,mu_lit
	mov r0,r7
	.word 0xeb000000	@ bl ak_thread_mutex_lock(0x4D2AD8)
	ldr r0,[sp,#28]		@ usec64 entrant (sp0 = sp+28)
	ldr r1,[sp,#32]
	str r0,[sp]
	str r1,[sp,#4]
	mov r0,r4
	mov r1,r5
	mov r2,r6
	.word 0xeb000000	@ bl factory_cb0 (usec64 recopie a [sp])
	ldr r0,mu_lit
	.word 0xeb000000	@ bl ak_thread_mutex_unlock(0x4D2AD8)
	add sp,sp,#8
	pop {r4,r5,r6,r7,pc}
mu_lit: .word 0x004D2AD8
