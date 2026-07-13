.global read_first, forward, main

main:
    mov fh r0 r0 99
    mov fh r0 r1 0
    mov fh r0 r2 2
    sll r1 r2 r1
    mov fh r0 r2 0
    add r1 r2 r1
    wm fh r1 r0
    mov fh r0 r0 0
    wm fh r0 r0 12
    call forward
    mov fh r30 r0
    wm fh r0 r0 16
    ret

read_first:
    mov fh r0 r0 0
    mov fh r0 r1 2
    sll r0 r1 r0
    rm fh r0 r1 8
    add r0 r1 r0
    rm fh r0 r0
    mov fh r0 r30
    ret
    ret

forward:
    rm fh r0 r0 12
    wm fh r0 r0 8
    call read_first
    mov fh r30 r0
    mov fh r0 r30
    ret
    ret
