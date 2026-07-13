.global f, main

main:
    mov fh r0 r0 5
    wm fh r0 r0 36
    mov fh r0 r0 3
    wm fh r0 r0 40
    mov fh r0 r0 0
    wm fh r0 r0 44
    rm fh r0 r0 36
    wm fh r0 r0 48
    rm fh r0 r1 40
    wm fh r0 r1 0
    call f
    mov fh r30 r1
    rm fh r0 r0 48
    elt r0 r1 .L0
    mov fh r0 r0 1
    wm fh r0 r0 44
.L0:
    rm fh r0 r0 44
    wm fh r0 r0 48
    rm fh r0 r1 36
    wm fh r0 r1 0
    call f
    mov fh r30 r1
    rm fh r0 r0 48
    add r0 r1 r0
    wm fh r0 r0 44
    rm fh r0 r0 44
    wm fh r0 r0 48
    rm fh r0 r1 36
    wm fh r0 r1 0
    call f
    mov fh r30 r1
    rm fh r0 r0 48
    mov fh r0 r2 2
    sll r1 r2 r1
    mov fh r0 r2 4
    add r1 r2 r1
    wm fh r1 r0
    ret

f:
    rm fh r0 r0 0
    mov fh r0 r1 1
    add r0 r1 r0
    mov fh r0 r30
    ret
    ret
