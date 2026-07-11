.global sadd, main

main:
    mov fh r0 r0 3
    wm fh r0 r0 0
    mov fh r0 r0 5
    wm fh r0 r0 4
    call sadd
    mov fh r30 r0
    wm fh r0 r0 12
    ret

sadd:
    rm fh r0 r0 0
    rm fh r0 r1 4
    add r0 r1 r0
    wm fh r0 r0 8
    rm fh r0 r0 8
    mov fh r0 r30
    ret
    ret
