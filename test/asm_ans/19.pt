.global main

main:
    mov fh r0 r0 16961
    wm fh r0 r0 0
    mov fh r0 r0 17475
    wm fh r0 r0 4
    mov fh r0 r1 0
    mov fh r0 r0 0
    mov fh r0 r3 3
    mov fh r0 r4 0
    mov fh r0 r5 1
.L0:
    egt r0 r3 .L1
    add r1 r0 r2
    rm 1h r2 r2
    eq r2 r4 .L1
    print r2
    add r0 r5 r0
    jmp .L0
.L1:
    mov fh r0 r1 4
    mov fh r0 r0 0
    mov fh r0 r3 3
    mov fh r0 r4 0
    mov fh r0 r5 1
.L2:
    egt r0 r3 .L3
    add r1 r0 r2
    rm 1h r2 r2
    eq r2 r4 .L3
    print r2
    add r0 r5 r0
    jmp .L2
.L3:
    ret
