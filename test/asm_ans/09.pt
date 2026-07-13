.global main

main:
    mov fh r0 r0 0
    wm fh r0 r0 0
    mov fh r0 r0 2
    wm fh r0 r0 4
    rm fh r0 r0 4
    mov fh r0 r1 1
    eq r0 r1 .L0
    mov fh r0 r1 2
    eq r0 r1 .L1
    mov fh r0 r1 3
    eq r0 r1 .L2
    jmp .L3
.L0:
.L1:
    mov fh r0 r0 10
    wm fh r0 r0 0
    jmp .L4
.L2:
    mov fh r0 r0 20
    wm fh r0 r0 0
.L3:
    mov fh r0 r0 30
    wm fh r0 r0 0
.L4:
    ret
