; pview.asm: the planet's picture without the card, its hot parts (pview.h, pview.c).  rv_pv_steps, rv_pv_lines,
; rv_pv_march, rv_pv_far and rv_pv_glow do what pview.c's c_pv_* do, to the bit (RVP.EXE /twins compares them):
; 64-bit products and quotients, each one instruction of a 386's where Watcom's 16-bit C makes a call of each.
; Far cdecl, as the medium model calls them; SI DI BP DS kept, FS GS too.  The frame (PvFrame) is copied onto the
; stack at the start (its arrays' places made offsets in their segments) and what changed copied back at the end;
; the arrays are in buf (ES: the picture, steps, lines, slab cells) and scr (GS: the level's window, its blocks'
; highest, the haze's and glow's tables).  In the march FS:BX is the step being sampled (buf's, or one between two
; on the stack) and ES:SI the line.
        cpu 486
; Assembled as a blob (nasm -f bin -dBLOB: PVIEW.BIN): position free (near jumps and calls only, data on the stack
; and in the segments the caller gives), kept in XMS and copied into the band buffer's tail for each frame, run from
; there by far calls through its head (pview.c): RV.EXE has no conventional memory to spare for 12 KB of code.
%ifdef BLOB
        bits 16
        org 0
        db 'PVB2'
        dw _rv_pv_steps, _rv_pv_lines, _rv_pv_march, _rv_pv_far, _rv_pv_glow, _rv_pv_setup, _rv_pv_quarter, _rv_pv_blit
        dw _rv_pv_fogt
        dw set_start, set_end, march_start, march_end, blit_start, blit_end, blob_end
; (three parts, each calling only within itself, each run from where pview.c puts it: the set-up's from the scratch
; buffer's start before any window is in it, the march's from the band buffer's tail, the blit's from the scratch
; buffer's tail at a band)
%define EXPORT(x)
%else
segment PVIEW_TEXT public align=16 class=CODE use16
%define EXPORT(x) global x
%endif

; --- PvFrame (pview.h), on the stack at bp-316 ---
%define FSZ 332
%define FR(x) [bp-FSZ+(x)]
%define O_FR(x) (-FSZ+(x))
%define F_R 0
%define F_D 4
%define F_RT 8
%define F_RB 12
%define F_UP 16
%define F_UE 28
%define F_UA 32
%define F_UB 36
%define F_PX 40
%define F_PY 44
%define F_LCX 48
%define F_LCY 52
%define F_LF 56
%define F_VX 60
%define F_VY 64
%define F_CR 68
%define F_CU 80
%define F_CF 92
%define F_E 104
%define F_A 116
%define F_B 128
%define F_FOG_H0 140
%define F_FOG_RCP 144
%define F_ALT 148
%define F_UNDER_DIV 152
%define F_SKF 156
%define F_SKR 160
%define F_SKU 164
%define F_NADIR 168
%define F_VSH 170
%define F_NSTEPS 172
%define F_NLINES 174
%define F_BX0 176
%define F_BY0 178
%define F_BX1 180
%define F_BY1 182
%define F_SHIFT0 184
%define F_NLEV 186
%define F_FAR_C 188
%define F_NSLAB 190
%define F_SLAB_SHIFT 192
%define F_GLOW_ANY 194
%define F_LEV 196
%define F_SH 198
%define F_OX 200
%define F_OY 204
%define F_MAX_STEPS 208
%define F_MAX_LINES 210
%define F_SLAB 212                      ; PvSlab [4]: x0 0, y0 4, w 8, h 10, at 12 (16 bytes)
%define F_SAMPLES 276
%define F_SEEN 280
%define F_SKIPS 284
%define F_SUBS 288
%define F_O_PIC 292
%define F_O_ST 294
%define F_O_LN 296
%define F_O_SLAB 298
%define F_O_WIN 300
%define F_O_BWIN 302
%define F_O_FOGLEV 304
%define F_O_FOGM 306
%define F_O_GLOW 308
%define F_O_UNDER 310
%define F_BUF 312
%define F_SCR 316
%define F_STEP_PX 320
%define F_WIDE 322
%define F_FOG_K 324
%define F_FOG_HS 328

; --- PvStep (28 bytes) ---
%define S_C 0
%define S_S 4
%define S_A 8
%define S_B 12
%define S_UC 16
%define S_DIST 20
%define S_DS 24
%define S_LEV 26
%define STEP_BYTES 28

; --- PvLine (64 bytes) ---
%define L_TE 0
%define L_KX 4
%define L_KY 8
%define L_HU 12
%define L_HT 16
%define L_COL 20
%define L_DCOL 24
%define L_RA 28                         ; (int16)
%define L_RB 30                         ; (int16)
%define L_US 32
%define L_TS 34
%define L_UF 36
%define L_TF 38
%define L_HORIZON 40
%define L_PRHO 42
%define L_PLO 44
%define L_PHI 46
%define L_PLIGHT 48
%define L_PFOG 50
%define L_PH 52
%define L_PDS 54
%define L_STEP 56
%define L_ROW 58
%define L_K0 59
%define L_K1 60
%define L_DROW 61
%define L_FLAGS 62
%define L_MAT 63
%define LINE_BYTES 64

%define LF_VERT 1
%define LF_DONE 2
%define LF_HID 4
%define LF_POK 8
%define LF_PSLAB 16
%define LF_PRECT 32

; --- PvQuarter ---
%define Q_ON 0
%define Q_VERT 2
%define Q_S 4
%define Q_INNER 6
%define Q_EDGE 8
%define Q_LEN 10
%define Q_FIRST 12
%define Q_LINES 14
%define Q_VA 16
%define Q_VC 20

; --- pview.h's sizes ---
%define PV_LW 128
%define PV_LH 80
%define PV_W 96
%define PV_BW 24
%define PV_CELL 3
%define PV_ABSENT 0x80
%define PV_EMPTY 0xFFFF
%define PV_GLOW 0xE000
%define PV_STEPS 256
%define PV_LINES 360
%define PV_FOG_HB 64
%define PV_FOG_GS 256
%define PV_FOG_TAUS 2048
%define PV_GLOW_CZ 256
%define PV_LIGHTS 32
%define PV_FOGS 32
%define PV_SLABS 4
%define PV_DEN_MIN 0x4000000
%define PV_NEAR_DS 192
%define PV_SUBS 16
%define PV_SUBS_WIDE 6

; --- the locals, below the frame: dwords L(1) .. ---
%define L(k) [bp-FSZ-4*(k)]
%define O_L(k) (-FSZ-4*(k))
%define NLOCAL 97                       ; dwords
%define EXTRA (-FSZ-4*NLOCAL-512)       ; 512 bytes, each function's own:
%define TSTEP EXTRA                     ;   the march's step between two (28 bytes, at bp+TSTEP)
%define QCOPY EXTRA                     ;   the lines' quarter (24 bytes)
%define THINC (EXTRA+32)                ;   and its thinning (128 bytes)
%define KMINS (EXTRA+160)               ;   and the first pixel for each m (13 words)
%define GLOWG EXTRA                     ;   the glow's grid of indices (11 x 17 words)
%define GLOWR (EXTRA+400)               ;   and a row's (17 words)
%define FRAME_ALL (FSZ+4*NLOCAL+512)

; the 64-bit dot product of two 2^30-or-so 3-vectors at bp+%1 and bp+%2, shifted right %3: eax (ebx ecx edx used;
; dotsh's)
%macro DOTSH 3
        push si
        push di
        mov si,%1
        mov di,%2
        mov cl,%3
        call dotsh
        pop di
        pop si
%endmacro

; the frame onto the stack, the arrays' places made offsets in their segments: es buf, gs scr
%macro PV_ENTER 0
        push bp
        mov bp,sp
        sub sp,FRAME_ALL
        push si
        push di
        push ds
        push fs
        push gs
        cld
        lds si,[bp+6]
        mov ax,ss
        mov es,ax
        lea di,[bp-FSZ]
        mov cx,FSZ/2
        rep movsw
        mov ds,ax
        les ax,FR(F_BUF)
        add FR(F_O_PIC),ax
        add FR(F_O_ST),ax
        add FR(F_O_LN),ax
        add FR(F_O_SLAB),ax
        lgs ax,FR(F_SCR)
        add FR(F_O_WIN),ax
        add FR(F_O_BWIN),ax
        add FR(F_O_FOGLEV),ax
        add FR(F_O_FOGM),ax
        add FR(F_O_GLOW),ax
        add FR(F_O_UNDER),ax
%endmacro

%macro PV_LEAVE 0
        pop gs
        pop fs
        pop ds
        pop di
        pop si
        mov sp,bp
        pop bp
        retf
%endmacro


; (profiling: nasm -dPROF: each part's cycles (RDTSC) and calls, at the scratch buffer's 32000)
%ifdef PROF
%macro PSTART 1
        push eax
        push edx
        push di
        mov di,FR(F_O_WIN)
        db 0Fh,31h
        mov [gs:di+32000+64+4*%1],eax
        pop di
        pop edx
        pop eax
%endmacro
%macro PEND 1
        push eax
        push edx
        push di
        mov di,FR(F_O_WIN)
        db 0Fh,31h
        sub eax,[gs:di+32000+64+4*%1]
        add [gs:di+32000+8*%1],eax
        inc dword [gs:di+32000+8*%1+4]
        pop di
        pop edx
        pop eax
%endmacro
%else
%macro PSTART 1
%endmacro
%macro PEND 1
%endmacro
%endif

set_start:
; the floor of the root of edx:eax (under 2^62) in eax: Newton's steps down from a power of two over the root
; (pview.c pv_isqrt's, bake.asm isqrt's).  ecx edx used
isqrt64:
        push ebx
        push esi
        push edi
        mov esi,eax
        mov edi,edx
        xor eax,eax
        mov ecx,edi
        or ecx,esi
        jz .done
        bsr ecx,edi
        jz .low
        add cl,32
        jmp .have
.low:   bsr ecx,esi
.have:  add cl,2
        shr cl,1
        mov ebx,0xFFFFFFFF
        cmp cl,32
        jae .step
        mov ebx,1
        shl ebx,cl
.step:  mov eax,esi
        mov edx,edi
        div ebx
        add eax,ebx
        rcr eax,1
        cmp eax,ebx
        jae .root
        mov ebx,eax
        jmp .step
.root:  mov eax,ebx
.done:  pop edi
        pop esi
        pop ebx
        ret

; eax = (the 3-vectors at bp+si . bp+di) >> cl (28 .. 34): DOTSH's (ebx ecx edx used)
dotsh:  push cx
        mov eax,[bp+si]
        imul dword [bp+di]
        mov ebx,eax
        mov ecx,edx
        mov eax,[bp+si+4]
        imul dword [bp+di+4]
        add ebx,eax
        adc ecx,edx
        mov eax,[bp+si+8]
        imul dword [bp+di+8]
        add eax,ebx
        adc edx,ecx
        pop cx
        cmp cl,32
        jae .hi
        shrd eax,edx,cl
        ret
.hi:    sub cl,32
        mov eax,edx
        sar eax,cl
        ret

; ================================================================================================================
; void rv_pv_steps(PvFrame far *f)
; ================================================================================================================
%define ST_C L(1)
%define ST_S L(2)
%define ST_N L(3)
%define ST_COSMAX L(4)
%define ST_A L(5)
%define ST_B L(6)
%define ST_DIST L(7)
%define ST_DS L(8)
%define ST_CD L(9)
%define ST_CT L(10)
%define ST_SD L(11)
%define ST_DT L(12)
%define ST_T0 L(13)
%define ST_T1 L(14)
        EXPORT(_rv_pv_steps)
_rv_pv_steps:
        PV_ENTER
        mov eax,FR(F_D)
        cmp eax,FR(F_RB)
        jg .cm
        mov dword ST_COSMAX,-0x40000000
        jmp .go
.cm:    mov eax,FR(F_RB)                ; cd = rb 2^30 / D, ct = rb 2^30 / rt
        cdq
        shld edx,eax,30
        shl eax,30
        idiv dword FR(F_D)
        mov ST_CD,eax
        mov eax,FR(F_RB)
        cdq
        shld edx,eax,30
        shl eax,30
        idiv dword FR(F_RT)
        mov ST_CT,eax
        mov eax,ST_CD                   ; sd = root(2^60 - cd^2)
        imul eax
        xor ebx,ebx
        mov ecx,0x10000000
        sub ebx,eax
        sbb ecx,edx
        mov eax,ebx
        mov edx,ecx
        call isqrt64
        mov ST_SD,eax
        mov eax,ST_CT                   ; stt = root(2^60 - ct^2)
        imul eax
        xor ebx,ebx
        mov ecx,0x10000000
        sub ebx,eax
        sbb ecx,edx
        mov eax,ebx
        mov edx,ecx
        call isqrt64
        mov ST_T0,eax
        mov eax,ST_CD                   ; cosmax = (cd ct - sd stt) >> 30
        imul dword ST_CT
        mov ebx,eax
        mov ecx,edx
        mov eax,ST_SD
        imul dword ST_T0
        sub ebx,eax
        sbb ecx,edx
        shrd ebx,ecx,30
        mov ST_COSMAX,ebx
.go:    mov dword ST_C,0x40000000
        mov dword ST_S,0
        mov dword ST_N,0
        mov di,FR(F_O_ST)
.loop:  inc dword ST_N
        mov eax,FR(F_R)                 ; a = (R c >> 30) - D
        imul dword ST_C
        shrd eax,edx,30
        sub eax,FR(F_D)
        mov ST_A,eax
        mov eax,FR(F_R)                 ; b = R s >> 30
        imul dword ST_S
        shrd eax,edx,30
        mov ST_B,eax
        mov eax,ST_A                    ; dist = root(a^2 + b^2)
        imul eax
        mov ebx,eax
        mov ecx,edx
        mov eax,ST_B
        imul eax
        add eax,ebx
        adc edx,ecx
        call isqrt64
        mov ST_DIST,eax
        movsx ecx,word FR(F_STEP_PX)    ; ds = dist step_px 256 / lf, at least PV_NEAR_DS
        shl ecx,8
        imul ecx
        idiv dword FR(F_LF)
        cmp eax,PV_NEAR_DS
        jge .ds1
        mov eax,PV_NEAR_DS
.ds1:   mov ST_DS,eax
        mov eax,ST_A                    ; g = -((a c + b s) >> 30)
        imul dword ST_C
        mov ebx,eax
        mov ecx,edx
        mov eax,ST_B
        imul dword ST_S
        add ebx,eax
        adc ecx,edx
        shrd ebx,ecx,30
        neg ebx
        cmp ebx,ST_DIST
        jge .nog
        mov eax,ST_DIST                 ; q = g > dist / 4 ? g : dist / 4, at least 1
        sar eax,2
        cmp ebx,eax
        jg .q1
        mov ebx,eax
.q1:    cmp ebx,0
        jg .q2
        mov ebx,1
.q2:    mov eax,ST_DS                   ; ds = ds dist / q
        imul dword ST_DIST
        idiv ebx
        mov ST_DS,eax
.nog:   mov eax,ST_C
        mov [es:di+S_C],eax
        mov eax,ST_S
        mov [es:di+S_S],eax
        mov cl,FR(F_VSH)
        mov eax,ST_A
        sar eax,cl
        mov [es:di+S_A],eax
        mov eax,ST_B
        sar eax,cl
        mov [es:di+S_B],eax
        mov eax,FR(F_UE)
        imul dword ST_C
        shrd eax,edx,30
        mov [es:di+S_UC],eax
        mov eax,ST_DIST
        mov [es:di+S_DIST],eax
        mov eax,ST_DS
        cmp eax,32767
        jle .d16
        mov eax,32767
.d16:   mov [es:di+S_DS],ax
        xor bx,bx                       ; lev: the least l (under nlev - 1) whose 2 << (shift0 + l) is ds or more
        mov dx,FR(F_NLEV)
        dec dx
.lv:    cmp bx,dx
        jge .lvd
        mov cl,FR(F_SHIFT0)
        add cl,bl
        mov eax,2
        shl eax,cl
        cmp eax,ST_DS
        jge .lvd
        inc bx
        jmp .lv
.lvd:   mov [es:di+S_LEV],bl
        mov byte [es:di+S_LEV+1],0
        mov eax,ST_C
        cmp eax,ST_COSMAX
        jle .end
        mov ax,ST_N
        cmp ax,FR(F_MAX_STEPS)
        jge .end
        mov eax,ST_DS                   ; dt = ds 2^30 / R
        cdq
        shld edx,eax,30
        shl eax,30
        idiv dword FR(F_R)
        mov ST_DT,eax
        imul eax                        ; dt^2
        mov ebx,eax
        mov ecx,edx
        mov eax,ebx                     ; cd = 2^30 - (dt^2 >> 31)
        shrd eax,ecx,31
        mov edx,0x40000000
        sub edx,eax
        mov ST_CD,edx
        mov eax,ebx                     ; sd = dt - (((dt^2 >> 30) dt >> 30) / 6)
        shrd eax,ecx,30
        imul dword ST_DT
        shrd eax,edx,30
        cdq
        mov ecx,6
        idiv ecx
        mov edx,ST_DT
        sub edx,eax
        mov ST_SD,edx
        mov eax,ST_C                    ; x = (c cd - s sd) >> 30
        imul dword ST_CD
        mov ebx,eax
        mov ecx,edx
        mov eax,ST_S
        imul dword ST_SD
        sub ebx,eax
        sbb ecx,edx
        shrd ebx,ecx,30
        mov ST_T1,ebx
        mov eax,ST_S                    ; s = (s cd + c sd) >> 30
        imul dword ST_CD
        mov ebx,eax
        mov ecx,edx
        mov eax,ST_C
        imul dword ST_SD
        add eax,ebx
        adc edx,ecx
        shrd eax,edx,30
        mov ST_S,eax
        mov eax,ST_T1
        mov ST_C,eax
        add di,STEP_BYTES
        jmp .loop
.end:   mov ax,ST_N
        lfs bx,[bp+6]
        mov [fs:bx+F_NSTEPS],ax
        PV_LEAVE

; ================================================================================================================
; void rv_pv_lines(PvFrame far *f, const PvQuarter far *q, const uint8_t far *thin)
; ================================================================================================================
%define LN_I L(1)
%define LN_X L(2)
%define LN_E8 L(3)
%define LN_C0 L(4)
%define LN_COL L(5)
%define LN_DCOL L(6)
%define LN_K0 L(7)
%define LN_K1 L(8)
%define LN_KMIN L(9)
%define LN_LO L(10)
%define LN_HI L(11)
%define LN_WUP L(12)
%define LN_T0 L(13)                     ; T: L(13) .. L(15)
%define O_LN_T O_L(15)
%define O_LN_TT O_L(18)                 ; t: L(18) .. L(16)
%define LN_TT0 L(18)
%define LN_TT1 L(17)
%define LN_TT2 L(16)
%define LN_TL L(19)
%define LN_TA L(20)
%define LN_TB L(21)
%define LN_KH L(22)
%define LN_PTR L(23)                    ; the line's place (word)
%define O_RAY O_L(26)                   ; ray's out: L(26) .. L(24)
%define RAY0 L(26)
%define RAY1 L(25)
%define RAY2 L(24)
%define RX_ L(27)
%define RY_ L(28)
%define EN_WU L(29)
%define EN_WT L(30)
%define EN_NN L(31)
%define EN_B L(32)
%define EN_SX L(33)
%define EN_QU L(34)
%define EN_QT L(35)
%define LN_C0V L(36)
%define LN_C1V L(37)
%define LN_A0 L(38)
%define LN_A1 L(39)
%define LN_CV L(40)                     ; (word: the frame's offset of cu or cr)
%define QV(x) [bp+QCOPY+(x)]

; RAY = the ray of the picture point (eax, edx), 256ths of a pixel: (cf 65536 + cr rx - cu ry) >> 6, each part
ray:    push edx
        sub eax,FR(F_LCX)
        cdq
        shld edx,eax,16
        shl eax,16
        idiv dword FR(F_LF)
        mov RX_,eax
        pop eax
        sub eax,FR(F_LCY)
        cdq
        shld edx,eax,16
        shl eax,16
        idiv dword FR(F_LF)
        mov RY_,eax
%assign k 0
%rep 3
        mov eax,FR(F_CR+4*k)
        imul dword RX_
        mov ebx,eax
        mov ecx,edx
        mov eax,FR(F_CU+4*k)
        imul dword RY_
        sub ebx,eax
        sbb ecx,edx
        mov eax,FR(F_CF+4*k)
        cdq
        shld edx,eax,16
        shl eax,16
        add ebx,eax
        adc ecx,edx
        shrd ebx,ecx,6
        mov [bp+O_RAY+4*k],ebx
%assign k k+1
%endrep
        ret

; eax = the first step at or past where a ray along (EN_WU, EN_WT) reaches the top sphere, -1 when never (pv_enter;
; si di used)
penter: mov eax,EN_WU
        imul eax
        mov ebx,eax
        mov ecx,edx
        mov eax,EN_WT
        imul eax
        add eax,ebx
        adc edx,ecx
        call isqrt64
        test eax,eax
        jz .no
        mov EN_NN,eax
        mov eax,FR(F_D)                 ; b = D wu / n
        imul dword EN_WU
        idiv dword EN_NN
        mov EN_B,eax
        imul eax                        ; disc = b^2 - (D^2 - rt^2)
        mov ebx,eax
        mov ecx,edx
        mov eax,FR(F_D)
        imul eax
        sub ebx,eax
        sbb ecx,edx
        mov eax,FR(F_RT)
        imul eax
        add ebx,eax
        adc ecx,edx
        test ecx,ecx
        js .no
        jnz .pos
        test ebx,ebx
        jz .no
.pos:   mov eax,ebx
        mov edx,ecx
        call isqrt64
        mov ecx,EN_B                    ; sx = -b - root
        neg ecx
        sub ecx,eax
        js .no
        mov EN_SX,ecx
        mov eax,ecx                     ; qu = D + sx wu / n, qt = sx wt / n
        imul dword EN_WU
        idiv dword EN_NN
        add eax,FR(F_D)
        mov EN_QU,eax
        mov eax,EN_SX
        imul dword EN_WT
        idiv dword EN_NN
        mov EN_QT,eax
        xor si,si                       ; lo
        mov di,FR(F_NSTEPS)             ; hi
.bs:    cmp si,di
        jge .bsd
        mov ax,si
        add ax,di
        shr ax,1                        ; mid
        push ax
        mov bx,STEP_BYTES
        mul bx
        add ax,FR(F_O_ST)
        mov bx,ax
        mov eax,[es:bx+S_S]             ; s qu - c qt >= 0: hi = mid, else lo = mid + 1
        imul dword EN_QU
        mov LN_LO,eax
        mov LN_HI,edx
        mov eax,[es:bx+S_C]
        imul dword EN_QT
        sub LN_LO,eax
        sbb LN_HI,edx
        pop ax
        cmp dword LN_HI,0
        jl .lo
        mov di,ax
        jmp .bs
.lo:    inc ax
        mov si,ax
        jmp .bs
.bsd:   movsx eax,si
        ret
.no:    mov eax,-1
        ret

; floor(eax / ecx), ecx > 0: eax (edx used)
floordiv:
        cdq
        idiv ecx
        test edx,edx
        jns .ok
        dec eax
.ok:    ret
; ceil(eax / ecx), ecx > 0
ceildiv:
        cdq
        idiv ecx
        test edx,edx
        jle .ok
        inc eax
.ok:    ret

; the pixel along (eax: k) of the quarter's line as a ray: RAY (the line's col, dcol in LN_COL, LN_DCOL)
kray:   push eax
        movsx edx,word QV(Q_S)          ; along = (near + s k) 256
        imul edx,eax
        movsx eax,word QV(Q_INNER)
        add edx,eax
        shl edx,8
        pop eax
        imul eax,LN_DCOL                ; across = (col - 32768 + k dcol) >> 8
        add eax,LN_COL
        sub eax,32768
        sar eax,8
        cmp word QV(Q_VERT),0
        jne .v
        xchg eax,edx
.v:     jmp ray

        EXPORT(_rv_pv_lines)
_rv_pv_lines:
        PV_ENTER
        push ds                         ; the quarter and its thinning onto the stack
        lds si,[bp+10]
        mov ax,ss
        mov es,ax
        lea di,[bp+QCOPY]
        mov cx,12
        rep movsw
        lds si,[bp+14]
        lea di,[bp+THINC]
        mov cx,64
        rep movsw
        pop ds
        les ax,FR(F_BUF)                ; (es buf again)
        xor bx,bx                       ; kmin for each m (12 down to 0): the first k whose thinning is m or less
        mov cx,12                       ; (the thinning falls away from V: kmin grows as m falls)
        lea di,[bp+THINC]
.kt:    cmp bx,QV(Q_LEN)
        jge .kt1
        cmp [bx+di],cl
        jbe .kt1
        inc bx
        jmp .kt
.kt1:   push bx
        mov bx,cx
        shl bx,1
        pop ax
        lea si,[bp+KMINS]
        mov [bx+si],ax
        mov bx,ax
        dec cx
        jns .kt
        mov ax,O_FR(F_CU)
        cmp word QV(Q_VERT),0
        jne .cv
        mov ax,O_FR(F_CR)
.cv:    mov LN_CV,ax
        mov dword LN_I,0
.next:  mov eax,LN_I
        cmp ax,QV(Q_LINES)
        jl .more
        jmp .done
.more:  mov ax,FR(F_NLINES)
        cmp ax,FR(F_MAX_LINES)
        jl .room
        jmp .done
.room:  cmp word FR(F_WIDE),0            ; (wide: every other line, each filling its neighbour's pixels too)
        je .room1
        test byte LN_I,1
        jz .room1
        jmp .skip
.room1: mov ax,FR(F_NLINES)             ; the line's place
        mov bx,LINE_BYTES
        mul bx
        add ax,FR(F_O_LN)
        mov LN_PTR,ax
        movsx eax,word QV(Q_FIRST)      ; X = (first + i) 256
        add eax,LN_I
        shl eax,8
        mov LN_X,eax
        movsx eax,word QV(Q_EDGE)       ; e8 = edge 256 - va
        shl eax,8
        sub eax,QV(Q_VA)
        mov LN_E8,eax
        mov eax,LN_I                    ; m: i's trailing zero bits, 12 at most
        xor cx,cx
.tz:    cmp cx,12
        jge .tzd
        test ax,1
        jnz .tzd
        shr ax,1
        inc cx
        jmp .tz
.tzd:   mov bx,cx                       ; kmin: the first k whose thinning is m or less (the quarter's table)
        shl bx,1
        lea si,[bp+KMINS]
        movzx ebx,word [bx+si]
        cmp bx,QV(Q_LEN)
        jl .kmd
        jmp .skip
.kmd:   mov LN_KMIN,ebx
        mov eax,LN_X                    ; c0 = vc + (X - vc) (near 256 - va) / e8
        sub eax,QV(Q_VC)
        movsx ecx,word QV(Q_INNER)
        shl ecx,8
        sub ecx,QV(Q_VA)
        imul ecx
        idiv dword LN_E8
        add eax,QV(Q_VC)
        mov LN_C0,eax
        shl eax,8                       ; col = c0 256 + 32768
        add eax,32768
        mov LN_COL,eax
        mov eax,LN_X                    ; dcol = (X - vc) s 65536 / e8
        sub eax,QV(Q_VC)
        movsx ecx,word QV(Q_S)
        shl ecx,16
        imul ecx
        idiv dword LN_E8
        mov LN_DCOL,eax
        ; the pixels wanted
        movsx eax,word FR(F_BY0)
        movsx ecx,word FR(F_BY1)
        movsx edx,word FR(F_BX0)
        movsx ebx,word FR(F_BX1)
        cmp word QV(Q_VERT),0
        jne .w1
        xchg eax,edx
        xchg ecx,ebx
.w1:    mov LN_A0,eax
        mov LN_A1,ecx
        shl edx,16
        mov LN_C0V,edx
        shl ebx,16
        dec ebx
        mov LN_C1V,ebx
        mov eax,LN_KMIN
        mov LN_K0,eax
        movsx eax,word QV(Q_LEN)
        dec eax
        mov LN_K1,eax
        movsx ecx,word QV(Q_INNER)
        cmp word QV(Q_S),0
        jl .sneg
        mov eax,LN_A0                   ; lo = a0 - near, hi = a1 - 1 - near
        sub eax,ecx
        mov edx,LN_A1
        dec edx
        sub edx,ecx
        jmp .clip1
.sneg:  mov eax,ecx                     ; lo = near - (a1 - 1), hi = near - a0
        mov edx,LN_A1
        dec edx
        sub eax,edx
        mov edx,ecx
        sub edx,LN_A0
.clip1: call .clip
        mov ecx,LN_DCOL
        cmp ecx,0
        jle .dneg
        mov eax,LN_C0V                  ; lo = ceil((c0v - col) / dcol), hi = floor((c1v - col) / dcol)
        sub eax,LN_COL
        call ceildiv
        push eax
        mov eax,LN_C1V
        sub eax,LN_COL
        mov ecx,LN_DCOL
        call floordiv
        mov edx,eax
        pop eax
        jmp .clip2
.dneg:  jz .dz
        neg ecx                         ; lo = ceil((col - c1v) / -dcol), hi = floor((col - c0v) / -dcol)
        mov eax,LN_COL
        sub eax,LN_C1V
        call ceildiv
        push eax
        mov eax,LN_COL
        sub eax,LN_C0V
        mov ecx,LN_DCOL
        neg ecx
        call floordiv
        mov edx,eax
        pop eax
        jmp .clip2
.dz:    mov eax,LN_COL
        cmp eax,LN_C0V
        jl .dzn
        cmp eax,LN_C1V
        jg .dzn
        mov eax,LN_K0
        mov edx,LN_K1
        jmp .clip2
.dzn:   mov eax,1
        xor edx,edx
.clip2: call .clip
        mov eax,LN_K0
        cmp eax,LN_K1
        jle .wanted
        jmp .skip
        ; (k0 = max(k0, eax), k1 = min(k1, edx))
.clip:  cmp eax,LN_K0
        jle .c1
        mov LN_K0,eax
.c1:    cmp edx,LN_K1
        jge .c2
        mov LN_K1,edx
.c2:    ret
.wanted:
        ; t: the ray at the edge's crossing less its part along up, made unit
        mov eax,LN_X
        movsx edx,word QV(Q_EDGE)
        shl edx,8
        cmp word QV(Q_VERT),0
        jne .w2
        xchg eax,edx
.w2:    call ray
        DOTSH O_RAY,O_FR(F_UP),30
        mov LN_WUP,eax
%assign k 0
%rep 3
        mov eax,FR(F_UP+4*k)
        imul dword LN_WUP
        shrd eax,edx,30
        mov ecx,[bp+O_RAY+4*k]
        sub ecx,eax
        mov [bp+O_LN_T+4*k],ecx
%assign k k+1
%endrep
        mov eax,[bp+O_LN_T]
        imul eax
        mov ebx,eax
        mov ecx,edx
        mov eax,[bp+O_LN_T+4]
        imul eax
        add ebx,eax
        adc ecx,edx
        mov eax,[bp+O_LN_T+8]
        imul eax
        add eax,ebx
        adc edx,ecx
        call isqrt64
        cmp eax,16
        jge .tl
        jmp .skip
.tl:    mov LN_TL,eax
%assign k 0
%rep 3
        mov eax,[bp+O_LN_T+4*k]
        cdq
        shld edx,eax,30
        shl eax,30
        idiv dword LN_TL
        mov [bp+O_LN_TT+4*k],eax
%assign k k+1
%endrep
        mov di,LN_PTR
        DOTSH O_LN_TT,O_FR(F_E),30      ; te, ta, tb
        mov [es:di+L_TE],eax
        DOTSH O_LN_TT,O_FR(F_A),30
        mov LN_TA,eax
        DOTSH O_LN_TT,O_FR(F_B),30
        mov LN_TB,eax
        mov eax,LN_TA                   ; qx = (ta ue - ua te) >> 30, kx = R qx / ue
        imul dword FR(F_UE)
        mov ebx,eax
        mov ecx,edx
        mov eax,FR(F_UA)
        imul dword [es:di+L_TE]
        sub ebx,eax
        sbb ecx,edx
        shrd ebx,ecx,30
        mov eax,FR(F_R)
        imul ebx
        idiv dword FR(F_UE)
        mov [es:di+L_KX],eax
        mov eax,LN_TB                   ; qy, ky
        imul dword FR(F_UE)
        mov ebx,eax
        mov ecx,edx
        mov eax,FR(F_UB)
        imul dword [es:di+L_TE]
        sub ebx,eax
        sbb ecx,edx
        shrd ebx,ecx,30
        mov eax,FR(F_R)
        imul ebx
        idiv dword FR(F_UE)
        mov [es:di+L_KY],eax
        ; us ts (up and t along the camera's cu or cr), uf tf (along cf): >> 34
        mov si,LN_CV
%assign k 0
%rep 3
        mov eax,FR(F_UP+4*k)
        imul dword [bp+si+4*k]
%if k == 0
        mov ebx,eax
        mov ecx,edx
%else
        add ebx,eax
        adc ecx,edx
%endif
%assign k k+1
%endrep
        sar ecx,2
        mov [es:di+L_US],cx
%assign k 0
%rep 3
        mov eax,[bp+O_LN_TT+4*k]
        imul dword [bp+si+4*k]
%if k == 0
        mov ebx,eax
        mov ecx,edx
%else
        add ebx,eax
        adc ecx,edx
%endif
%assign k k+1
%endrep
        sar ecx,2
        mov [es:di+L_TS],cx
        DOTSH O_FR(F_UP),O_FR(F_CF),34
        mov [es:di+L_UF],ax
        DOTSH O_LN_TT,O_FR(F_CF),34
        mov [es:di+L_TF],ax
        movsx ecx,word QV(Q_INNER)       ; ra_ = s (lc - near 256), rb_ = -+ s lf
        shl ecx,8
        movsx edx,word QV(Q_S)
        cmp word QV(Q_VERT),0
        je .rh
        mov eax,FR(F_LCY)
        sub eax,ecx
        imul eax,edx
        mov [es:di+L_RA],ax
        mov eax,FR(F_LF)
        imul eax,edx
        neg eax
        mov [es:di+L_RB],ax
        jmp .rd
.rh:    mov eax,FR(F_LCX)
        sub eax,ecx
        imul eax,edx
        mov [es:di+L_RA],ax
        mov eax,FR(F_LF)
        imul eax,edx
        mov [es:di+L_RB],ax
.rd:    mov eax,LN_COL
        mov [es:di+L_COL],eax
        mov eax,LN_DCOL
        mov [es:di+L_DCOL],eax
        mov al,QV(Q_INNER)
        mov [es:di+L_ROW],al
        mov al,QV(Q_S)
        mov [es:di+L_DROW],al
        mov al,LN_K0
        mov [es:di+L_K0],al
        mov al,LN_K1
        mov [es:di+L_K1],al
        mov al,LF_HID
        cmp word QV(Q_VERT),0
        je .fl
        or al,LF_VERT
.fl:    mov [es:di+L_FLAGS],al
        mov byte [es:di+L_MAT],0
        xor eax,eax
        mov [es:di+L_PRHO],eax          ; prho plo phi plight pfog ph pds
        mov [es:di+L_PHI],eax
        mov [es:di+L_PFOG],eax
        mov [es:di+L_PDS],ax
        ; the horizon: the ray just short of the first pixel wanted (past the last, looking up at the ground)
        mov eax,LN_K0
        dec eax
        cmp word FR(F_NADIR),0
        jne .kh
        mov eax,LN_K1
        inc eax
.kh:    mov LN_KH,eax
        call kray
        DOTSH O_RAY,O_FR(F_UP),30
        mov di,LN_PTR
        mov [es:di+L_HU],eax
        DOTSH O_RAY,O_LN_TT,30
        mov [es:di+L_HT],eax
        mov eax,LN_KH
        shl eax,8
        mov [es:di+L_HORIZON],ax
        mov word [es:di+L_STEP],0
        mov eax,FR(F_D)
        cmp eax,FR(F_RT)
        jle .counted
        ; above every hill: nothing before the lowest of the line's rays reaches the highest ground can show
        mov eax,LN_K0
        cmp word FR(F_NADIR),0
        jne .kl
        mov eax,LN_K1
.kl:    call kray
        DOTSH O_RAY,O_FR(F_UP),30
        mov EN_WU,eax
        DOTSH O_RAY,O_LN_TT,30
        mov EN_WT,eax
        call penter
        cmp eax,0
        jl .skip
        mov di,LN_PTR
        mov [es:di+L_STEP],ax
.counted:
        inc word FR(F_NLINES)
.skip:  inc dword LN_I
        jmp .next
.done:  mov ax,FR(F_NLINES)
        lfs bx,[bp+6]
        mov [fs:bx+F_NLINES],ax
        PV_LEAVE

; ================================================================================================================
; int rv_pv_setup(PvFrame far *f, const PvCam far *cam, const PvHead far *h): the frame's numbers (pv_setup): ax 1,
; or 0 when there is no ground to draw
; ================================================================================================================
%define C_EYE 0                         ; PvCam
%define C_M 12
%define C_FOCAL 30
%define C_CX 32
%define C_CY 34
%define C_X0 36
%define C_Y0 38
%define C_X1 40
%define C_Y1 42
%define H_E 4                           ; PvHead
%define H_RADIUS 40
%define H_NLEV 44
%define H_SHIFT0 45
%define H_TOP 48
%define H_BOTTOM 50
%define H_FARC 52
%define SU_E L(1)                       ; e: L(3) .. L(1)
%define O_SU_E O_L(3)
%define SU_NX L(4)
%define SU_NY L(5)
%define SU_NZ L(6)
%define SU_MAXD L(7)
%define SU_RES L(8)
%define SU_T0 L(9)
%define SU_T1 L(10)

; a far-off V's place: lc +- lf n / nz held to 2^28 (eax n, ecx: 1 for +, -1 for -): eax
%macro SU_V 2
        mov eax,FR(F_LF)
        imul dword %1                   ; lf n
        ; |lf n| >= |nz| 2^30: past 2^28 whatever lc is
        mov SU_T0,eax
        mov SU_T1,edx
        test edx,edx
        jns %%pos
        not eax
        not edx
        add eax,1
        adc edx,0
%%pos:  mov ecx,SU_NZ
        mov ebx,ecx
        sar ebx,31
        xor ecx,ebx
        sub ecx,ebx                     ; |nz|
                                        ; (|lf n| in edx:eax; |nz| 2^30 is ecx >> 2 : ecx << 30)
        mov ebx,ecx
        shr ebx,2
        cmp edx,ebx
        ja %%huge
        jb %%div
        mov ebx,ecx
        shl ebx,30
        cmp eax,ebx
        jae %%huge
%%div:  mov eax,SU_T0
        mov edx,SU_T1
        idiv dword SU_NZ
%if %2 > 0
        add eax,FR(F_LCX)
%else
        neg eax
        add eax,FR(F_LCY)
%endif
        cmp eax,0x10000000
        jle %%c1
        mov eax,0x10000000
%%c1:   cmp eax,-0x10000000
        jge %%done
        mov eax,-0x10000000
        jmp %%done
%%huge: mov eax,SU_T1                   ; the quotient's sign: lf n's and nz's
        xor eax,SU_NZ
%if %2 < 0
        not eax
%endif
        test eax,eax
        mov eax,0x10000000
        jns %%done
        neg eax
%%done:
%endmacro

        EXPORT(_rv_pv_setup)
_rv_pv_setup:
        PV_ENTER
        mov dword SU_RES,0
        xor eax,eax
        mov FR(F_SAMPLES),eax
        mov FR(F_SEEN),eax
        mov FR(F_SKIPS),eax
        mov FR(F_SUBS),eax
        mov FR(F_NSTEPS),eax            ; (nsteps, nlines)
        mov FR(F_PX),eax
        mov FR(F_PY),eax
        lfs bx,[bp+14]                  ; the head: the radius, the plane's ways
        mov eax,[fs:bx+H_RADIUS]
        mov FR(F_R),eax
%assign k 0
%rep 9
        mov eax,[fs:bx+H_E+4*k]
        mov FR(F_E+4*k),eax
%assign k k+1
%endrep
        lfs bx,[bp+10]                  ; the camera: e = eye / 16, its ways
%assign k 0
%rep 3
        mov eax,[fs:bx+C_EYE+4*k]
        cdq
        mov ecx,16
        idiv ecx
        mov [bp+O_SU_E+4*k],eax
%assign k k+1
%endrep
%assign k 0
%rep 9
        movsx eax,word [fs:bx+C_M+2*k]
        mov FR(F_CR+4*k),eax
%assign k k+1
%endrep
        mov eax,[bp+O_SU_E]             ; D = root(e . e)
        imul eax
        mov ebx,eax
        mov ecx,edx
        mov eax,[bp+O_SU_E+4]
        imul eax
        add ebx,eax
        adc ecx,edx
        mov eax,[bp+O_SU_E+8]
        imul eax
        add eax,ebx
        adc edx,ecx
        call isqrt64
        mov FR(F_D),eax
        test eax,eax
        jg .d
        jmp .out
.d:
%assign k 0
%rep 3
        mov eax,[bp+O_SU_E+4*k]         ; up = e 2^30 / D
        cdq
        shld edx,eax,30
        shl eax,30
        idiv dword FR(F_D)
        mov FR(F_UP+4*k),eax
%assign k k+1
%endrep
        lfs bx,[bp+14]
        movsx eax,word [fs:bx+H_TOP]    ; rt = R + top 4 + 16, rb = R + bottom 4 - 16
        shl eax,2
        add eax,FR(F_R)
        add eax,16
        mov FR(F_RT),eax
        movsx eax,word [fs:bx+H_BOTTOM]
        shl eax,2
        add eax,FR(F_R)
        sub eax,16
        mov FR(F_RB),eax
        mov eax,FR(F_D)                 ; alt = (D - R) / 4
        sub eax,FR(F_R)
        cdq
        mov ecx,4
        idiv ecx
        mov FR(F_ALT),eax
        DOTSH O_FR(F_UP),O_FR(F_E),30
        mov FR(F_UE),eax
        DOTSH O_FR(F_UP),O_FR(F_A),30
        mov FR(F_UA),eax
        DOTSH O_FR(F_UP),O_FR(F_B),30
        mov FR(F_UB),eax
        cmp dword FR(F_UE),0x8000000    ; the eye's point on the plane
        jle .nop
        mov eax,FR(F_R)
        imul dword FR(F_UA)
        idiv dword FR(F_UE)
        mov FR(F_PX),eax
        mov eax,FR(F_R)
        imul dword FR(F_UB)
        idiv dword FR(F_UE)
        mov FR(F_PY),eax
.nop:   DOTSH O_FR(F_UP),O_FR(F_CR),30  ; V: nx, ny, nz = -(up . c) >> 30
        neg eax
        mov SU_NX,eax
        DOTSH O_FR(F_UP),O_FR(F_CU),30
        neg eax
        mov SU_NY,eax
        DOTSH O_FR(F_UP),O_FR(F_CF),30
        neg eax
        jnz .nz
        mov eax,1
.nz:    mov SU_NZ,eax
        xor cx,cx
        cmp eax,0
        jle .nd
        inc cx
.nd:    mov FR(F_NADIR),cx
        lfs bx,[bp+10]
        movsx eax,word [fs:bx+C_CX]     ; lcx = (cx - 2) 256 / 5, lcy, lf = focal 256 / 5
        sub eax,2
        shl eax,8
        cdq
        mov ecx,5
        idiv ecx
        mov FR(F_LCX),eax
        movsx eax,word [fs:bx+C_CY]
        sub eax,2
        shl eax,8
        cdq
        mov ecx,5
        idiv ecx
        mov FR(F_LCY),eax
        movsx eax,word [fs:bx+C_FOCAL]
        shl eax,8
        cdq
        mov ecx,5
        idiv ecx
        mov FR(F_LF),eax
        ; the box: x0 / 5, y0 / 5, (x1 + 4) / 5, (y1 + 4) / 5, within the picture
%macro SU_BOX 4
        movsx eax,word [fs:bx+%1]
        add eax,%2
        cdq
        mov ecx,5
        idiv ecx
        cmp eax,0
        jge %%a
        xor eax,eax
%%a:    cmp eax,%3
        jle %%b
        mov eax,%3
%%b:    mov FR(%4),ax
%endmacro
        SU_BOX C_X0,0,0x7FFF,F_BX0
        SU_BOX C_Y0,0,0x7FFF,F_BY0
        SU_BOX C_X1,4,PV_LW,F_BX1
        SU_BOX C_Y1,4,PV_LH,F_BY1
        SU_V SU_NX,1
        mov FR(F_VX),eax
        SU_V SU_NY,-1
        mov FR(F_VY),eax
        ; the vertical numbers' scale: maxd = (D > rb ? root(D^2 - rb^2) : 0) + root(rt^2 - rb^2) + 65536
        xor eax,eax
        mov ecx,FR(F_D)
        cmp ecx,FR(F_RB)
        jle .m1
        mov eax,ecx
        imul eax
        mov ebx,eax
        mov ecx,edx
        mov eax,FR(F_RB)
        imul eax
        sub ebx,eax
        sbb ecx,edx
        mov eax,ebx
        mov edx,ecx
        call isqrt64
.m1:    mov SU_MAXD,eax
        mov eax,FR(F_RT)
        imul eax
        mov ebx,eax
        mov ecx,edx
        mov eax,FR(F_RB)
        imul eax
        sub ebx,eax
        sbb ecx,edx
        mov eax,ebx
        mov edx,ecx
        call isqrt64
        add eax,SU_MAXD
        add eax,65536
        xor cx,cx
.vs:    mov edx,eax
        shr edx,cl
        cmp edx,0x100000
        jle .vsd
        inc cx
        jmp .vs
.vsd:   mov FR(F_VSH),cx
        cmp cx,3
        jg .out
        lfs bx,[bp+14]
        movzx ax,byte [fs:bx+H_SHIFT0]
        mov FR(F_SHIFT0),ax
        movzx ax,byte [fs:bx+H_NLEV]
        mov FR(F_NLEV),ax
        movzx ax,byte [fs:bx+H_FARC]
        mov FR(F_FAR_C),ax
        DOTSH O_FR(F_UP),O_FR(F_CF),28  ; the glow's: up along the camera's front, right, up, 65536ths
        mov FR(F_SKF),eax
        DOTSH O_FR(F_UP),O_FR(F_CR),28
        mov FR(F_SKR),eax
        DOTSH O_FR(F_UP),O_FR(F_CU),28
        mov FR(F_SKU),eax
        mov dword SU_RES,1
.out:   push ds                         ; the frame's numbers back (its arrays' places as they were)
        les di,[bp+6]
        lea si,[bp-FSZ]
        mov ax,ss
        mov ds,ax
        mov cx,F_SLAB/2
        rep movsw
        lea si,[bp-FSZ+F_SAMPLES]
        mov di,[bp+6]
        add di,F_SAMPLES
        mov cx,8
        rep movsw
        pop ds
        mov ax,SU_RES
        PV_LEAVE

; ================================================================================================================
; void rv_pv_quarter(const PvFrame far *f, int which, PvQuarter far *q, uint8_t far *thin): a quarter of the
; picture round V and its lines' thinning near it (pv_quarter)
; ================================================================================================================
%define QU_VA L(1)
%define QU_VC L(2)
%define QU_AMAX L(3)
%define QU_CMAX L(4)
%define QU_EDGE L(5)
%define QU_INNER L(6)
%define QU_S L(7)
%define QU_EXT L(8)
%define QU_LO L(9)
%define QU_HI L(10)
%define QU_K L(11)
%define QU_A L(12)
%define QU_CC L(13)
%define QU_RN L(14)
%define QU_LEN L(15)
%define QU_FIRST L(16)
        EXPORT(_rv_pv_quarter)
_rv_pv_quarter:
        PV_ENTER
        lfs di,[bp+12]                  ; q
        mov word [fs:di+Q_ON],0
        mov ax,[bp+10]                  ; which: vertical (0, 1) or not; s -1 (0, 2) or 1
        xor cx,cx
        cmp ax,2
        jge .h
        inc cx
.h:     mov [fs:di+Q_VERT],cx
        mov edx,-1
        test ax,1
        jz .s
        mov edx,1
.s:     mov QU_S,edx
        mov [fs:di+Q_S],dx
        mov eax,FR(F_VY)
        mov ebx,FR(F_VX)
        mov ecx,PV_LH-1
        mov edx,PV_LW-1
        cmp word [fs:di+Q_VERT],0
        jne .v
        xchg eax,ebx
        xchg ecx,edx
.v:     mov QU_VA,eax
        mov [fs:di+Q_VA],eax
        mov QU_VC,ebx
        mov [fs:di+Q_VC],ebx
        mov QU_AMAX,ecx
        mov QU_CMAX,edx
        xor eax,eax                     ; edge
        cmp dword QU_S,0
        jl .e
        mov eax,QU_AMAX
.e:     mov QU_EDGE,eax
        mov [fs:di+Q_EDGE],ax
        mov eax,QU_VA                   ; inner: the row (column) nearest V on its far side, on the picture
        cmp dword QU_S,0
        jl .in
        sar eax,8
        inc eax
        jns .i1
        xor eax,eax
.i1:    cmp eax,QU_AMAX
        jle .ind
        jmp .off
.in:    neg eax
        sar eax,8
        neg eax
        dec eax
        cmp eax,QU_AMAX
        jle .i2
        mov eax,QU_AMAX
.i2:    cmp eax,0
        jge .ind
        jmp .off
.ind:   mov QU_INNER,eax
        mov [fs:di+Q_INNER],ax
        mov ecx,QU_AMAX                 ; len
        sub ecx,eax
        inc ecx
        cmp dword QU_S,0
        jg .l1
        mov ecx,eax
        inc ecx
.l1:    mov QU_LEN,ecx
        mov [fs:di+Q_LEN],cx
        mov eax,QU_EDGE                 ; ext = |edge 256 - va|
        shl eax,8
        sub eax,QU_VA
        cdq
        xor eax,edx
        sub eax,edx
        mov QU_EXT,eax
        mov eax,QU_VC                   ; its wedge misses the picture (V far off across it): off
        sub eax,QU_EXT
        mov ecx,QU_CMAX
        shl ecx,8
        cmp eax,ecx
        jle .m1
        jmp .off
.m1:    mov eax,QU_VC
        add eax,QU_EXT
        jge .m2
        jmp .off
.m2:
        mov dword QU_LO,0x7FFFFFFF
        mov dword QU_HI,-0x7FFFFFFF
        mov dword QU_K,0
.k:     mov eax,QU_INNER                ; the corners' lines' crossings at the edge
        cmp dword QU_K,2
        jl .k1
        mov eax,QU_EDGE
.k1:    shl eax,8
        mov QU_A,eax
        sub eax,QU_VA                   ; half = |a - va|
        cdq
        xor eax,edx
        sub eax,edx
        test byte QU_K,1
        jnz .k2
        neg eax
.k2:    add eax,QU_VC                   ; cc = vc -+ half, within the picture
        jns .k3
        xor eax,eax
.k3:    mov ecx,QU_CMAX
        shl ecx,8
        cmp eax,ecx
        jle .k4
        mov eax,ecx
.k4:    sub eax,QU_VC                   ; xs = vc + (cc - vc) (edge 256 - va) / (a - va)
        mov ecx,QU_A
        sub ecx,QU_VA
        mov edx,eax                     ; (|cc - vc| over |a - va|: the corner outside the wedge there, past its edge)
        sar edx,31
        mov ebx,eax
        xor ebx,edx
        sub ebx,edx
        mov edx,ecx
        sar edx,31
        push ecx
        xor ecx,edx
        sub ecx,edx
        cmp ebx,ecx
        pop ecx
        jbe .kd
        xor eax,ecx                     ; the sign: (cc - vc)'s, (a - va)'s and (edge 256 - va)'s
        mov ecx,QU_EDGE
        shl ecx,8
        sub ecx,QU_VA
        xor eax,ecx
        mov eax,QU_VC
        js .kn
        add eax,QU_EXT
        jmp .k5a
.kn:    sub eax,QU_EXT
        jmp .k5a
.kd:    push ecx
        mov ecx,QU_EDGE
        shl ecx,8
        sub ecx,QU_VA
        imul ecx
        pop ecx
        idiv ecx
        add eax,QU_VC
.k5a:
        cmp eax,QU_LO
        jge .k5
        mov QU_LO,eax
.k5:    cmp eax,QU_HI
        jle .k6
        mov QU_HI,eax
.k6:    inc dword QU_K
        cmp dword QU_K,4
        jl .k
        mov eax,QU_VC                   ; within the wedge
        sub eax,QU_EXT
        cmp QU_LO,eax
        jge .w1
        mov QU_LO,eax
.w1:    mov eax,QU_VC
        add eax,QU_EXT
        cmp QU_HI,eax
        jle .w2
        mov QU_HI,eax
.w2:    mov eax,QU_HI
        cmp eax,QU_LO
        jge .w3
        jmp .off
.w3:    mov eax,QU_LO                   ; first = (lo >> 8) - 1; lines = ceil((hi - first 256) / 256) + 2
        sar eax,8
        dec eax
        mov QU_FIRST,eax
        mov [fs:di+Q_FIRST],ax
        shl eax,8
        mov ecx,QU_HI
        sub ecx,eax
        neg ecx
        sar ecx,8
        neg ecx
        add ecx,2
        mov [fs:di+Q_LINES],cx
        mov eax,QU_INNER                ; rn = s (inner 256 - va)
        shl eax,8
        sub eax,QU_VA
        imul eax,QU_S
        mov QU_RN,eax
        lgs si,[bp+16]                  ; the thinning: a line of index with m trailing zero bits from where twice
        mov cx,12                       ; 2^m of the spacing reaches a pixel
        xor bx,bx
.t:     cmp bx,QU_LEN
        jge .td
        movzx eax,bx                    ; sp = k 256 + rn
        shl eax,8
        add eax,QU_RN
.t1:    test cx,cx                      ; while kk > 0 and sp >= (L + 2^kk - 1) >> kk: kk--
        jz .t2
        mov edx,1
        shl edx,cl
        dec edx
        add edx,QU_EXT
        sar edx,cl
        cmp eax,edx
        jl .t2
        dec cx
        jmp .t1
.t2:    cmp cx,12                       ; while kk < 12 and sp < (L + 2^(kk+1) - 1) >> (kk + 1): kk++
        jge .t3
        inc cx
        mov edx,1
        shl edx,cl
        dec edx
        add edx,QU_EXT
        sar edx,cl
        dec cx
        cmp eax,edx
        jge .t3
        inc cx
        jmp .t2
.t3:    mov [gs:si+bx],cl
        inc bx
        jmp .t
.td:    lfs di,[bp+12]
        mov word [fs:di+Q_ON],1
.off:   PV_LEAVE

; ================================================================================================================
; void rv_pv_glow(PvFrame far *f): the empty pixels in the view glowing (c_pv_glow)
; ================================================================================================================
%define G_DX L(1)
%define G_DY L(2)
%define G_NUM L(3)
%define G_Y L(4)
%define G_X L(5)
%define G_GX L(6)
%define G_GY L(7)
%define G_ROW L(8)
%define G_D L(9)                        ; (a grid segment's step, eighths of an index a pixel)

; the glow's table index for the picture point (eax, edx), pixels: eax (pv_glow_index; ecx edx edi used)
gidx:   push edx
        shl eax,8
        sub eax,FR(F_LCX)
        cdq
        shld edx,eax,16
        shl eax,16
        idiv dword FR(F_LF)
        mov G_DX,eax
        pop eax
        shl eax,8
        sub eax,FR(F_LCY)
        cdq
        shld edx,eax,16
        shl eax,16
        idiv dword FR(F_LF)
        mov G_DY,eax
        mov eax,G_DX                    ; num = skf + (dx skr >> 16) - (dy sku >> 16)
        imul dword FR(F_SKR)
        shrd eax,edx,16
        mov ecx,eax
        mov eax,G_DY
        imul dword FR(F_SKU)
        shrd eax,edx,16
        sub ecx,eax
        add ecx,FR(F_SKF)
        mov G_NUM,ecx
        mov eax,G_DX                    ; den = root(2^32 + dx^2 + dy^2)
        imul eax
        mov ecx,eax
        mov edi,edx
        mov eax,G_DY
        imul eax
        add eax,ecx
        adc edx,edi
        add edx,1
        call isqrt64
        mov ecx,eax                     ; cz = num 65536 / den; ((cz + 65536) 256) >> 17
        mov eax,G_NUM
        cdq
        shld edx,eax,16
        shl eax,16
        idiv ecx
        add eax,65536
        sar eax,9
        jns .a
        xor eax,eax
.a:     cmp eax,PV_GLOW_CZ-1
        jle .b
        mov eax,PV_GLOW_CZ-1
.b:     ret

        EXPORT(_rv_pv_glow)
_rv_pv_glow:
        PV_ENTER
        cmp word FR(F_GLOW_ANY),0
        jne .go
        jmp .done
.go:    xor eax,eax                     ; the grid: every 8th pixel each way
        mov G_GY,eax
.gy:    xor eax,eax
        mov G_GX,eax
.gx:    mov eax,G_GX
        shl eax,3
        mov edx,G_GY
        shl edx,3
        call gidx
        mov ecx,G_GY
        imul ecx,ecx,PV_LW/8+1
        add ecx,G_GX
        shl ecx,1
        lea di,[bp+GLOWG]
        add di,cx
        mov [di],ax
        inc dword G_GX
        cmp dword G_GX,PV_LW/8
        jle .gx
        inc dword G_GY
        cmp dword G_GY,PV_LH/8
        jle .gy
        movsx eax,word FR(F_BY0)
        mov G_Y,eax
.row:   mov eax,G_Y
        cmp ax,FR(F_BY1)
        jl .r1
        jmp .done
.r1:    mov ebx,eax                     ; the row's indices: weighed between the grid's two rows
        shr ebx,3
        imul ebx,ebx,(PV_LW/8+1)*2
        lea si,[bp+GLOWG]
        add si,bx
        mov ecx,eax
        and ecx,7
        lea di,[bp+GLOWR]
        xor bx,bx
.rk:    movsx eax,word [si+bx]
        movsx edx,word [si+bx+(PV_LW/8+1)*2]
        sub edx,eax
        imul edx,ecx
        sar edx,3
        add eax,edx
        mov [di+bx],ax
        add bx,2
        cmp bx,(PV_LW/8+1)*2
        jl .rk
        mov ax,G_Y                      ; its pixels, a grid segment at a time: the index r[k] + ((r[k+1] - r[k]) j >> 3)
        shl ax,7                        ; as a running sum of eighths (the same numbers: c_pv_glow's)
        add ax,FR(F_BX0)
        shl ax,1
        add ax,FR(F_O_PIC)
        mov si,ax
        movsx eax,word FR(F_BX0)
        mov G_X,eax
.seg:   mov eax,G_X
        cmp ax,FR(F_BX1)
        jl .sg1
        jmp .rowd
.sg1:   mov ebx,eax
        shr ebx,3
        shl bx,1
        lea di,[bp+GLOWR]
        movsx edx,word [di+bx]          ; r[k]
        movsx ecx,word [di+bx+2]
        sub ecx,edx                     ; d
        mov G_D,ecx
        shl edx,3
        mov ebx,eax
        and ebx,7
        imul ebx,ecx
        add edx,ebx                     ; the index's eighths at x
        mov ecx,eax                     ; the segment: x .. its 8 pixels' end, the part's at most
        or ecx,7
        inc ecx
        movsx ebx,word FR(F_BX1)
        cmp ecx,ebx
        jle .sg2
        mov ecx,ebx
.sg2:   mov G_X,ecx
        sub ecx,eax
        movzx edi,word FR(F_O_GLOW)
.p:     cmp word [es:si],PV_EMPTY
        jne .pn
        mov ebx,edx
        sar ebx,3
        movzx eax,byte [gs:edi+ebx]
        cmp ax,4
        jb .pn
        add ax,4
        shr ax,3
        or ax,PV_GLOW
        mov [es:si],ax
.pn:    add si,2
        add edx,G_D
        dec ecx
        jnz .p
        jmp .seg
.rowd:  inc dword G_Y
        jmp .row
.done:  PV_LEAVE

; ================================================================================================================
; void rv_pv_fogt(PvFrame far *f): the haze's rates by height band for the frame, from its table G (PV_FOG_GS dwords at
; the window's place) and the eye's height: fogm (c_pv_fogt)
; ================================================================================================================
%define FT_K L(1)
%define FT_FR L(2)
%define FT_GA L(3)
%define FT_DG1 L(4)
%define FT_B L(5)
        EXPORT(_rv_pv_fogt)
_rv_pv_fogt:
        PV_ENTER
        movzx edi,word FR(F_O_WIN)      ; G
        mov eax,FR(F_ALT)               ; k = (ha - h0) / hs and fr its rest; under the table 0, 0; past it the last, hs
        sub eax,FR(F_FOG_H0)
        jl .k0
        cdq
        idiv dword FR(F_FOG_HS)
        cmp eax,PV_FOG_GS-1
        jl .kd
        mov eax,PV_FOG_GS-2
        mov edx,FR(F_FOG_HS)
        jmp .kd
.k0:    xor eax,eax
        xor edx,edx
.kd:    mov FT_K,eax
        mov FT_FR,edx
        mov ecx,[gs:edi+eax*4+4]        ; ga = G[k] + (G[k+1] - G[k]) fr / hs
        sub ecx,[gs:edi+eax*4]
        mov FT_DG1,ecx
        mov eax,ecx
        mov ebx,FT_FR
        mov ecx,FR(F_FOG_HS)
        call muldiv
        mov ecx,FT_K
        add eax,[gs:edi+ecx*4]
        mov FT_GA,eax
        xor eax,eax
        mov FT_B,eax
.b:     mov eax,FT_B                    ; a band: dh = h0 + b hs - ha, dg = G[b] - ga
        imul eax,FR(F_FOG_HS)
        add eax,FR(F_FOG_H0)
        sub eax,FR(F_ALT)
        mov ecx,FT_B
        mov ebx,[gs:edi+ecx*4]
        sub ebx,FT_GA
        mov ecx,FR(F_FOG_HS)            ; level with the eye (-hs/2 < dh < hs/2): the rate there
        sar ecx,1
        cmp eax,ecx
        jge .far
        neg ecx
        cmp eax,ecx
        jle .far
        mov eax,FR(F_FOG_HS)
        mov ebx,FT_DG1
.far:   mov ecx,eax                     ; fogm[b] = dg k / (dh 256), 0 for dh 0
        shl ecx,8
        xor eax,eax
        test ecx,ecx
        jz .put
        mov eax,ebx
        mov ebx,FR(F_FOG_K)
        call muldiv
.put:   movzx edx,word FR(F_O_FOGM)
        mov ecx,FT_B
        mov [gs:edx+ecx*4],eax
        inc dword FT_B
        cmp dword FT_B,PV_FOG_HB
        jl .b
        PV_LEAVE

; eax = a b / c as C's (int32_t)((int64_t)a b / c), truncated, wrapped to 32 bits, for any c not 0 (no divide fault:
; the magnitudes divided in two steps); a eax, b ebx, c ecx (ebx ecx edx used)
muldiv: imul ebx
        push esi
        mov esi,edx
        xor esi,ecx                     ; (the quotient's sign)
        test edx,edx
        jns .p1
        neg edx
        neg eax
        sbb edx,0
.p1:    test ecx,ecx
        jns .p2
        neg ecx
.p2:    push eax
        mov eax,edx
        xor edx,edx
        div ecx
        pop eax
        div ecx
        test esi,esi
        jns .p3
        neg eax
.p3:    pop esi
        ret

set_end:

march_start:
; (the march's own copy) the floor of the root of edx:eax (under 2^62) in eax: Newton's steps down from a power of two over the root
; (pview.c pv_isqrt's, bake.asm isqrt's).  ecx edx used
isqrt64b:
        push ebx
        push esi
        push edi
        mov esi,eax
        mov edi,edx
        xor eax,eax
        mov ecx,edi
        or ecx,esi
        jz .done
        bsr ecx,edi
        jz .low
        add cl,32
        jmp .have
.low:   bsr ecx,esi
.have:  add cl,2
        shr cl,1
        mov ebx,0xFFFFFFFF
        cmp cl,32
        jae .step
        mov ebx,1
        shl ebx,cl
.step:  mov eax,esi
        mov edx,edi
        div ebx
        add eax,ebx
        rcr eax,1
        cmp eax,ebx
        jae .root
        mov ebx,eax
        jmp .step
.root:  mov eax,ebx
.done:  pop edi
        pop esi
        pop ebx
        ret

; ================================================================================================================
; the march's parts (es buf, gs scr; es:si the line, fs:bx the step)
; ================================================================================================================
%define M_M L(1)
%define M_X L(2)
%define M_Y L(3)
%define M_H L(4)
%define M_C0 L(5)
%define M_LIGHT L(6)
%define M_FX L(7)
%define M_FY L(8)
%define M_L0 L(9)
%define M_PA L(10)                      ; place's: the point from the eye
%define M_PB L(11)
%define M_PLH L(12)
%define M_ALO L(13)
%define M_AHI L(14)
%define M_RHO L(15)
%define M_I0 L(16)
%define M_I1 L(17)
%define M_SI0 L(18)
%define M_SI1 L(19)
%define F_I0_ L(20)                     ; fill's
%define F_I1_ L(21)
%define F_RA_ L(22)
%define F_RB_ L(23)
%define F_LA_ L(24)
%define F_LB_ L(25)
%define F_FA_ L(26)
%define F_FB_ L(27)
%define F_MAT_ L(28)
%define F_SPAN L(29)
%define F_T0 L(30)
%define F_DT L(31)
%define F_LL L(32)
%define F_DL L(33)
%define F_FF L(34)
%define F_DF L(35)
%define F_ROW L(36)
%define F_COLX L(37)
%define S_H_ L(38)                      ; sample's
%define S_MAT_ L(39)
%define S_LIGHT_ L(40)
%define S_SB L(41)
%define S_LO L(42)
%define S_HI L(43)
%define S_FOG L(44)
%define S_R0 L(45)
%define S_R1 L(46)
%define S_S0 L(47)
%define S_S1 L(48)
%define S_SL L(49)
%define S_SM L(50)
%define FG_DIST L(51)
%define M_PLACESH L(52)                 ; (byte: 28 + vsh)
%define M_N L(53)                       ; the line's number (march)
%define M_STEPOFF L(54)                 ; (word: the step's place, buf)
%define M_INR L(55)
%define M_NS L(56)                      ; sub-samples
%define M_J L(57)
%define M_PX L(58)
%define M_PY L(59)
%define K_I L(60)                       ; skip's
%define K_BX L(61)
%define K_BY L(62)
%define K_HB L(63)
%define K_MX L(64)
%define K_MY L(65)
%define K_J L(66)
%define O_NN L(67)                      ; over's
%define O_B L(68)
%define O_S1 L(69)
%define O_X L(70)
%define O_Y L(71)
%define SL_K L(72)                      ; slab_at's
%define SL_N L(73)
%define SL_MID L(74)
%define SL_A L(75)
%define SL_B L(76)
%define SL_FX L(77)
%define SL_FY L(78)
%define SL_W0 L(79)
%define SL_W1 L(80)
%define SL_W2 L(81)
%define SL_W3 L(82)
%define SL_PTR L(83)
%define SL_WROW L(84)
%define M_A0OFF L(85)
%define M_FARC L(86)
%define M_CELL L(87)
%define S_HAVE L(88)
%define M_SBX0 L(89)                    ; (the slab grids' box on the plane)
%define M_SBX1 L(90)
%define M_SBY0 L(91)
%define M_SBY1 L(92)
%define P_SUM L(93)                     ; fill_px's: the light and haze's sum, its step, the material's 32 x
%define P_DSUM L(94)
%define P_BASE L(95)
%define P_DCOL L(96)
%define P_DROW L(97)

; m for the line (es:si) at the step (fs:bx): eax, carry when the step is past the plane's reach (pv_m)
pm:     mov eax,[es:si+L_TE]
        imul dword [fs:bx+S_S]
        shrd eax,edx,30
        add eax,[fs:bx+S_UC]
        cmp eax,PV_DEN_MIN
        jl .no
        mov ecx,eax
        mov eax,[fs:bx+S_S]
        cdq
        shld edx,eax,16
        shl eax,16
        idiv ecx
        clc
        ret
.no:    stc
        ret

; the line's plane point at m (eax): eax X, edx Y (ecx used)
plane:  mov ecx,eax
        mov eax,[es:si+L_KY]
        imul ecx
        shrd eax,edx,16
        add eax,FR(F_PY)
        push eax
        mov eax,[es:si+L_KX]
        imul ecx
        shrd eax,edx,16
        add eax,FR(F_PX)
        pop edx
        ret

; the ground at (M_X, M_Y) from the window: M_H, M_C0, M_LIGHT; carry when the window has none (pv_ground)
ground: mov cl,FR(F_SH)
        mov eax,M_X
        sar eax,cl
        sub eax,FR(F_OX)
        cmp eax,PV_W-1
        jae .no
        mov edi,eax
        mov eax,M_Y
        sar eax,cl
        sub eax,FR(F_OY)
        cmp eax,PV_W-1
        jae .no
        imul eax,eax,PV_W
        add eax,edi
        lea eax,[eax+eax*2]
        add ax,FR(F_O_WIN)
        mov di,ax
        cmp byte [gs:di+1],PV_ABSENT
        je .no
        sub cl,8
        mov eax,M_X
        sar eax,cl
        and eax,255
        mov M_FX,eax
        mov eax,M_Y
        sar eax,cl
        and eax,255
        mov M_FY,eax
        movsx eax,word [gs:di]          ; h0 = a + ((b - a) fx >> 8)
        movsx ecx,word [gs:di+3]
        sub ecx,eax
        imul ecx,M_FX
        sar ecx,8
        add ecx,eax
        movsx eax,word [gs:di+PV_W*3]   ; h1 = c + ((d - c) fx >> 8)
        movsx edx,word [gs:di+PV_W*3+3]
        sub edx,eax
        imul edx,M_FX
        sar edx,8
        add edx,eax
        sub edx,ecx                     ; h = h0 + ((h1 - h0) fy >> 8)
        imul edx,M_FY
        sar edx,8
        add edx,ecx
        mov M_H,edx
        mov M_CELL,edi                  ; (the colour and light later, if the sample is seen: colour)
        clc
        ret
.no:    stc
        ret

; the colour id and light of the ground found last (M_CELL, M_FX, M_FY): S_MAT_, S_LIGHT_, S_HAVE (ecx edx edi used)
colour: movzx edi,word M_CELL
        lea ecx,[edi+2]                 ; the colour: the nearest cell's
        cmp dword M_FX,128
        jl .c1
        add ecx,3
.c1:    cmp dword M_FY,128
        jl .c2
        add ecx,PV_W*3
.c2:    movzx eax,byte [gs:ecx]
        mov edx,eax
        shr edx,5
        mov S_MAT_,edx
        mov cl,[gs:edi+2]               ; the four of one material (their high bits the same): their light weighed
        mov ch,cl
        xor cl,[gs:edi+5]
        xor ch,[gs:edi+PV_W*3+2]
        or cl,ch
        mov ch,[gs:edi+PV_W*3+2]
        xor ch,[gs:edi+PV_W*3+5]
        or cl,ch
        cmp cl,PV_LIGHTS
        jae .flat
        movzx eax,byte [gs:edi+2]       ; l0 = 256 a + (b - a) fx (a, b over c, d: the four's light)
        and eax,31
        movzx ecx,byte [gs:edi+5]
        and ecx,31
        sub ecx,eax
        imul ecx,M_FX
        shl eax,8
        add eax,ecx
        movzx edx,byte [gs:edi+PV_W*3+2]   ; l1 = 256 c + (d - c) fx
        and edx,31
        movzx ecx,byte [gs:edi+PV_W*3+5]
        and ecx,31
        sub ecx,edx
        imul ecx,M_FX
        shl edx,8
        add edx,ecx
        sub edx,eax                     ; light = (256 l0 + (l1 - l0) fy) >> 8 (pv_ground's sums, the same)
        imul edx,M_FY
        shl eax,8
        add eax,edx
        sar eax,8
        mov S_LIGHT_,eax
        mov dword S_HAVE,1
        ret
.flat:  and eax,31
        shl eax,8
        mov S_LIGHT_,eax
        mov dword S_HAVE,1
        ret

; the point at the step at height eax (BK_HU) from the eye: M_PA, M_PB (pv_place)
place:  mov M_PLH,eax
        imul dword [fs:bx+S_C]
        mov cl,M_PLACESH
        shrd eax,edx,cl
        add eax,[fs:bx+S_A]
        mov M_PA,eax
        mov eax,M_PLH
        imul dword [fs:bx+S_S]
        shrd eax,edx,cl
        add eax,[fs:bx+S_B]
        mov M_PB,eax
        ret

; carry when (M_PA, M_PB) is above the horizon's ray: a ht - b hu > 0 (pv_above)
above:  mov eax,M_PA
        imul dword [es:si+L_HT]
        mov M_ALO,eax
        mov M_AHI,edx
        mov eax,M_PB
        imul dword [es:si+L_HU]
        sub M_ALO,eax
        sbb M_AHI,edx
        cmp dword M_AHI,0
        jg .yes
        jl .no
        cmp dword M_ALO,0
        jne .yes
.no:    clc
        ret
.yes:   stc
        ret

; (M_PA, M_PB)'s pixel on the line, 256ths, held to 16 bits: eax; carry when it is behind the eye (pv_rho)
rho:    movsx ecx,word [es:si+L_UF]
        imul ecx,M_PA
        movsx edx,word [es:si+L_TF]
        imul edx,M_PB
        add ecx,edx
        jle .behind
        movsx eax,word [es:si+L_US]
        imul eax,M_PA
        movsx edx,word [es:si+L_TS]
        imul edx,M_PB
        add eax,edx
        mov edx,eax
        sar edx,31
        mov edi,eax
        xor edi,edx
        sub edi,edx
        shr edi,16
        cmp edi,ecx
        jae .big
        movsx edx,word [es:si+L_RB]
        imul edx
        idiv ecx
        jmp .add
.big:   movsx edx,word [es:si+L_RB]
        xor edx,eax
        mov eax,0x40000000
        test edx,edx
        jns .add
        neg eax
.add:   movsx edx,word [es:si+L_RA]
        add eax,edx
        cmp eax,-32768
        jge .c1
        mov eax,-32768
.c1:    cmp eax,32767
        jle .c2
        mov eax,32767
.c2:    clc
        ret
.behind:
        stc
        ret

; the haze's level for distance FG_DIST and height edx: eax (pv_fog_at; di used)
fog_at: mov eax,edx
        sub eax,FR(F_FOG_H0)
        imul dword FR(F_FOG_RCP)
        shrd eax,edx,16
        cmp eax,0
        jge .b1
        xor eax,eax
.b1:    cmp eax,PV_FOG_HB-1
        jle .b2
        mov eax,PV_FOG_HB-1
.b2:    shl ax,2
        add ax,FR(F_O_FOGM)
        mov di,ax
        mov eax,FG_DIST
        imul dword [gs:di]
        shrd eax,edx,24
        cmp eax,0
        jge .k1
        xor eax,eax
.k1:    cmp eax,PV_FOG_TAUS-1
        jle .k2
        mov eax,PV_FOG_TAUS-1
.k2:    add ax,FR(F_O_FOGLEV)
        mov di,ax
        movzx eax,byte [gs:di]
        ret

; the pixels between the horizon and eax (rho): M_I0, M_I1; carry when none (pv_span)
span:   cmp word FR(F_NADIR),0
        je .z
        movsx ecx,word [es:si+L_HORIZON]
        sar ecx,8
        inc ecx
        mov edx,eax
        sar edx,8
        jmp .clip
.z:     mov ecx,eax
        neg ecx
        sar ecx,8
        neg ecx
        movsx edx,word [es:si+L_HORIZON]
        neg edx
        sar edx,8
        neg edx
        dec edx
.clip:  movzx eax,byte [es:si+L_K0]
        cmp ecx,eax
        jge .a
        mov ecx,eax
.a:     movzx eax,byte [es:si+L_K1]
        cmp edx,eax
        jle .b
        mov edx,eax
.b:     mov M_I0,ecx
        mov M_I1,edx
        cmp ecx,edx
        jg .none
        clc
        ret
.none:  stc
        ret

; t0 for num ecx (i0 256 - ra) and span F_SPAN (not 0): eax (edx used; pv_fill's)
t0_of:  xor eax,eax
        test ecx,ecx
        jz .r                           ; (num 0, or of the other sign: 0)
        mov edx,ecx
        xor edx,F_SPAN
        js .r
        mov eax,65536                   ; (|num| >= |span|: 65536)
        test ecx,ecx
        js .ng
        cmp ecx,F_SPAN
        jge .r
        jmp .dv
.ng:    cmp ecx,F_SPAN
        jle .r
.dv:    mov eax,ecx
        cdq
        shld edx,eax,16
        shl eax,16
        idiv dword F_SPAN
.r:     ret

; dt for ecx (>= 1) pixels past the first, t0 F_T0, span F_SPAN: F_DT, held so t stays within -7 .. 65543 (pv_fill's)
dt_of:  mov eax,1<<24
        cdq
        idiv dword F_SPAN
        mov F_DT,eax
        imul ecx
        add eax,F_T0
        adc edx,0
        add eax,7
        adc edx,0
        jnz .fix
        cmp eax,65536+14
        jbe .ok
.fix:   mov eax,65536
        sub eax,F_T0
        cdq
        idiv ecx
        mov F_DT,eax
.ok:    ret

; the line's pixels F_I0_ .. F_I1_ still empty filled from F_RA_ (light F_LA_, haze F_FA_) to F_RB_ (F_LB_, F_FB_),
; material F_MAT_ (its first light: pv_fill)
fill:   mov eax,F_RB_
        sub eax,F_RA_
        mov F_SPAN,eax
        jnz .sp
        mov dword F_T0,65536            ; (one place: t 1 throughout, b's light and haze)
        mov dword F_DT,0
        jmp .acc
.sp:    mov ecx,F_I0_                   ; t0 at the first pixel
        shl ecx,8
        sub ecx,F_RA_
        call t0_of
        mov F_T0,eax
        mov dword F_DT,0
        mov ecx,F_I1_
        sub ecx,F_I0_
        jle .acc                        ; (one pixel: no slope)
        call dt_of
.acc:   mov eax,F_LB_                   ; l = la 256 + ((lb - la) t0 >> 8) + 32768, the haze the same: the sum's
        sub eax,F_LA_                   ; (fo >> 8) 65536 + (l >> 8)
        imul eax,F_T0
        sar eax,8
        mov ecx,F_LA_
        shl ecx,8
        add eax,ecx
        add eax,32768
        sar eax,8
        mov P_SUM,eax
        mov eax,F_FB_
        sub eax,F_FA_
        imul eax,F_T0
        sar eax,8
        mov ecx,F_FA_
        shl ecx,8
        add eax,ecx
        add eax,32768
        sar eax,8
        shl eax,16
        add P_SUM,eax
        xor eax,eax                     ; its step a pixel: (fb - fa) dt >> 16 in its high half, (lb - la) dt >> 16
        mov ecx,F_I1_
        cmp ecx,F_I0_
        jle .d0
        mov eax,F_FB_
        sub eax,F_FA_
        imul dword F_DT
        shrd eax,edx,16
        shl eax,16
        mov ecx,eax
        mov eax,F_LB_
        sub eax,F_LA_
        imul dword F_DT
        shrd eax,edx,16
        add eax,ecx
.d0:    mov P_DSUM,eax
        mov eax,F_MAT_
        mov P_BASE,eax
        mov ecx,F_I0_
        mov edx,F_I1_
        jmp fill_px

; the line's pixels ecx .. edx, those still empty, from the sum P_SUM (light its bits 8-15, haze its bits 24-31) and
; its step a pixel P_DSUM, material P_BASE (32 x it): pv_fill's loop (bx, si kept)
fill_px:
        PSTART 4
        push bx
        push si
        sub edx,ecx
        inc edx
        mov bx,dx                       ; (the count: under 256)
        movsx eax,byte [es:si+L_DROW]
        mov P_DROW,eax
        imul eax,ecx
        movzx edi,byte [es:si+L_ROW]
        add edi,eax                     ; the along at the first
        mov eax,[es:si+L_DCOL]
        mov P_DCOL,eax
        imul eax,ecx
        add eax,[es:si+L_COL]
        movzx ecx,bx
        mov edx,P_SUM
        movzx ebx,word FR(F_O_PIC)
        test byte [es:si+L_FLAGS],LF_VERT
        mov esi,eax                     ; across, 65536ths
        jz .hz
        shl edi,8                       ; vertical: the row's place, 256 bytes a row
        add edi,ebx
        shl dword P_DROW,8
        cmp word FR(F_WIDE),0
        jne .vw
.vp:    mov ebx,esi
        sar ebx,16
        cmp ebx,PV_LW
        jae .vn
        cmp word [es:edi+ebx*2],PV_EMPTY
        jne .vn
        mov eax,edx
        shr eax,16
        mov al,dh
        add al,P_BASE
        mov [es:edi+ebx*2],ax
.vn:    add esi,P_DCOL
        add edi,P_DROW
        add edx,P_DSUM
        dec ecx
        jnz .vp
        jmp .end
.vw:    mov ebx,esi                     ; (wide: the pixel and the next across, each where still empty)
        sar ebx,16
        cmp ebx,PV_LW
        jae .vwn
        mov eax,edx
        shr eax,16
        mov al,dh
        add al,P_BASE
        cmp word [es:edi+ebx*2],PV_EMPTY
        jne .vw2
        mov [es:edi+ebx*2],ax
.vw2:   inc ebx
        cmp ebx,PV_LW
        jae .vwn
        cmp word [es:edi+ebx*2],PV_EMPTY
        jne .vwn
        mov [es:edi+ebx*2],ax
.vwn:   add esi,P_DCOL
        add edi,P_DROW
        add edx,P_DSUM
        dec ecx
        jnz .vw
        jmp .end
.hz:    shl edi,1                       ; horizontal: the column's place, 2 bytes a column
        add edi,ebx
        shl dword P_DROW,1
        cmp word FR(F_WIDE),0
        jne .hw
.hp:    mov ebx,esi
        sar ebx,16
        cmp ebx,PV_LH
        jae .hn
        shl ebx,8
        cmp word [es:edi+ebx],PV_EMPTY
        jne .hn
        mov eax,edx
        shr eax,16
        mov al,dh
        add al,P_BASE
        mov [es:edi+ebx],ax
.hn:    add esi,P_DCOL
        add edi,P_DROW
        add edx,P_DSUM
        dec ecx
        jnz .hp
        jmp .end
.hw:    mov ebx,esi
        sar ebx,16
        cmp ebx,PV_LH
        jae .hwn
        mov eax,edx
        shr eax,16
        mov al,dh
        add al,P_BASE
        shl ebx,8
        cmp word [es:edi+ebx],PV_EMPTY
        jne .hw2
        mov [es:edi+ebx],ax
.hw2:   cmp ebx,(PV_LH-1)*256
        jae .hwn
        cmp word [es:edi+ebx+256],PV_EMPTY
        jne .hwn
        mov [es:edi+ebx+256],ax
.hwn:   add esi,P_DCOL
        add edi,P_DROW
        add edx,P_DSUM
        dec ecx
        jnz .hw
.end:   pop si
        pop bx
        PEND 4
        ret

; is the line over (pv_over): carry when its horizon's ray has left the top sphere behind (the step at fs:bx)
over:   mov eax,[es:si+L_HU]
        imul eax
        mov ecx,eax
        mov edi,edx
        mov eax,[es:si+L_HT]
        imul eax
        add eax,ecx
        adc edx,edi
        call isqrt64b
        test eax,eax
        jz .yes
        mov O_NN,eax
        mov eax,FR(F_D)                 ; b = D hu / nn
        imul dword [es:si+L_HU]
        idiv dword O_NN
        mov O_B,eax
        imul eax                        ; disc = b^2 - (D^2 - rt^2)
        mov ecx,eax
        mov edi,edx
        mov eax,FR(F_D)
        imul eax
        sub ecx,eax
        sbb edi,edx
        mov eax,FR(F_RT)
        imul eax
        add ecx,eax
        adc edi,edx
        test edi,edi
        js .yes
        jnz .pos
        test ecx,ecx
        jz .yes
.pos:   mov eax,ecx
        mov edx,edi
        call isqrt64b
        sub eax,O_B                     ; s1 = -b + root
        jle .yes
        mov O_S1,eax
        imul dword [es:si+L_HU]         ; x = D + s1 hu / nn, y = s1 ht / nn
        idiv dword O_NN
        add eax,FR(F_D)
        mov O_X,eax
        mov eax,O_S1
        imul dword [es:si+L_HT]
        idiv dword O_NN
        mov O_Y,eax
        mov eax,[fs:bx+S_S]             ; s x - c y > 0
        imul dword O_X
        mov ecx,eax
        mov edi,edx
        mov eax,[fs:bx+S_C]
        imul dword O_Y
        sub ecx,eax
        sbb edi,edx
        test edi,edi
        js .no
        jnz .yes
        test ecx,ecx
        jnz .yes
.no:    clc
        ret
.yes:   stc
        ret

; a sample (pv_sample): S_H_, S_MAT_, S_LIGHT_, and the slab S_SB (S_LO .. S_HI), at the step fs:bx, the line es:si
sample: inc dword FR(F_SAMPLES)
        mov eax,S_H_
        call place
        call above
        jnc sample_hid
; (the march's fast path comes in here: counted, placed, seen above the horizon's ray)
sample_seen:
        movsx ecx,word [es:si+L_UF]     ; its pixel on the line, rho (pv_rho): z = a uf + b tf, behind the eye when <= 0
        imul ecx,M_PA
        movsx edx,word [es:si+L_TF]
        imul edx,M_PB
        add ecx,edx
        jg .front
        jmp sample_hid
.front: movsx eax,word [es:si+L_US]     ; num = a us + b ts
        imul eax,M_PA
        movsx edx,word [es:si+L_TS]
        imul edx,M_PB
        add eax,edx
        cdq
        mov edi,eax
        xor edi,edx
        sub edi,edx
        shr edi,16
        cmp edi,ecx
        jae .big
        movsx edx,word [es:si+L_RB]
        imul edx
        idiv ecx
.add:   movsx edx,word [es:si+L_RA]
        add eax,edx
        movsx edx,ax                    ; (held to 16 bits)
        cmp edx,eax
        jne .clamp
.rho:   mov M_RHO,eax
        cmp dword S_HAVE,0              ; (seen: its colour and light now)
        jne .hv
        call colour
.hv:    mov eax,S_H_                    ; the haze (pv_fog_at): by its height's band, the level at its distance
        sub eax,FR(F_FOG_H0)
        imul dword FR(F_FOG_RCP)
        shrd eax,edx,16
        cmp eax,PV_FOG_HB-1
        jbe .b1
        sar eax,31                      ; (under 0: 0; past the last: the last)
        not eax
        and eax,PV_FOG_HB-1
.b1:    movzx edi,word FR(F_O_FOGM)
        mov eax,[gs:edi+eax*4]
        imul dword [fs:bx+S_DIST]
        shrd eax,edx,24
        cmp eax,PV_FOG_TAUS-1
        jbe .b2
        sar eax,31
        not eax
        and eax,PV_FOG_TAUS-1
.b2:    movzx edi,word FR(F_O_FOGLEV)
        movzx eax,byte [gs:edi+eax]
        shl eax,8
        mov S_FOG,eax
        inc dword FR(F_SEEN)
        mov eax,M_RHO                   ; the pixels between the horizon and rho (pv_span): ecx .. edx
        cmp word FR(F_NADIR),0
        je .z
        movsx ecx,word [es:si+L_HORIZON]
        sar ecx,8
        inc ecx
        mov edx,eax
        sar edx,8
        jmp .clip
.z:     mov ecx,eax
        neg ecx
        sar ecx,8
        neg ecx
        movsx edx,word [es:si+L_HORIZON]
        neg edx
        sar edx,8
        not edx
.clip:  movzx edi,byte [es:si+L_K0]
        cmp ecx,edi
        jge .k0
        mov ecx,edi
.k0:    movzx edi,byte [es:si+L_K1]
        cmp edx,edi
        jle .k1
        mov edx,edi
.k1:    cmp ecx,edx
        jg .after
        mov M_SI0,ecx
        mov M_SI1,edx
        test byte [es:si+L_FLAGS],LF_POK
        jz .flat
        mov al,[es:si+L_MAT]
        cmp al,S_MAT_
        jne .gentle
        movsx edi,word [es:si+L_PRHO]   ; the same material: weighed from the last sample (pv_fill from prho)
        mov eax,M_RHO
        sub eax,edi
        mov F_SPAN,eax
        jnz .isp
        mov dword F_T0,65536            ; (one place: this one's throughout)
        mov dword F_DT,0
        jmp .iacc
.isp:   shl ecx,8                       ; t0: num = i0 256 - prho
        sub ecx,edi
        call t0_of
        mov F_T0,eax
        mov dword F_DT,0
        mov ecx,M_SI1
        sub ecx,M_SI0
        jle .iacc
        call dt_of
.iacc:  movsx ecx,word [es:si+L_PLIGHT] ; the sum: (fo >> 8) 65536 + (l >> 8)
        mov eax,S_LIGHT_
        sub eax,ecx
        mov F_DL,eax                    ; (lb - la)
        imul eax,F_T0
        sar eax,8
        shl ecx,8
        add eax,ecx
        add eax,32768
        sar eax,8
        mov P_SUM,eax
        movsx ecx,word [es:si+L_PFOG]
        mov eax,S_FOG
        sub eax,ecx
        mov F_DF,eax                    ; (fb - fa)
        imul eax,F_T0
        sar eax,8
        shl ecx,8
        add eax,ecx
        add eax,32768
        sar eax,8
        shl eax,16
        add P_SUM,eax
        xor eax,eax                     ; its step
        mov ecx,M_SI1
        cmp ecx,M_SI0
        jle .id0
        mov eax,F_DF
        imul dword F_DT
        shrd eax,edx,16
        shl eax,16
        mov ecx,eax
        mov eax,F_DL
        imul dword F_DT
        shrd eax,edx,16
        add eax,ecx
.id0:   mov P_DSUM,eax
        mov eax,S_MAT_
        shl eax,5
        mov P_BASE,eax
        mov ecx,M_SI0
        mov edx,M_SI1
        call fill_px
        jmp .after
.big:   movsx edx,word [es:si+L_RB]     ; (|num| >> 16 >= z: 2^30 of num's sign times rb's)
        xor edx,eax
        mov eax,0x40000000
        test edx,edx
        jns .add
        neg eax
        jmp .add
.clamp: test eax,eax
        mov eax,32767
        jns .rho
        mov eax,-32768
        jmp .rho
.gentle:
        movsx ecx,word [es:si+L_PDS]    ; (h - ph) 4 < pds / 2 and (ph - h) 4 < pds / 2
        sar ecx,1
        movsx edx,word [es:si+L_PH]
        mov eax,S_H_
        sub eax,edx
        shl eax,2
        cmp eax,ecx
        jge .flat
        neg eax
        cmp eax,ecx
        jge .flat
        movsx eax,word [es:si+L_PRHO]   ; mid = (prho + rho) / 2: the pixels nearer the last sample its
        add eax,M_RHO
        cdq
        sub eax,edx
        sar eax,1
        call span
        jc .flat
        movsx eax,word [es:si+L_PFOG]   ; (its light and haze throughout: the sum (pfog + 128) 65536 + plight + 128)
        add eax,128
        shl eax,16
        movsx edx,word [es:si+L_PLIGHT]
        add eax,edx
        add eax,128
        mov P_SUM,eax
        mov dword P_DSUM,0
        movzx eax,byte [es:si+L_MAT]
        shl eax,5
        mov P_BASE,eax
        mov ecx,M_I0
        mov edx,M_I1
        call fill_px
.flat:  mov eax,S_FOG                   ; this sample's own throughout
        add eax,128
        shl eax,16
        add eax,S_LIGHT_
        add eax,128
        mov P_SUM,eax
        mov dword P_DSUM,0
        mov eax,S_MAT_
        shl eax,5
        mov P_BASE,eax
        mov ecx,M_SI0
        mov edx,M_SI1
        call fill_px
.after: mov eax,S_FOG                   ; the line's state: this sample's
        mov [es:si+L_PFOG],ax
        mov eax,M_RHO
        mov [es:si+L_HORIZON],ax
        mov [es:si+L_PRHO],ax
        mov ecx,M_PA
        mov [es:si+L_HU],ecx
        mov ecx,M_PB
        mov [es:si+L_HT],ecx
        mov cl,[es:si+L_FLAGS]
        and cl,~LF_HID
        or cl,LF_POK
        cmp word FR(F_NADIR),0
        je .dz
        movzx edx,byte [es:si+L_K1]
        shl edx,8
        cmp eax,edx
        jl .dn
        or cl,LF_DONE
        jmp .dn
.dz:    movzx edx,byte [es:si+L_K0]
        shl edx,8
        cmp eax,edx
        jg .dn
        or cl,LF_DONE
.dn:    mov [es:si+L_FLAGS],cl
        mov eax,S_LIGHT_
        mov [es:si+L_PLIGHT],ax
        mov eax,S_H_
        mov [es:si+L_PH],ax
        mov al,S_MAT_
        mov [es:si+L_MAT],al
        mov ax,[fs:bx+S_DS]
        mov [es:si+L_PDS],ax
        jmp sample_slab
sample_hid:
        mov al,[es:si+L_FLAGS]
        or al,LF_HID
        and al,~LF_POK
        mov [es:si+L_FLAGS],al
sample_slab:
        cmp dword S_SB,0
        je .noslab
        mov eax,S_LO                    ; the slab: its underside's and top's pixels
        call place
        call rho
        jc .noslab
        mov S_R0,eax
        mov eax,S_HI
        call place
        call rho
        jc .noslab
        mov S_R1,eax
        mov ecx,S_R0
        mov edx,eax
        cmp ecx,edx
        jle .o1
        xchg ecx,edx
.o1:    test byte [es:si+L_FLAGS],LF_PSLAB
        jz .o3
        movsx eax,word [es:si+L_PLO]
        cmp eax,ecx
        jge .o2
        mov ecx,eax
.o2:    movsx eax,word [es:si+L_PHI]
        cmp eax,edx
        jle .o3
        mov edx,eax
.o3:    mov S_S0,ecx
        mov S_S1,edx
        mov eax,FR(F_ALT)               ; the underside seen from under it, else the top
        mov dword S_SM,3*32
        cmp eax,S_LO
        jl .m1
        mov dword S_SM,1*32
.m1:    cmp eax,S_HI
        jle .ul
        cmp dword S_HAVE,0              ; (over it: the ground's light)
        jne .hv2
        call colour
.hv2:   mov eax,S_LIGHT_
        sar eax,8
        jmp .sl
.ul:    xor eax,eax                     ; the underside's glow by its height over the ground
        mov ecx,S_LO
        cmp ecx,S_H_
        jle .g0
        sub ecx,S_H_
        shl ecx,4
        mov eax,ecx
        cdq
        idiv dword FR(F_UNDER_DIV)
        cmp eax,63
        jle .g0
        mov eax,63
.g0:    add ax,FR(F_O_UNDER)
        mov di,ax
        movzx eax,byte [gs:di]
.sl:    mov S_SL,eax
        mov eax,[fs:bx+S_DIST]
        mov FG_DIST,eax
        mov edx,S_HI
        call fog_at
        shl eax,8
        mov F_FA_,eax
        mov F_FB_,eax
        mov eax,S_S0                    ; i0 = ceil(s0), i1 = floor(s1), within k0 .. k1
        neg eax
        sar eax,8
        neg eax
        movzx ecx,byte [es:si+L_K0]
        cmp eax,ecx
        jge .p1
        mov eax,ecx
.p1:    mov F_I0_,eax
        mov eax,S_S1
        sar eax,8
        movzx ecx,byte [es:si+L_K1]
        cmp eax,ecx
        jle .p2
        mov eax,ecx
.p2:    mov F_I1_,eax
        cmp eax,F_I0_
        jl .p3
        xor eax,eax
        mov F_RA_,eax
        mov F_RB_,eax
        mov eax,S_SL
        shl eax,8
        mov F_LA_,eax
        mov F_LB_,eax
        mov eax,S_SM
        mov F_MAT_,eax
        call fill
.p3:    mov ecx,S_R0
        mov edx,S_R1
        cmp ecx,edx
        jle .p4
        xchg ecx,edx
.p4:    mov [es:si+L_PLO],cx
        mov [es:si+L_PHI],dx
        or byte [es:si+L_FLAGS],LF_PSLAB
        movsx eax,word [es:si+L_HORIZON]   ; some of it past the filled part: it may hide what follows
        cmp word FR(F_NADIR),0
        je .hz
        cmp S_S1,eax
        jle .ret
        and byte [es:si+L_FLAGS],~LF_HID
        ret
.hz:    cmp S_S0,eax
        jge .ret
        and byte [es:si+L_FLAGS],~LF_HID
.ret:   ret
.noslab:
        and byte [es:si+L_FLAGS],~LF_PSLAB
        ret

; (M_X, M_Y) within one of the slab grids: carry (pv_in_slabs)
in_slabs:
        xor di,di
.k:     cmp di,FR(F_NSLAB)
        jge .no
        push di
        shl di,4
        mov cl,FR(F_SLAB_SHIFT)
        mov eax,M_X
        sar eax,cl
        sub eax,FR(F_SLAB+di)
        movzx edx,word FR(F_SLAB+8+di)
        cmp eax,edx
        jae .n
        mov eax,M_Y
        sar eax,cl
        sub eax,FR(F_SLAB+4+di)
        movzx edx,word FR(F_SLAB+10+di)
        cmp eax,edx
        jae .n
        pop di
        stc
        ret
.n:     pop di
        inc di
        jmp .k
.no:    clc
        ret

; one corner of a slab cell (di: its place, %1 its offset): to the middle's sum and count when it has a slab
%macro SLAB_MID 1
        movsx eax,word [es:di+%1]
        movsx edx,word [es:di+%1+2]
        cmp eax,edx
        jge %%none
        add eax,edx
        add SL_MID,eax
        inc dword SL_N
%%none:
%endmacro
; and its share: SL_A += (w lo') >> 8, SL_B += (w hi') >> 8, lo' and hi' its own 256ths or the middle
%macro SLAB_ADD 2
        movsx ecx,word [es:di+%1]
        movsx edx,word [es:di+%1+2]
        cmp ecx,edx
        jl %%has
        mov ecx,SL_MID
        mov edx,ecx
        jmp %%go
%%has:  shl ecx,8
        shl edx,8
%%go:   push edx
        mov eax,%2
        imul ecx
        shrd eax,edx,8
        add SL_A,eax
        pop ecx
        mov eax,%2
        imul ecx
        shrd eax,edx,8
        add SL_B,eax
%endmacro

; the slab over (M_X, M_Y): S_LO, S_HI, carry when there is one (pv_slab_at; di used)
slab_at:
        mov dword SL_K,0
.k:     mov eax,SL_K
        cmp ax,FR(F_NSLAB)
        jl .k1
        jmp .no
.k1:    shl ax,4
        mov di,ax                       ; di: the grid's record in the frame
        mov cl,FR(F_SLAB_SHIFT)
        mov eax,M_X
        sar eax,cl
        sub eax,FR(F_SLAB+di)
        movzx edx,word FR(F_SLAB+8+di)
        dec edx
        cmp eax,edx
        jae .next
        mov SL_A,eax                    ; sx
        mov eax,M_Y
        sar eax,cl
        sub eax,FR(F_SLAB+4+di)
        movzx edx,word FR(F_SLAB+10+di)
        dec edx
        cmp eax,edx
        jae .next
        movzx edx,word FR(F_SLAB+8+di)  ; the cell: at + sy w + sx, 4 bytes each
        mov SL_WROW,edx
        imul eax,edx
        add eax,SL_A
        add eax,FR(F_SLAB+12+di)
        shl eax,2
        add ax,FR(F_O_SLAB)
        mov SL_PTR,eax
        shl dword SL_WROW,2
        xor eax,eax
        mov SL_MID,eax
        mov SL_N,eax
        mov di,SL_PTR
        SLAB_MID 0
        SLAB_MID 4
        add di,SL_WROW
        SLAB_MID 0
        SLAB_MID 4
        cmp dword SL_N,0
        je .no
        mov eax,SL_MID                  ; the middle, 256ths
        shl eax,7
        cdq
        idiv dword SL_N
        mov SL_MID,eax
        mov cl,FR(F_SLAB_SHIFT)
        sub cl,8
        mov eax,M_X
        sar eax,cl
        and eax,255
        mov SL_FX,eax
        mov eax,M_Y
        sar eax,cl
        and eax,255
        mov SL_FY,eax
        mov ecx,256                     ; the weights
        sub ecx,SL_FX
        mov edx,256
        sub edx,SL_FY
        mov eax,ecx
        imul eax,edx
        mov SL_W0,eax
        mov eax,SL_FX
        imul eax,edx
        mov SL_W1,eax
        mov eax,ecx
        imul eax,SL_FY
        mov SL_W2,eax
        mov eax,SL_FX
        imul eax,SL_FY
        mov SL_W3,eax
        xor eax,eax
        mov SL_A,eax
        mov SL_B,eax
        mov di,SL_PTR
        SLAB_ADD 0,SL_W0
        SLAB_ADD 4,SL_W1
        add di,SL_WROW
        SLAB_ADD 0,SL_W2
        SLAB_ADD 4,SL_W3
        mov eax,SL_B                    ; (65536ths of a height unit: half a unit at least)
        sub eax,SL_A
        cmp eax,32768
        jl .no
        mov eax,SL_A
        sar eax,16
        mov S_LO,eax
        mov eax,SL_B
        sar eax,16
        mov S_HI,eax
        stc
        ret
.next:  inc dword SL_K
        jmp .k
.no:    clc
        ret

; hidden ground skipped (pv_skip): the sample at step K_I (fs:bx) in block (K_BX, K_BY), its highest K_HB: eax, the
; step after the samples that cannot be seen, or K_I (di used)
skip:   mov eax,K_HB
        call place
        call above
        jnc .try
        mov eax,K_I
        ret
.try:   mov dword K_MX,0x7FFFFFFF
        mov dword K_MY,0x7FFFFFFF
        cmp dword [es:si+L_KX],0        ; where the line leaves the block (m)
        je .my
        mov eax,K_BX
        shl eax,2
        add eax,FR(F_OX)
        mov cl,FR(F_SH)
        cmp dword [es:si+L_KX],0
        jl .xn
        add eax,4
        shl eax,cl
        sub eax,FR(F_PX)
        jmp .xe
.xn:    shl eax,cl
        sub eax,FR(F_PX)
        dec eax
.xe:    mov ecx,eax
        cdq
        xor eax,edx
        sub eax,edx
        shr eax,15
        mov edi,[es:si+L_KX]
        mov edx,edi
        sar edx,31
        xor edi,edx
        sub edi,edx
        cmp eax,edi
        jae .my
        mov eax,ecx
        cdq
        shld edx,eax,16
        shl eax,16
        idiv dword [es:si+L_KX]
        mov K_MX,eax
.my:    cmp dword [es:si+L_KY],0
        je .mm
        mov eax,K_BY
        shl eax,2
        add eax,FR(F_OY)
        mov cl,FR(F_SH)
        cmp dword [es:si+L_KY],0
        jl .yn
        add eax,4
        shl eax,cl
        sub eax,FR(F_PY)
        jmp .ye
.yn:    shl eax,cl
        sub eax,FR(F_PY)
        dec eax
.ye:    mov ecx,eax
        cdq
        xor eax,edx
        sub eax,edx
        shr eax,15
        mov edi,[es:si+L_KY]
        mov edx,edi
        sar edx,31
        xor edi,edx
        sub edi,edx
        cmp eax,edi
        jae .mm
        mov eax,ecx
        cdq
        shld edx,eax,16
        shl eax,16
        idiv dword [es:si+L_KY]
        mov K_MY,eax
.mm:    mov eax,K_MY
        cmp eax,K_MX
        jge .scan0
        mov K_MX,eax
.scan0: push bx
        mov eax,K_I
        inc eax
        mov K_J,eax
.scan:  mov eax,K_J                     ; the steps while they are of this level and in the block
        cmp ax,FR(F_NSTEPS)
        jge .sd
        imul ax,ax,STEP_BYTES
        add ax,FR(F_O_ST)
        mov bx,ax
        mov al,[fs:bx+S_LEV]
        cmp al,FR(F_LEV)
        ja .sd
        call pm
        jc .sd
        cmp eax,K_MX
        jge .sd
        inc dword K_J
        jmp .scan
.sd:    dec dword K_J
        mov eax,K_J
        cmp eax,K_I
        jle .no
        imul ax,ax,STEP_BYTES           ; the block's highest hidden at the last of them too
        add ax,FR(F_O_ST)
        mov bx,ax
        mov eax,K_HB
        call place
        call above
        jc .no
        cmp dword [es:si+L_HT],0        ; the ray's nearest approach to the centre between them, the highest over it
        jle .ok
        mov eax,[fs:bx+S_C]             ; c_j hu + s_j ht >= 0
        imul dword [es:si+L_HU]
        mov ecx,eax
        mov edi,edx
        mov eax,[fs:bx+S_S]
        imul dword [es:si+L_HT]
        add eax,ecx
        adc edx,edi
        js .ok
        pop bx
        push bx
        mov eax,[fs:bx+S_C]             ; c_i hu + s_i ht <= 0
        imul dword [es:si+L_HU]
        mov ecx,eax
        mov edi,edx
        mov eax,[fs:bx+S_S]
        imul dword [es:si+L_HT]
        add eax,ecx
        adc edx,edi
        test edx,edx
        js .betw
        jnz .ok
        test eax,eax
        jnz .ok
.betw:  mov eax,[es:si+L_HU]
        imul eax
        mov ecx,eax
        mov edi,edx
        mov eax,[es:si+L_HT]
        imul eax
        add eax,ecx
        adc edx,edi
        call isqrt64b
        test eax,eax
        jz .ok
        mov ecx,eax
        mov eax,FR(F_D)
        imul dword [es:si+L_HT]
        idiv ecx
        mov ecx,K_HB
        shl ecx,2
        add ecx,FR(F_R)
        cmp eax,ecx
        jle .no
.ok:    mov eax,K_J
        inc eax
        sub eax,K_I
        add FR(F_SKIPS),eax
        pop bx
        mov eax,K_J
        inc eax
        ret
.no:    pop bx
        mov eax,K_I
        ret

; the sample's inputs from the ground found (M_H, M_C0, M_LIGHT)
%macro TO_SAMPLE 0
        mov eax,M_H
        mov S_H_,eax
        mov dword S_HAVE,0
%endmacro

; ================================================================================================================
; void rv_pv_march(PvFrame far *f): every line through the frame's level (c_pv_march)
; ================================================================================================================
        EXPORT(_rv_pv_march)
_rv_pv_march:
        PV_ENTER
        mov al,FR(F_VSH)
        add al,28
        mov M_PLACESH,al
        xor eax,eax                     ; the slab grids' box on the plane (none: empty): outside it, the fast path
        mov M_SBX0,eax
        mov M_SBX1,eax
        mov M_SBY0,eax
        mov M_SBY1,eax
        mov dx,FR(F_NSLAB)
        xor di,di
        mov cl,FR(F_SLAB_SHIFT)
.sb:    cmp di,dx
        jge .sbd
        push di
        shl di,4
        mov eax,FR(F_SLAB+di)           ; x0 << shift .. (x0 + w) << shift
        shl eax,cl
        movzx ebx,word FR(F_SLAB+8+di)
        add ebx,FR(F_SLAB+di)
        shl ebx,cl
        pop di
        test di,di
        jz .sb1
        cmp eax,M_SBX0
        jge .sb2
.sb1:   mov M_SBX0,eax
.sb2:   test di,di
        jz .sb3
        cmp ebx,M_SBX1
        jle .sb4
.sb3:   mov M_SBX1,ebx
.sb4:   push di
        shl di,4
        mov eax,FR(F_SLAB+4+di)
        shl eax,cl
        movzx ebx,word FR(F_SLAB+10+di)
        add ebx,FR(F_SLAB+4+di)
        shl ebx,cl
        pop di
        test di,di
        jz .sb5
        cmp eax,M_SBY0
        jge .sb6
.sb5:   mov M_SBY0,eax
.sb6:   test di,di
        jz .sb7
        cmp ebx,M_SBY1
        jle .sb8
.sb7:   mov M_SBY1,ebx
.sb8:   inc di
        jmp .sb
.sbd:   push es
        pop fs
        mov dword M_N,0
.line:  mov eax,M_N
        cmp ax,FR(F_NLINES)
        jl .ln1
        jmp .done
.ln1:   mov bx,LINE_BYTES
        mul bx
        add ax,FR(F_O_LN)
        mov si,ax
.step:  test byte [es:si+L_FLAGS],LF_DONE
        jz .s1
        jmp .nextl
.s1:    PSTART 0
        mov ax,[es:si+L_STEP]
        cmp ax,FR(F_NSTEPS)
        jl .s2
        jmp .nextl
.s2:    imul ax,ax,STEP_BYTES
        add ax,FR(F_O_ST)
        mov bx,ax
        push es
        pop fs
        mov al,[fs:bx+S_LEV]
        cmp al,FR(F_LEV)
        jbe .s3
        jmp .nextl
.s3:    call pm
        jnc .s4
        jmp .nextl
.s4:    call plane
        mov M_X,eax
        mov M_Y,edx
        mov cl,FR(F_SH)                 ; in the window, on ground
        sar eax,cl
        sub eax,FR(F_OX)
        cmp eax,PV_W-1
        jb .s5
        jmp .nextl
.s5:    mov K_BX,eax
        mov eax,edx
        sar eax,cl
        sub eax,FR(F_OY)
        cmp eax,PV_W-1
        jb .s6
        jmp .nextl
.s6:    mov K_BY,eax
        imul eax,eax,PV_W
        add eax,K_BX
        lea eax,[eax+eax*2]
        add ax,FR(F_O_WIN)
        mov di,ax
        mov M_CELL,eax
        cmp byte [gs:di+1],PV_ABSENT
        jne .s7
        jmp .nextl
.s7:    PEND 0
        test byte [es:si+L_FLAGS],LF_HID
        jz .noskip
        mov eax,K_BY                    ; hidden: try skipping by the block's highest
        shr eax,2
        mov K_BY,eax
        imul eax,eax,PV_BW
        mov ecx,K_BX
        shr ecx,2
        mov K_BX,ecx
        add eax,ecx
        shl eax,1
        add ax,FR(F_O_BWIN)
        mov di,ax
        movsx eax,word [gs:di]
        mov K_HB,eax
        movsx eax,word [es:si+L_STEP]
        mov K_I,eax
        PSTART 1
        call skip
        PEND 1
        cmp eax,K_I
        je .noskip
        mov [es:si+L_STEP],ax
        and byte [es:si+L_FLAGS],~(LF_POK|LF_PRECT)
        jmp .step
.noskip:
        test byte [es:si+L_FLAGS],LF_PRECT   ; outside every slab grid's box (and the last step too): the fast path,
        jnz .slow                       ; the ground's height, the point and the ray test here
        mov eax,M_X
        cmp eax,M_SBX0
        jl .fast
        cmp eax,M_SBX1
        jge .fast
        mov eax,M_Y
        cmp eax,M_SBY0
        jl .fast
        cmp eax,M_SBY1
        jl .slow
.fast:  PSTART 2
        mov di,M_CELL
        mov cl,FR(F_SH)
        sub cl,8
        mov eax,M_X
        sar eax,cl
        and eax,255
        mov M_FX,eax
        mov edx,M_Y
        sar edx,cl
        and edx,255
        mov M_FY,edx
        movsx ecx,word [gs:di]          ; h0 = a + ((b - a) fx >> 8)
        movsx edx,word [gs:di+3]
        sub edx,ecx
        imul edx,eax
        sar edx,8
        add ecx,edx
        movsx edx,word [gs:di+PV_W*3]   ; h1 = c + ((d - c) fx >> 8)
        movsx eax,word [gs:di+PV_W*3+3]
        sub eax,edx
        imul eax,M_FX
        sar eax,8
        add eax,edx
        sub eax,ecx                     ; h = h0 + ((h1 - h0) fy >> 8)
        imul eax,M_FY
        sar eax,8
        add eax,ecx
        mov S_H_,eax
        mov dword S_HAVE,0
        mov dword S_SB,0
        inc dword FR(F_SAMPLES)
        mov edi,eax                     ; the point: a + (h c >> 28 + vsh), b + (h s >> ...)
        imul dword [es:bx+S_C]
        mov cl,M_PLACESH
        shrd eax,edx,cl
        add eax,[es:bx+S_A]
        mov M_PA,eax
        mov eax,edi
        imul dword [es:bx+S_S]
        shrd eax,edx,cl
        add eax,[es:bx+S_B]
        mov M_PB,eax
        mov eax,M_PA                    ; above the horizon's ray: a ht - b hu > 0
        imul dword [es:si+L_HT]
        mov edi,eax
        mov ecx,edx
        mov eax,M_PB
        imul dword [es:si+L_HU]
        sub edi,eax
        sbb ecx,edx
        js .fhid
        jnz .fseen
        test edi,edi
        jz .fhid
.fseen: PEND 2
        PSTART 3
        call sample_seen
        PEND 3
        jmp .fadv
.fhid:  PEND 2
        mov al,[es:si+L_FLAGS]
        or al,LF_HID
        and al,~(LF_POK|LF_PSLAB)
        mov [es:si+L_FLAGS],al
.fadv:  inc word [es:si+L_STEP]
        test word [es:si+L_STEP],15
        jnz .fx1
        test byte [es:si+L_FLAGS],LF_DONE
        jnz .fx1
        PSTART 5
        call over
        PEND 5
        jnc .fx1
        or byte [es:si+L_FLAGS],LF_DONE
.fx1:   jmp .step
.slow:  PSTART 6
        mov dword M_INR,0               ; crossing a slab's grid: samples between the last step and this one
        call in_slabs
        jnc .ni
        mov dword M_INR,1
.ni:    cmp dword M_INR,0
        jne .sub
        test byte [es:si+L_FLAGS],LF_PRECT
        jnz .sub
        jmp .nosub
.sub:   cmp word [es:si+L_STEP],0
        jne .sub1
        jmp .nosub
.sub1:  mov ax,bx
        sub ax,STEP_BYTES
        mov M_A0OFF,eax
        push bx
        mov bx,ax
        call pm
        pop bx
        jnc .sub2
        jmp .nosub
.sub2:  call plane                      ; ns: half slab cells between the two plane points, PV_SUBS at most
        mov ecx,M_X
        sub ecx,eax
        mov eax,ecx
        sar ecx,31
        xor eax,ecx
        sub eax,ecx
        mov ecx,M_Y
        sub ecx,edx
        mov edx,ecx
        sar ecx,31
        xor edx,ecx
        sub edx,ecx
        add eax,edx
        mov cl,FR(F_SLAB_SHIFT)
        dec cl
        add cl,FR(F_WIDE)               ; (wide: a slab cell apart)
        sar eax,cl
        mov ecx,PV_SUBS                 ; (wide: PV_SUBS_WIDE at most)
        cmp word FR(F_WIDE),0
        je .ns0
        mov ecx,PV_SUBS_WIDE
.ns0:   cmp eax,ecx
        jle .ns1
        mov eax,ecx
.ns1:   mov M_NS,eax
        mov eax,M_X
        mov M_PX,eax
        mov eax,M_Y
        mov M_PY,eax
        mov dword M_J,1
.sj:    mov eax,M_J
        cmp eax,M_NS
        jl .sj1
        jmp .sdone
.sj1:   test byte [es:si+L_FLAGS],LF_DONE
        jz .sj2
        jmp .sdone
.sj2:   mov di,M_A0OFF                  ; the step between: each part weighed j / ns of the way
%assign k 0
%rep 6
        mov eax,[es:bx+k]
        sub eax,[es:di+k]
        imul dword M_J
        idiv dword M_NS
        add eax,[es:di+k]
        mov [bp+TSTEP+k],eax
%assign k k+4
%endrep
        movsx eax,word [es:di+S_DS]
        cdq
        idiv dword M_NS
        mov [bp+TSTEP+S_DS],ax
        mov al,[es:bx+S_LEV]
        mov [bp+TSTEP+S_LEV],al
        push bx
        mov ax,ss
        mov fs,ax
        lea bx,[bp+TSTEP]
        call pm
        jc .sbrk
        call plane
        mov M_X,eax
        mov M_Y,edx
        call ground
        jc .sbrk
        mov dword S_SB,0
        call slab_at
        jnc .sb0
        mov dword S_SB,1
.sb0:   inc dword FR(F_SUBS)
        TO_SAMPLE
        call sample
        pop bx
        push es
        pop fs
        inc dword M_J
        jmp .sj
.sbrk:  pop bx
        push es
        pop fs
.sdone: mov eax,M_PX
        mov M_X,eax
        mov eax,M_PY
        mov M_Y,eax
        test byte [es:si+L_FLAGS],LF_DONE
        jz .nosub
        jmp .nextl
.nosub: mov al,[es:si+L_FLAGS]
        and al,~LF_PRECT
        cmp dword M_INR,0
        je .p0
        or al,LF_PRECT
.p0:    mov [es:si+L_FLAGS],al
        call ground
        mov dword S_SB,0
        mov dword S_LO,0
        mov dword S_HI,0
        cmp dword M_INR,0
        je .nsl
        call slab_at
        jnc .nsl
        mov dword S_SB,1
.nsl:   TO_SAMPLE
        call sample
        inc word [es:si+L_STEP]
        test word [es:si+L_STEP],15
        jnz .nx
        test byte [es:si+L_FLAGS],LF_DONE
        jnz .nx
        call over
        jnc .nx
        or byte [es:si+L_FLAGS],LF_DONE
.nx:    PEND 6
        jmp .step
.nextl: inc dword M_N
        jmp .line
.done:  lfs bx,[bp+6]
        mov eax,FR(F_SAMPLES)
        mov [fs:bx+F_SAMPLES],eax
        mov eax,FR(F_SEEN)
        mov [fs:bx+F_SEEN],eax
        mov eax,FR(F_SKIPS)
        mov [fs:bx+F_SKIPS],eax
        mov eax,FR(F_SUBS)
        mov [fs:bx+F_SUBS],eax
        PV_LEAVE

; ================================================================================================================
; void rv_pv_far(PvFrame far *f): the lines past every level, over the plain sphere (c_pv_far)
; ================================================================================================================
        EXPORT(_rv_pv_far)
_rv_pv_far:
        PV_ENTER
        mov al,FR(F_VSH)
        add al,28
        mov M_PLACESH,al
        push es
        pop fs
        mov dword M_N,0
.line:  mov eax,M_N
        cmp ax,FR(F_NLINES)
        jge .done
        mov bx,LINE_BYTES
        mul bx
        add ax,FR(F_O_LN)
        mov si,ax
.step:  test byte [es:si+L_FLAGS],LF_DONE
        jnz .nextl
        mov ax,[es:si+L_STEP]
        cmp ax,FR(F_NSTEPS)
        jge .nextl
        imul ax,ax,STEP_BYTES
        add ax,FR(F_O_ST)
        mov bx,ax
        mov dword S_H_,0
        movsx eax,word FR(F_FAR_C)
        mov ecx,eax
        sar eax,5
        mov S_MAT_,eax
        and ecx,31
        shl ecx,8
        mov S_LIGHT_,ecx
        mov dword S_HAVE,1
        mov dword S_SB,0
        call sample
        inc word [es:si+L_STEP]
        test word [es:si+L_STEP],15
        jnz .step
        test byte [es:si+L_FLAGS],LF_DONE
        jnz .step
        call over
        jnc .step
        or byte [es:si+L_FLAGS],LF_DONE
        jmp .step
.nextl: inc dword M_N
        jmp .line
.done:  lfs bx,[bp+6]
        mov eax,FR(F_SAMPLES)
        mov [fs:bx+F_SAMPLES],eax
        mov eax,FR(F_SEEN)
        mov [fs:bx+F_SEEN],eax
        PV_LEAVE

march_end:

; ================================================================================================================
; void rv_pv_blit(const PvBlit far *b): the picture's pixels as 5 x 5 blocks over a band (c_pv_blit).  Its own range
; of the blob, nothing outside called: pview.c runs it from a copy in the scratch buffer's tail.  The picture's rows,
; the dither table and the work area are in one segment (gs, and ds for the copies); the band is es's.
; ================================================================================================================
%define BL_Y L(1)
%define BL_LY L(2)
%define BL_NR L(3)
%define BL_CY L(4)
%define BL_XA L(5)
%define BL_XB L(6)
%define BL_PA L(7)                      ; (words: offsets in gs)
%define BL_PB L(8)
%define BL_RUNS L(9)
%define BL_PIC L(10)
%define BL_DITH L(11)
%define BL_BAND L(12)
%define BL_EXT L(13)                    ; (far)
%define BL_EY0 L(14)
%define BL_EROWS L(15)
%define BL_LY0 L(16)
%define BL_Y1 L(17)
%define BL_Y0 L(18)
%define BL_MASKS L(19)
%define BL_IN L(20)
%define BL_X L(21)
%define BL_SRC L(22)
        EXPORT(_rv_pv_blit)
blit_start:
_rv_pv_blit:
        push bp
        mov bp,sp
        sub sp,FRAME_ALL
        push si
        push di
        push ds
        push fs
        push gs
        cld
        call .here                      ; (the masks' place: this code's own, wherever it runs)
.here:  pop ax
        add ax,bl_masks-.here
        mov BL_MASKS,ax
        lfs bx,[bp+6]
        les ax,[fs:bx+0]                ; the band
        mov BL_BAND,ax
        mov eax,[fs:bx+4]               ; the opening's extents
        mov BL_EXT,eax
        lgs ax,[fs:bx+8]                ; the picture's rows, the dither, the work: one segment
        mov BL_PIC,ax
        mov ax,[fs:bx+12]
        mov BL_DITH,ax
        mov ax,[fs:bx+16]
        mov BL_PA,ax
        add ax,PV_LW*5
        mov BL_PB,ax
        add ax,PV_LW*5
        mov BL_RUNS,ax
        movsx eax,word [fs:bx+20]
        mov BL_Y0,eax
        mov BL_Y,eax
        movsx eax,word [fs:bx+22]
        mov BL_Y1,eax
        movsx eax,word [fs:bx+24]
        mov BL_EY0,eax
        movsx eax,word [fs:bx+26]
        mov BL_EROWS,eax
        movsx eax,word [fs:bx+28]
        mov BL_LY0,eax
        mov dword BL_LY,-1
        mov dword BL_NR,0
.y:     mov eax,BL_Y
        cmp eax,BL_Y1
        jl .y1
        jmp .done
.y1:    cdq                             ; cy = y / 5
        mov ecx,5
        idiv ecx
        mov BL_CY,eax
        cmp eax,BL_LY
        je .have
        mov BL_LY,eax
        call .build
.have:  mov dword BL_XA,0
        mov dword BL_XB,PV_LW*5
        cmp dword BL_EXT,0
        je .ex
        mov eax,BL_Y                    ; within the opening's rows; its extents
        sub eax,BL_EY0
        jl .next
        cmp eax,BL_EROWS
        jge .next
        shl ax,2
        lfs bx,BL_EXT
        add bx,ax
        movsx eax,word [fs:bx]
        mov BL_XA,eax
        movsx eax,word [fs:bx+2]
        mov BL_XB,eax
.ex:    mov eax,BL_CY                   ; the pattern: block rows 1 and 3 the second
        imul eax,eax,5
        neg eax
        add eax,BL_Y
        mov si,BL_PA
        test al,1
        jz .p
        mov si,BL_PB
.p:     mov eax,BL_Y                    ; the row
        sub eax,BL_Y0
        imul ax,ax,PV_LW*5
        add ax,BL_BAND
        mov di,ax
        mov bx,BL_RUNS
        mov cx,BL_NR
        push gs
        pop ds
.run:   jcxz .rund
        push cx
        movsx eax,word [bx]             ; x0 = max(run's first, xa), x1 = min(run's end, xb)
        cmp eax,BL_XA
        jge .r1
        mov eax,BL_XA
.r1:    movsx edx,word [bx+2]
        cmp edx,BL_XB
        jle .r2
        mov edx,BL_XB
.r2:    sub edx,eax
        jle .r3
        push si
        push di
        add si,ax
        add di,ax
        mov cx,dx
        shr cx,2
        rep movsd
        mov cx,dx
        and cx,3
        rep movsb
        pop di
        pop si
.r3:    add bx,4
        pop cx
        dec cx
        jmp .run
.rund:  mov ax,ss
        mov ds,ax
.next:  inc dword BL_Y
        jmp .y
.done:  pop gs
        pop fs
        pop ds
        pop di
        pop si
        mov sp,bp
        pop bp
        retf

; the picture row BL_LY's two patterns and its runs (ds is ss here)
.build: mov eax,BL_LY
        sub eax,BL_LY0
        shl ax,8
        add ax,BL_PIC
        mov BL_SRC,eax
        mov dword BL_NR,0
        mov dword BL_IN,0
        mov dword BL_X,0
        mov di,BL_PA
.bx:    mov si,BL_SRC
        mov ax,[gs:si]
        add word BL_SRC,2
        cmp ax,PV_EMPTY
        jne .bg
        cmp dword BL_IN,0               ; (no ground: a run ends)
        je .bnx
        mov bx,BL_NR
        shl bx,2
        add bx,BL_RUNS
        imul cx,BL_X,5
        mov [gs:bx+2],cx
        inc dword BL_NR
        mov dword BL_IN,0
        jmp .bnx
.bg:    cmp dword BL_IN,0               ; (ground: a run begins)
        jne .b1
        mov bx,BL_NR
        shl bx,2
        add bx,BL_RUNS
        imul cx,BL_X,5
        mov [gs:bx],cx
        mov dword BL_IN,1
.b1:    cmp ax,PV_GLOW                  ; the pair: the ground's (id | haze << 8), or the sky's at the glow's level
        jb .b2
        and ax,31
        add ax,PV_FOGS*256
.b2:    imul bx,ax,3
        add bx,BL_DITH
        movzx eax,byte [gs:bx]          ; a
        movzx edx,byte [gs:bx+1]        ; b
        movzx bx,byte [gs:bx+2]         ; t: its masks
        shl bx,4
        add bx,BL_MASKS
        xor edx,eax
        imul eax,eax,0x01010101         ; four a's, four (a ^ b)'s
        imul edx,edx,0x01010101
        mov ecx,edx                     ; the first pattern's five: a ^ ((a ^ b) & mask)
        and ecx,[cs:bx]
        xor ecx,eax
        mov [gs:di],ecx
        mov cl,dl
        and cl,[cs:bx+4]
        xor cl,al
        mov [gs:di+4],cl
        mov ecx,edx                     ; the second's
        and ecx,[cs:bx+8]
        xor ecx,eax
        mov [gs:di+PV_LW*5],ecx
        mov cl,dl
        and cl,[cs:bx+12]
        xor cl,al
        mov [gs:di+PV_LW*5+4],cl
.bnx:   add di,5
        inc dword BL_X
        cmp dword BL_X,PV_LW
        jge .bend
        jmp .bx
.bend:  cmp dword BL_IN,0
        je .bret
        mov bx,BL_NR
        shl bx,2
        add bx,BL_RUNS
        mov word [gs:bx+2],PV_LW*5
        inc dword BL_NR
.bret:  ret

; a dither's t: the block's thresholds under it (rows 0, 2, 4: 0 6 2 8 4; rows 1, 3: 5 1 7 3 9), as byte masks
bl_masks:
%assign t 0
%rep 11
        db (0<t)*255, (6<t)*255, (2<t)*255, (8<t)*255, (4<t)*255, 0, 0, 0
        db (5<t)*255, (1<t)*255, (7<t)*255, (3<t)*255, (9<t)*255, 0, 0, 0
%assign t t+1
%endrep
blit_end:
blob_end:
