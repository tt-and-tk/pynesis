.global main

main:
    mov fh r0 r0 200
    mov fh r0 r1 0
    mov fh r0 r2 0
    add r1 r2 r1
    wm 1h r1 r0
    mov fh r0 r0 0
    mov fh r0 r1 0
    add r0 r1 r0
    rm 1h r0 r0
    mov fh r0 r1 24
    sll r0 r1 r0
    sra r0 r1 r0
    wm fh r0 r0 8
    mov fh r0 r0 40000
    mov fh r0 r1 0
    mov fh r0 r2 1
    sll r1 r2 r1
    mov fh r0 r2 4
    add r1 r2 r1
    wm 3h r1 r0
    mov fh r0 r0 0
    mov fh r0 r1 1
    sll r0 r1 r0
    mov fh r0 r1 4
    add r0 r1 r0
    rm 3h r0 r0
    mov fh r0 r1 16
    sll r0 r1 r0
    sra r0 r1 r0
    wm fh r0 r0 12
    ret
