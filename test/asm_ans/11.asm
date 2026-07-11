.global helper, main

main:
    mov fh r0 r0 0
    wm fh r0 r0 0
    mov fh r0 r0 0
    wm fh r0 r0 8
.L0:
    rm fh r0 r0 8
    mov fh r0 r1 3
    egt r0 r1 .L2
    call helper
.L1:
    rm fh r0 r0 8
    mov fh r0 r1 1
    add r0 r1 r0
    wm fh r0 r0 8
    jmp .L0
.L2:
    ret

helper:
    mov fh r0 r0 5
    wm fh r0 r0 4
    rm fh r0 r0 0
    rm fh r0 r1 4
    add r0 r1 r0
    wm fh r0 r0 0
    ret
