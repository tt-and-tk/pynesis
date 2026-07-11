.global main

main:
    mov fh r0 r0 0
    wm fh r0 r0 0
    mov fh r0 r0 3
    wm fh r0 r0 4
    mov fh r0 r0 0
    wm fh r0 r0 8
    rm fh r0 r0 4
    rm fh r0 r1 8
    lt r0 r1 .L0
    mov fh r0 r0 0
    jmp .L1
.L0:
    mov fh r0 r0 1
.L1:
    wm fh r0 r0 12
    rm fh r0 r0 4
    mov fh r0 r1 0
    eq r0 r1 .L2
    rm fh r0 r0 8
    mov fh r0 r1 0
    eq r0 r1 .L2
    mov fh r0 r0 1
    jmp .L3
.L2:
    mov fh r0 r0 0
.L3:
    wm fh r0 r0 16
    rm fh r0 r0 4
    mov fh r0 r1 0
    ne r0 r1 .L4
    rm fh r0 r0 8
    mov fh r0 r1 0
    ne r0 r1 .L4
    mov fh r0 r0 0
    jmp .L5
.L4:
    mov fh r0 r0 1
.L5:
    wm fh r0 r0 20
    rm fh r0 r0 4
    rm fh r0 r1 8
    egt r0 r1 .L6
    rm fh r0 r0 4
    jmp .L7
.L6:
    rm fh r0 r0 8
.L7:
    wm fh r0 r0 24
    ret
