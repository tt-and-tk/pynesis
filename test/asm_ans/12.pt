.global main

main:
    mov fh r0 r2 0
    mov fh r0 r4 10
    mov fh r0 r5 7
    mov fh r0 r6 1
    mov fh r0 r7 0
    scan r0
.L0:
    ne r0 r4 .L1
    scan r0
    jmp .L0
.L1:
    mov fh r0 r1 0
.L2:
    eq r0 r4 .L3
    add r2 r1 r3
    wm 1h r3 r0
    add r1 r6 r1
    egt r1 r5 .L3
    scan r0
    jmp .L2
.L3:
    add r2 r1 r3
    wm 1h r3 r7
    mov fh r0 r2 8
    mov fh r0 r4 10
    mov fh r0 r5 7
    mov fh r0 r6 1
    mov fh r0 r7 0
    scan r0
.L4:
    ne r0 r4 .L5
    scan r0
    jmp .L4
.L5:
    mov fh r0 r1 0
.L6:
    eq r0 r4 .L7
    add r2 r1 r3
    wm 1h r3 r0
    add r1 r6 r1
    egt r1 r5 .L7
    scan r0
    jmp .L6
.L7:
    add r2 r1 r3
    wm 1h r3 r7
    mov fh r0 r1 0
    mov fh r0 r0 0
    mov fh r0 r3 8
    mov fh r0 r4 0
    mov fh r0 r5 1
.L8:
    egt r0 r3 .L9
    add r1 r0 r2
    rm 1h r2 r2
    eq r2 r4 .L9
    print r2
    add r0 r5 r0
    jmp .L8
.L9:
    mov fh r0 r1 8
    mov fh r0 r0 0
    mov fh r0 r3 8
    mov fh r0 r4 0
    mov fh r0 r5 1
.L10:
    egt r0 r3 .L11
    add r1 r0 r2
    rm 1h r2 r2
    eq r2 r4 .L11
    print r2
    add r0 r5 r0
    jmp .L10
.L11:
    ret
