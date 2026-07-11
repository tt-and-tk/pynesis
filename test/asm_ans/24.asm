.global main

main:
    mov fh r0 r0 -5
    wm fh r0 r0 0
    mov fh r0 r0 5
    mov fh r0 r1 0
    sub r1 r0 r0
    wm fh r0 r0 4
    rm fh r0 r0 0
    rm fh r0 r1 4
    add r0 r1 r0
    wm fh r0 r0 8
    ret
