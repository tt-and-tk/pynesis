.global main

main:
    mov fh r0 r0 100
    wm 1h r0 r0 0
    rm 1h r0 r0 0
    mov fh r0 r1 24
    sll r0 r1 r0
    sra r0 r1 r0
    mov fh r0 r1 100
    add r0 r1 r0
    wm 1h r0 r0 0
    rm 1h r0 r0 0
    mov fh r0 r1 24
    sll r0 r1 r0
    sra r0 r1 r0
    wm fh r0 r0 4
    ret
