.global f, main

main:
    mov fh r0 r0 10
    wm fh r0 r0 4
    mov fh r0 r0 20
    wm fh r0 r0 8
    rm fh r0 r0 4
    wm fh r0 r0 16
    rm fh r0 r1 8
    wm fh r0 r1 0
    call f
    mov fh r30 r1
    rm fh r0 r0 16
    add r0 r1 r0
    wm fh r0 r0 12
    ret

f:
    rm fh r0 r0 0
    mov fh r0 r1 1
    add r0 r1 r0
    mov fh r0 r30
    ret
    ret
