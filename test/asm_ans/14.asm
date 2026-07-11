.global main

main:
    mov fh r33 r0
    wm fh r0 r0 0
    rm fh r0 r0 0
    mov fh r0 r34
    mov fh r36 r0
    mov fh r0 r1 1
    add r0 r1 r0
    mov fh r0 r36
    mov fh r36 r0
    mov fh r0 r1 1
    add r0 r1 r0
    mov fh r0 r36
    mov fh r36 r0
    mov fh r0 r1 1
    sub r0 r1 r1
    mov fh r1 r36
    ret
