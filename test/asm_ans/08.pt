.global main

main:
    mov fh r0 r0 0
    wm fh r0 r0 0
    mov fh r0 r0 0
    wm fh r0 r0 4
.L0:
    rm fh r0 r0 4
    mov fh r0 r1 1
    add r0 r1 r0
    wm fh r0 r0 4
    rm fh r0 r0 4
    mov fh r0 r1 2
    ne r0 r1 .L3
    jmp .L1
.L3:
    rm fh r0 r0 4
    mov fh r0 r1 4
    ne r0 r1 .L4
    jmp .L2
.L4:
    rm fh r0 r0 0
    rm fh r0 r1 4
    add r0 r1 r0
    wm fh r0 r0 0
.L1:
    rm fh r0 r0 4
    mov fh r0 r1 5
    lt r0 r1 .L0
.L2:
    ret
