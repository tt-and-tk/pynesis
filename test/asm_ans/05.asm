.global main

main:
    mov fh r0 r0 5
    wm fh r0 r0 0
    mov fh r0 r0 3
    wm fh r0 r0 4
    rm fh r0 r0 4
    rm fh r0 r1 0
    egt r0 r1 .L0
    mov fh r0 r0 1
    wm fh r0 r0 4
    jmp .L1
.L0:
    mov fh r0 r0 2
    wm fh r0 r0 4
.L1:
    rm fh r0 r0 0
    mov fh r0 r1 0
    elt r0 r1 .L2
    mov fh r0 r0 0
    wm fh r0 r0 0
.L2:
    rm fh r0 r0 4
    mov fh r0 r1 0
    eq r0 r1 .L3
    mov fh r0 r0 9
    wm fh r0 r0 0
.L3:
    rm fh r0 r0 4
    mov fh r0 r1 1
    ne r0 r1 .L4
    mov fh r0 r0 10
    wm fh r0 r0 0
    jmp .L5
.L4:
    rm fh r0 r0 4
    mov fh r0 r1 2
    ne r0 r1 .L6
    mov fh r0 r0 20
    wm fh r0 r0 0
    jmp .L7
.L6:
    mov fh r0 r0 30
    wm fh r0 r0 0
.L7:
.L5:
    ret
