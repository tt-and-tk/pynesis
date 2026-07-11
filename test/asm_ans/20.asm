.global first_char, main

main:
    mov fh r0 r0 26984
    wm fh r0 r0 4
    mov fh r0 r0 4
    wm fh r0 r0 0
    call first_char
    mov fh r30 r0
    wm 1h r0 r0 8
    ret

first_char:
    mov fh r0 r0 0
    rm fh r0 r1 0
    add r0 r1 r0
    rm 1h r0 r0
    mov fh r0 r1 24
    sll r0 r1 r0
    sra r0 r1 r0
    mov fh r0 r30
    ret
    ret
