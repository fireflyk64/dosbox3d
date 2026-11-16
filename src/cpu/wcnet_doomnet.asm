; DOOMNET.COM: the launcher of DOOM's network games (src/cpu/wcnet_doom.cpp, docs/doom.md).
;
; DOOM plays a network game through a driver: a program that stays in memory under it, holds a block the two share
; (doomcom: a command, a node, a packet) and answers an interrupt ("send this", "is there a packet").  id's own were
; IPXSETUP.EXE and SERSETUP.EXE; this is ours.  All it does itself is what must be done by a DOS program: keep the
; block's memory, give the rest back, wait (the machine running: the room is met while it waits) and run the game.
; Everything else is the emulator's, called by its callback instruction (FE 38 nn nn, the number written into this
; file when the emulator makes it: every "5A A5" below): who is in the room, the block's contents, the game's command
; line, the interrupt itself.
;
;   nasm -f bin -o doomnet.com wcnet_doomnet.asm        (scripts/doomnet-asm.sh makes wcnet_doomnet.h of it)

        bits 16
        org 100h

%macro LAYER 0
        db 0FEh, 38h, 5Ah, 0A5h
%endmacro

start:  cli
        mov sp, stack_top
        sti
        mov bx, (image_end - $$ + 100h + 15) >> 4
        mov ah, 4Ah
        int 21h                         ; the memory past us given back (ES our PSP still): the game's
        mov si, doomcom
        mov di, path
        mov dx, tail
        xor ax, ax
        LAYER                           ; 0: here we are (CS our segment; the block, the path's and the tail's places)
poll:   xor bx, bx
        mov ah, 1
        int 16h
        jz .ask
        xor ah, ah
        int 16h
        mov bx, ax
.ask:   mov ax, 1
        LAYER                           ; 1: what now (BX a key pressed, or 0)?  AL 0 wait, 1 run the game, 2 leave
        cmp al, 1
        je run
        cmp al, 2
        je leave
        hlt                             ; (until the next interrupt, the timer's at the latest)
        jmp poll

run:    mov word [parm], 0              ; the environment: ours
        mov word [parm + 2], tail
        mov [parm + 4], cs
        mov word [parm + 6], fcb
        mov [parm + 8], cs
        mov word [parm + 10], fcb
        mov [parm + 12], cs
        mov [keep_sp], sp
        mov dx, path                    ; DS:DX the program, ES:BX its parameters
        mov bx, parm
        mov ax, 4B00h
        int 21h
        mov bx, cs                      ; (DOS may give back nothing but CS:IP)
        mov ds, bx
        mov es, bx
        cli
        mov ss, bx
        mov sp, [keep_sp]
        sti
        mov bx, 0
        jnc .ran
        mov bx, ax                      ; DOS's error: the program not run
.ran:   mov ax, 2
        LAYER                           ; 2: the game is over (BX DOS's error, 0 none)
        jmp poll

leave:  mov ax, 4C00h
        int 21h

keep_sp dw 0
parm    times 14 db 0
fcb     times 40 db 0
path    times 128 db 0                  ; the program's name, ended by a zero
tail    times 130 db 0                  ; its command tail: the length, the text, a carriage return
        align 4
doomcom times 160 db 0                  ; the block DOOM and the driver share (140 bytes of it used)
        times 512 db 0
stack_top:
image_end:
