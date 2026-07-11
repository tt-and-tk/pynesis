.global main

main:
    mov fh r0 r0 3
    wm fh r0 r0 0
    mov fh r0 r0 3
    wm fh r0 r0 4
    mov fh r0 r0 6
    wm fh r0 r0 8
    mov fh r0 r0 3
    wm fh r0 r0 12
    mov fh r0 r0 2
    wm fh r0 r0 16
    mov fh r0 r0 2
    wm fh r0 r0 20
    mov fh r0 r0 7
    wm fh r0 r0 24
    mov fh r0 r0 5
    wm fh r0 r0 28
    mov fh r0 r0 16
    wm fh r0 r0 32
    mov fh r0 r0 16
    wm fh r0 r0 36
    mov fh r0 r0 7
    wm fh r0 r0 40
    mov fh r0 r0 1
    mov fh r0 r1 2
    add r0 r1 r0
    wm fh r0 r0 44
    mov fh r0 r0 5
    mov fh r0 r1 2
    sub r0 r1 r0
    wm fh r0 r0 48
    mov fh r0 r0 2
    mov fh r0 r1 3
    mul r0 r1 r0
    wm fh r0 r0 52
    mov fh r0 r0 17
    mov fh r0 r1 5
    div r0 r1 r0
    wm fh r0 r0 56
    mov fh r0 r0 17
    mov fh r0 r1 5
    div r0 r1 r1 0
    wm fh r0 r0 60
    mov fh r0 r0 6
    mov fh r0 r1 3
    and r0 r1 r0
    wm fh r0 r0 64
    mov fh r0 r0 6
    mov fh r0 r1 1
    or r0 r1 r0
    wm fh r0 r0 68
    mov fh r0 r0 6
    mov fh r0 r1 3
    xor r0 r1 r0
    wm fh r0 r0 72
    mov fh r0 r0 1
    mov fh r0 r1 4
    sll r0 r1 r0
    wm fh r0 r0 76
    mov fh r0 r0 64
    mov fh r0 r1 2
    sra r0 r1 r0
    wm fh r0 r0 80
    mov fh r0 r0 1
    mov fh r0 r1 2
    mov fh r0 r2 3
    mul r1 r2 r1
    add r0 r1 r0
    wm fh r0 r0 84
    ret
