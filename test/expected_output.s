.module test

    nop                          ;; [1]

SYMVALUE = 10

    .db 0xAA                     ;; (1)
.area _DATA_sample
values :
    .db 1, 2, 3                  ;; (3)
    .dw 0x1234                   ;; (2)
patch_value : .dw 0              ;; (2)
    .asciz "A"                   ;; (2)
.rept 3
    .db 0                        ;; (3)
    .dw 0                        ;; (6)
.endm
.rept SYMVALUE
    .db 0                        ;; (10)
.endm

.area _CODE
start:
    ld a, b                      ;; [1] already correct
    ld (hl), e                  ;; [2] deliberately incorrect
    jr z, next                   ;; [2/3]
    jr c, next                   ;; [2/3]
    bit 7, (hl)                  ;; [3]
    call helper                  ;; [5]
    call computeXIntersect       ;; [5]
    ld bc, #XMinM1               ;; [3]
    ld hl, #patch_value + 3      ;; [3]
    ld hl, #3 + patch_value      ;; [3]
    ld a, #0x01                  ;; [2]
    ld (patch_value+1), a        ;; [4]
    or a, #0x01                  ;; [2]
    call mystery                 ;; [5]
    call mystery                 ;; [5]
    unknown_op 1
    sub e                        ;; [1]
    ld__a_ixl                    ;; [2]
    cp__ixh                      ;; [2]
    ld___hl__a                   ;; [2]
    ld__ixh #0x12                ;; [3]
    ld__iyl #0x34                ;; [3]
    ld de,(x0_val)               ;; [6]
    or d                         ;; [1]
    and d                        ;; [1]
    ld a,<#label                 ;; [2]
    ld a,>#label                 ;; [2]
    ld a,<#1234                  ;; [2]
    ld a,>#0x1234                ;; [2]
.macro ld__a_ixl
    ld a, ixl                    ;; [2]
.endm
.macro cp__ixh
    cp ixh                       ;; [2]
.endm
.macro draw
    ld a, b                      ;; [1]
.endm
    draw                         ;; [1]
#ifdef FEATURE
#endif
next:
    xor a                        ;; [1]
helper:
    ret                          ;; [3]
computeXIntersect:
    ret                          ;; [3]
    .db 0xBB                     ;; (1)
