; bake.asm: a baked planet's ground asked (bake.h).  rv_bk_column does what
; bake.c's c_bk_column does, to the bit (RVP.EXE /twins compares them), and
; rv_bk_muldiv what c_bk_muldiv does.  The sums are 64-bit products and
; quotients, each one instruction of a 386's; Watcom's 16-bit C makes a call
; of each and a page of code of the lot, in the conventional memory RV.EXE
; has least of.  The file's bytes come from XMS by the driver's own move.
; Far cdecl, as the medium model calls them; SI DI BP DS kept; SS is DGROUP
; (the driver reads its record at ds:si, the record is in the frame).
        cpu 486
        extern _rv_xms_entry            ; xms.c's: the driver's entry (far pointer, DGROUP)
        extern _bk_ask                  ; bake.c's BkAsk (far data): the planet being asked

segment BAKE_TEXT public align=16 class=CODE use16

%define ASK(k) [fs:_bk_ask+(k)]         ; BkAsk's fields
%define A_HANDLE 0
%define A_NPATCH 2
%define A_BASE 4
%define A_RADIUS 8
%define A_GRID 12
%define A_PATCHES 16
%define A_N 20
%define A_Q 24                          ; BkQuick [12]: c[3], cosmin (8 bytes)

%define PX    [bp+6]                    ; the point
%define PY    [bp+10]
%define PZ    [bp+14]
%define OUT   [bp+18]                   ; BkColumn near *: ground, slab_lo, slab_hi, r
%define XM_LEN   [bp-16]                ; the move's record (16 bytes): length,
%define XM_SH    [bp-12]                ;   source handle, source offset,
%define XM_SOFF  [bp-10]
%define XM_DH    [bp-6]                 ;   destination handle (0), destination seg:off
%define XM_DOFF  [bp-4]
%define XM_DSEG  [bp-2]
%define PT(k) [bp-84+(k)]               ; a patch's record (BkPatch, 68 bytes)
%define PT_C  -84                       ;   its vectors' places in the frame
%define PT_A  -84+12
%define PT_B  -84+24
%define P_HALF_A 36
%define P_HALF_B 40
%define P_CELL 44
%define P_NA 48
%define P_NB 50
%define P_SLABS 58
%define P_AT 60
%define P_SLAB_AT 64
%define E(k)  [bp-96+(k)]               ; four cells: two of a row, two of the next (3 bytes each)
%define M     [bp-100]                  ; the point's largest part
%define KK    [bp-104]                  ; a patch: the point's part along its middle over the radius
%define GX    [bp-108]                  ; the place on a grid, 65536ths of a cell; then the cell
%define GY    [bp-112]
%define FX    [bp-116]                  ; its part of a cell
%define FY    [bp-120]
%define AT    [bp-124]                  ; a patch's point's index
%define NA1   [bp-128]                  ; its row's points
%define CAT   [bp-132]                  ; cells': the first row's place in the file, a row's bytes
%define CROW  [bp-136]
%define D0    [bp-138]                  ; the point's direction in 16 bits
%define D1    [bp-140]
%define D2    [bp-142]
%define LEN   [bp-144]                  ;   and its length
%define AXIS  [bp-146]
%define K     [bp-148]                  ; the patch tried
%define FACE  [bp-150]
%define CFRAME 150

; the floor of the root of edi:esi (under 2^64) in eax: Newton's steps down from
; a power of two over the root (c_isqrt3's steps; c_isqrt32's answer).  ebx ecx edx used
isqrt:  xor eax,eax
        mov ecx,edi
        or ecx,esi
        jz .done
        bsr ecx,edi                 ; the highest bit
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
.step:  mov eax,esi                 ; q = (r + n / r) / 2
        mov edx,edi
        div ebx
        add eax,ebx
        rcr eax,1
        cmp eax,ebx
        jae .root
        mov ebx,eax
        jmp .step
.root:  mov eax,ebx
.done:  ret

; cx bytes (even) of the file from eax to ss:di (the frame).  eax ecx used, and what the driver changes (ax, bl)
fetch:  add eax,ASK(A_BASE)
        mov XM_SOFF,eax
        movzx ecx,cx
        mov XM_LEN,ecx
        mov ax,ASK(A_HANDLE)
        mov XM_SH,ax
        mov word XM_DH,0
        mov XM_DOFF,di
        mov XM_DSEG,ss
        push si
        lea si,XM_LEN               ; (ds:si the record: ss is DGROUP, as ds)
        mov ah,0x0B
        call far [_rv_xms_entry]
        pop si
        ret

; four cells into E: two at eax of the file, the two a row (edx bytes) on
cells:  mov CAT,eax
        mov CROW,edx
        lea di,E(0)
        mov cx,6
        call fetch
        mov eax,CAT
        add eax,CROW
        lea di,E(6)
        mov cx,6
        call fetch
        ret

; eax = eax + (edx - eax) ebx / 65536 (the product whole; bake.c's BK_LERP).  ecx edx used
lerp:   mov ecx,eax
        mov eax,edx
        sub eax,ecx
        imul ebx
        shrd eax,edx,16
        add eax,ecx
        ret

; E's four heights (height units: 64 positions each) weighed by FX and FY: eax, positions
weigh:  movsx eax,word E(0)
        shl eax,6
        movsx edx,word E(3)
        shl edx,6
        mov ebx,FX
        call lerp
        push eax
        movsx eax,word E(6)
        shl eax,6
        movsx edx,word E(9)
        shl edx,6
        mov ebx,FX
        call lerp
        mov edx,eax
        pop eax
        mov ebx,FY
        call lerp
        ret

; edx:eax = the point . a patch's vector (si: its place in the frame).  ebx ecx used
dot3:   mov eax,PX
        imul dword [bp+si]
        mov ecx,eax
        mov ebx,edx
        mov eax,PY
        imul dword [bp+si+4]
        add ecx,eax
        adc ebx,edx
        mov eax,PZ
        imul dword [bp+si+8]
        add eax,ecx
        adc edx,ebx
        ret

; eax = edx:eax / KK, towards 0 (KK 1 .. 2^31 - 1); carry when it does not fit 32 bits signed.  ebx ecx used
divk:   mov ecx,edx                 ; (the sign)
        or edx,edx
        jns .pos
        not edx
        neg eax
        sbb edx,-1
.pos:   mov ebx,edx                 ; fits when |n| / 2^31 is under KK
        shld ebx,eax,1
        cmp ebx,KK
        jae .wide
        div dword KK
        or ecx,ecx
        jns .fits
        neg eax
.fits:  clc
        ret
.wide:  stc
        ret

; di the next axis round (0 1 2 0), and eax the point's part along it on a face's grid:
; c[di] n 32768 / M + (n - 1) 32768.  ecx edx used
along:  inc di
        cmp di,3
        jb .axis
        xor di,di
.axis:  mov si,di
        shl si,2
        movzx ecx,word ASK(A_N)
        shl ecx,15
        mov eax,[bp+si+6]
        imul ecx
        idiv dword M
        sub ecx,32768
        add eax,ecx
        ret

; void rv_bk_column(long x, long y, long z, BkColumn near *out)
        global _rv_bk_column
_rv_bk_column:
        push bp
        mov bp,sp
        sub sp,CFRAME
        push si
        push di
        mov ax,seg _bk_ask
        mov fs,ax
        mov eax,PX                  ; the point's radius: the root of x^2 + y^2 + z^2, 2^31 - 1 at most
        imul eax
        mov esi,eax
        mov edi,edx
        mov eax,PY
        imul eax
        add esi,eax
        adc edi,edx
        mov eax,PZ
        imul eax
        add esi,eax
        adc edi,edx
        call isqrt
        cmp eax,0x7FFFFFFF
        jbe .r
        mov eax,0x7FFFFFFF
.r:     mov bx,OUT
        mov [bx+12],eax
        xor eax,eax
        mov [bx],eax
        mov [bx+4],eax
        mov [bx+8],eax
        cmp word ASK(A_N),0         ; no planet: ground 0
        je .out
        mov eax,PX                  ; the largest of |x| |y| |z| (the first of equals), and whose
        cdq
        xor eax,edx
        sub eax,edx
        mov ebx,eax
        xor si,si
        mov eax,PY
        cdq
        xor eax,edx
        sub eax,edx
        cmp eax,ebx
        jle .a1
        mov ebx,eax
        mov si,1
.a1:    mov eax,PZ
        cdq
        xor eax,edx
        sub eax,edx
        cmp eax,ebx
        jle .a2
        mov ebx,eax
        mov si,2
.a2:    mov M,ebx
        mov AXIS,si
        xor eax,eax
        mov GX,eax                  ; (the centre itself: the first face's first cell)
        mov GY,eax
        mov FACE,ax
        or ebx,ebx
        jz .grid
        cmp word ASK(A_NPATCH),0
        je .face
        ; the mission's patches first.  The direction in 16 bits (its largest part under 16384) and its length
        bsr ecx,ebx
        sub cl,13
        jg .sh
        xor cl,cl
.sh:    mov eax,PX
        sar eax,cl
        mov D0,ax
        movsx eax,ax
        imul eax,eax
        mov esi,eax
        mov eax,PY
        sar eax,cl
        mov D1,ax
        movsx eax,ax
        imul eax,eax
        add esi,eax
        mov eax,PZ
        sar eax,cl
        mov D2,ax
        movsx eax,ax
        imul eax,eax
        add esi,eax
        xor edi,edi
        call isqrt
        mov LEN,ax
        mov word K,0
.patch: mov bx,K                    ; far from its middle (the part along it under cosmin times the length): not in it
        shl bx,3
        movsx eax,word D0
        movsx edx,word ASK(A_Q+bx)
        imul eax,edx
        mov ecx,eax
        movsx eax,word D1
        movsx edx,word ASK(A_Q+2+bx)
        imul eax,edx
        add ecx,eax
        movsx eax,word D2
        movsx edx,word ASK(A_Q+4+bx)
        imul eax,edx
        add ecx,eax
        movsx eax,word LEN
        movsx edx,word ASK(A_Q+6+bx)
        imul eax,edx
        cmp ecx,eax
        jl .nextp
        movzx eax,word K            ; near: its record from the file
        imul eax,eax,68
        add eax,ASK(A_PATCHES)
        lea di,PT(0)
        mov cx,68
        call fetch
        mov si,PT_C                 ; the point along its middle, over the radius: 1 .. 2^31 - 1 (2^30: on the datum)
        call dot3
        or edx,edx
        js .nextp
        mov ebx,edx
        shld ebx,eax,1
        cmp ebx,ASK(A_RADIUS)
        jae .nextp
        div dword ASK(A_RADIUS)
        or eax,eax
        jz .nextp
        mov KK,eax
        mov si,PT_A                 ; its place on the tangent plane, each way: within the patch's half length
        call dot3
        call divk
        jc .nextp
        cmp eax,PT(P_HALF_A)
        jge .nextp
        add eax,PT(P_HALF_A)
        jle .nextp
        mov edx,eax                 ; in cells, 65536ths
        shl eax,16
        sar edx,16
        idiv dword PT(P_CELL)
        mov GX,eax
        mov si,PT_B
        call dot3
        call divk
        jc .nextp
        cmp eax,PT(P_HALF_B)
        jge .nextp
        add eax,PT(P_HALF_B)
        jle .nextp
        mov edx,eax
        shl eax,16
        sar edx,16
        idiv dword PT(P_CELL)
        ; in the patch: its cell (the last one's far edge: the whole of it), the four points round the place
        movzx ebx,ax
        shr eax,16
        movzx edx,word PT(P_NB)
        cmp eax,edx
        jb .j
        lea eax,[edx-1]
        mov ebx,65535
.j:     mov FY,ebx
        mov esi,eax                 ; (j)
        mov eax,GX
        movzx ebx,ax
        shr eax,16
        movzx ecx,word PT(P_NA)
        cmp eax,ecx
        jb .i
        lea eax,[ecx-1]
        mov ebx,65535
.i:     mov FX,ebx
        inc ecx
        mov NA1,ecx
        imul esi,ecx                ; the point's index: j (na + 1) + i
        add eax,esi
        mov AT,eax
        lea eax,[eax+eax*2]
        add eax,PT(P_AT)
        lea edx,[ecx+ecx*2]
        call cells
        call weigh
        add eax,ASK(A_RADIUS)
        mov bx,OUT
        mov [bx],eax
        cmp word PT(P_SLABS),0
        je .out
        mov eax,AT                  ; the slab of the point nearest
        cmp dword FY,32768
        jb .s1
        add eax,NA1
.s1:    cmp dword FX,32768
        jb .s2
        inc eax
.s2:    shl eax,2
        add eax,PT(P_SLAB_AT)
        lea di,E(0)
        mov cx,4
        call fetch
        movsx eax,word E(0)
        movsx edx,word E(2)
        cmp eax,edx
        jge .out
        shl eax,6
        add eax,ASK(A_RADIUS)
        shl edx,6
        add edx,ASK(A_RADIUS)
        mov bx,OUT
        mov [bx+4],eax
        mov [bx+8],edx
        jmp .out
.nextp: inc word K
        mov ax,K
        cmp ax,ASK(A_NPATCH)
        jb .patch
        ; the planet's own ground: the face the point is over (2 axis, + 1 on the minus side), its place on the
        ; face's grid along the next axis and the one after
.face:  mov di,AXIS
        mov si,di
        shl si,2
        mov eax,[bp+si+6]
        shr eax,31
        add ax,di
        add ax,di
        mov FACE,ax
        call along
        mov GX,eax
        call along
        mov GY,eax
.grid:  mov eax,GX                  ; the cell (-1 .. n - 1: the face's rows have a cell of border) and the part of it
        movzx ebx,ax
        mov FX,ebx
        sar eax,16
        inc eax
        mov esi,eax
        mov eax,GY
        movzx ebx,ax
        mov FY,ebx
        sar eax,16
        inc eax
        movzx ecx,word ASK(A_N)
        add ecx,2                   ; a row's cells
        movzx edx,word FACE
        imul edx,ecx
        add eax,edx
        imul eax,ecx
        add eax,esi
        lea eax,[eax+eax*2]
        add eax,ASK(A_GRID)
        lea edx,[ecx+ecx*2]
        call cells
        call weigh
        add eax,ASK(A_RADIUS)
        mov bx,OUT
        mov [bx],eax
.out:   pop di
        pop si
        mov sp,bp
        pop bp
        retf

; int rv_bk_hides(const long far *eye, const long far *p, int bottom, int top, int ground): the line from the eye to p
; (planet-relative positions) passes under the ground: its point nearest the centre (found in 32 km units) under the
; lowest ground (radius + bottom height units), or, with ground, the ground (rv_bk_column) at 8 of its points (ninths of
; the way) of those under the highest (top) (c_bk_hides)
%define HEYE  [bp+6]
%define HP    [bp+10]
%define HBOT  [bp+14]
%define HTOP  [bp+16]
%define HG    [bp+18]
%define HE    -12                       ; the eye
%define HD    -24                       ; p - eye
%define HQ    -36                       ; a point of the line
%define HC    -52                       ; BkColumn: ground, slab_lo, slab_hi, r
%define HI    [bp-56]
        global _rv_bk_hides
_rv_bk_hides:
        push bp
        mov bp,sp
        sub sp,56
        push si
        push di
        xor edi,edi                     ; ed, dd (32 km units)
        xor ecx,ecx
        xor si,si
.e:     les bx,HEYE
        mov edx,[es:bx+si]
        mov [bp+si+HE],edx
        les bx,HP
        mov eax,[es:bx+si]
        sub eax,edx
        mov [bp+si+HD],eax
        sar eax,13
        sar edx,13
        imul edx,eax
        add edi,edx
        imul eax,eax
        add ecx,eax
        add si,4
        cmp si,12
        jb .e
        cmp ecx,256                     ; its nearest to the centre on it: t = -ed / (dd >> 8), 256ths of the way
        jl .samples
        neg edi
        jle .samples
        cmp edi,ecx
        jge .samples
        shr ecx,8
        mov eax,edi
        xor edx,edx
        div ecx
        mov edi,eax
        mov ebx,256
        call point
        mov ax,HBOT
        call under
        jl .yes
.samples:
        cmp word HG,0
        je .no
        mov dword HI,1
.s:     mov edi,HI
        mov ebx,9
        call point
        mov ax,HTOP
        call under
        jge .sn
        lea ax,[bp+HC]
        push ax
        push dword [bp+HQ+8]
        push dword [bp+HQ+4]
        push dword [bp+HQ]
        push cs
        call _rv_bk_column
        add sp,14
        mov eax,[bp+HC+12]
        cmp eax,[bp+HC]                 ; under the ground, or within a slab
        jl .yes
        mov edx,[bp+HC+8]
        test edx,edx
        jz .sn
        cmp eax,[bp+HC+4]
        jl .sn
        cmp eax,edx
        jle .yes
.sn:    inc dword HI
        cmp dword HI,8
        jle .s
.no:    xor ax,ax
        jmp .out
.yes:   mov ax,1
.out:   pop di
        pop si
        mov sp,bp
        pop bp
        retf

; the line's point HE + HD / ebx * edi into HQ (si used)
point:  xor si,si
.p:     mov eax,[bp+si+HD]
        cdq
        idiv ebx
        imul eax,edi
        add eax,[bp+si+HE]
        mov [bp+si+HQ],eax
        add si,4
        cmp si,12
        jb .p
        ret

; HQ against the radius + ax height units: its parts' (q >> 11) squared summed, compared (cmp) with that radius's
; (>> 11) squared: less when it is nearer the centre (ebx ecx si used)
under:  movsx eax,ax
        shl eax,6
        push fs
        mov cx,seg _bk_ask
        mov fs,cx
        add eax,ASK(A_RADIUS)
        pop fs
        sar eax,11
        imul eax,eax
        mov ecx,eax
        xor ebx,ebx
        xor si,si
.u:     mov eax,[bp+si+HQ]
        sar eax,11
        imul eax,eax
        add ebx,eax
        add si,4
        cmp si,12
        jb .u
        cmp ebx,ecx
        ret

; long rv_bk_muldiv(long a, long b, long c): a b / c, the product whole
        global _rv_bk_muldiv
_rv_bk_muldiv:
        push bp
        mov bp,sp
        mov eax,[bp+6]
        imul dword [bp+10]
        idiv dword [bp+14]
        mov edx,eax
        shr edx,16
        pop bp
        retf
