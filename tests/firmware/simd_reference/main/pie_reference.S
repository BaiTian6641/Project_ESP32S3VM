/* Ordinary Xtensa assembler: no simulator-only instructions or MMIO.
 * Q/accumulator state is read before and after the selected real PIE operation.
 * a2=operation, a3=input, a4=before, a5=after. Input/state buffers align to 16.
 */
    .text
    .align 4
    .global pie_reference
    .type pie_reference,@function

    .macro snapshot
    mov a9, a6
    ee.vst.128.ip q0, a6, 16
    ee.vst.128.ip q1, a6, 16
    ee.vst.128.ip q2, a6, 16
    ee.vst.128.ip q3, a6, 16
    ee.vst.128.ip q4, a6, 16
    ee.vst.128.ip q5, a6, 16
    ee.vst.128.ip q6, a6, 16
    ee.vst.128.ip q7, a6, 16
    rur.qacc_l_0 a8
    s32i a8, a6, 0
    rur.qacc_l_1 a8
    s32i a8, a6, 4
    rur.qacc_l_2 a8
    s32i a8, a6, 8
    rur.qacc_l_3 a8
    s32i a8, a6, 12
    rur.qacc_l_4 a8
    s32i a8, a6, 16
    rur.qacc_h_0 a8
    s32i a8, a6, 20
    rur.qacc_h_1 a8
    s32i a8, a6, 24
    rur.qacc_h_2 a8
    s32i a8, a6, 28
    rur.qacc_h_3 a8
    s32i a8, a6, 32
    rur.qacc_h_4 a8
    s32i a8, a6, 36
    rsr.sar a8
    s32i a8, a6, 40
    rur.sar_byte a8
    s32i a8, a6, 44
    rur.fft_bit_width a8
    s32i a8, a6, 48
    rur.accx_0 a8
    s32i a8, a6, 52
    rur.accx_1 a8
    s32i a8, a6, 56
    rur.ua_state_0 a8
    s32i a8, a6, 60
    rur.ua_state_1 a8
    s32i a8, a6, 64
    rur.ua_state_2 a8
    s32i a8, a6, 68
    rur.ua_state_3 a8
    s32i a8, a6, 72
    s32i a10, a6, 76             /* observed source pointer increment */
    sub a8, a6, a9
    s32i a8, a6, 80              /* observed snapshot store increment */
    memw
    .endm

pie_reference:
    entry a1, 32
    mov a11, a3
    ee.vld.128.ip q0, a3, 16
    ee.vld.128.ip q1, a3, 16
    ee.vld.128.ip q2, a3, 16
    ee.vld.128.ip q3, a3, 16
    ee.vld.128.ip q4, a3, 16
    ee.vld.128.ip q5, a3, 16
    ee.vld.128.ip q6, a3, 16
    ee.vld.128.ip q7, a3, 16
    sub a10, a3, a11
    l32i a8, a3, 0
    wur.qacc_l_0 a8
    l32i a8, a3, 4
    wur.qacc_l_1 a8
    l32i a8, a3, 8
    wur.qacc_l_2 a8
    l32i a8, a3, 12
    wur.qacc_l_3 a8
    l32i a8, a3, 16
    wur.qacc_l_4 a8
    l32i a8, a3, 20
    wur.qacc_h_0 a8
    l32i a8, a3, 24
    wur.qacc_h_1 a8
    l32i a8, a3, 28
    wur.qacc_h_2 a8
    l32i a8, a3, 32
    wur.qacc_h_3 a8
    l32i a8, a3, 36
    wur.qacc_h_4 a8
    movi a8, 0
    wsr.sar a8
    isync
    wur.sar_byte a8
    wur.fft_bit_width a8
    wur.accx_0 a8
    wur.accx_1 a8
    wur.ua_state_0 a8
    wur.ua_state_1 a8
    wur.ua_state_2 a8
    wur.ua_state_3 a8
    mov a6, a4
    snapshot
    beqi a2, 0, .mac_u8
    beqi a2, 1, .add_s8
    beqi a2, 2, .add_s8_alias
    beqi a2, 3, .move_q
    beqi a2, 4, .sub_s8
    beqi a2, 5, .mac_s8
    beqi a2, 6, .add_s16_alias
    ill
.mac_u8:
    ee.vmulas.u8.qacc q0, q1
    j .done
.add_s8:
    ee.vadds.s8 q2, q0, q1
    j .done
.add_s8_alias:
    ee.vadds.s8 q0, q0, q1
    j .done
.move_q:
    mv.qr q2, q0
    j .done
.sub_s8:
    ee.vsubs.s8 q2, q0, q1
    j .done
.mac_s8:
    ee.vmulas.s8.qacc q0, q1
    j .done
.add_s16_alias:
    ee.vadds.s16 q1, q0, q1
.done:
    mov a6, a5
    snapshot
    retw.n
    .size pie_reference, .-pie_reference
