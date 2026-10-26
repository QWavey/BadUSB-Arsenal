    .section .vectors, "ax"
    .global _start
    .type _start, @function
_start:
    /* Setup stack pointer */
    lda.w   sp, _stack_start
    
    /* Jump to data initialization */
    rjmp    _init_data

    .section .text
    .global _init_data
    .type _init_data, @function
_init_data:
    /* Copy data section from flash to RAM */
    lda.w   r0, _sdata
    lda.w   r1, _edata
    lda.w   r2, _sidata
1:
    cp.w    r0, r1
    brge    2f
    ld.w    r3, r2[0]
    st.w    r0[0], r3
    sub     r0, -4
    sub     r2, -4
    rjmp    1b
2:
    /* Clear BSS section */
    lda.w   r0, _sbss
    lda.w   r1, _ebss
    mov     r2, 0
3:
    cp.w    r0, r1
    brge    4f
    st.w    r0[0], r2
    sub     r0, -4
    rjmp    3b
4:
    /* Execute main */
    call    main
3:
    /* Loop forever if main returns */
    rjmp    3b