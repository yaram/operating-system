.globl entry

entry:
    movb $'U', (0xB8000)
    ret