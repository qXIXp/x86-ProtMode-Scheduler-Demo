; defs.inc — x86 保护模式基础定义文件
; 包含：段/门描述符属性常量、描述符构造宏、任务段生成宏、页表初始化宏、任务切换宏

; ================================================================
; 一、描述符属性位定义
; ================================================================

; 段描述符 G/D 位：决定段的粒度与操作数位宽
F_SEG32         EQU 4000h       ; D/B=1：32 位段（代码/栈默认操作数 32 位）
F_GRAN4K        EQU 8000h       ; G=1：段界限以 4KB 为单位（页粒度）

; 描述符特权级（DPL），写入 type 字节高两位
RING0           EQU 00h         ; DPL=0，内核态
RING1           EQU 20h         ; DPL=1
RING2           EQU 40h         ; DPL=2
RING3           EQU 60h         ; DPL=3，用户态

; ================================================================
; 二、存储段 type 字段（S=1，即非系统段）
; ================================================================

T_RD            EQU 90h         ; 只读数据段
T_RDWR          EQU 92h         ; 可读写数据段
T_RDWRA         EQU 93h         ; 可读写数据段（已访问标志置位）
T_CODE          EQU 98h         ; 只执行代码段
T_CODER         EQU 9Ah         ; 可执行可读代码段
T_CONFC         EQU 9Ch         ; 一致性只执行代码段
T_CONFCR        EQU 9Eh         ; 一致性可执行可读代码段

; ================================================================
; 三、系统段 type 字段（S=0，即系统段/门描述符）
; ================================================================

T_LDT           EQU 82h         ; 局部描述符表段（LDT）
T_TSKGATE       EQU 85h         ; 任务门
T_TSS386        EQU 89h         ; 386 可用任务状态段（TSS）
T_CALLG386      EQU 8Ch         ; 386 调用门
T_INTG386       EQU 8Eh         ; 386 中断门
T_TRAPG386      EQU 8Fh         ; 386 陷阱门

; ================================================================
; 四、选择子 TI 位（Table Indicator）与 RPL（请求特权级）
; ================================================================

RPL0            EQU 0           ; 请求特权级 0（最高权限）
RPL1            EQU 1
RPL2            EQU 2
RPL3            EQU 3           ; 请求特权级 3（用户态）

TI_GDT          EQU 0           ; TI=0：选择子指向 GDT
TI_LDT          EQU 4           ; TI=1（bit2=1）：选择子指向 LDT

; ================================================================
; 五、页表项标志位（低 12 位控制位）
; ================================================================

PF_P            EQU 1           ; Present：页在物理内存中
PF_RO           EQU 0           ; R/W=0：只读/执行
PF_RW           EQU 2           ; R/W=1：可读写执行
PF_SYS          EQU 0           ; U/S=0：仅内核可访问
PF_USR          EQU 4           ; U/S=1：用户态可访问

; ================================================================
; 宏 DescEntry base, limit, attr
; 按 Intel 手册格式拼装 8 字节段描述符（小端序存储）：
;   字节 0-1 : limit[15:0]
;   字节 2-3 : base[15:0]
;   字节 4   : base[23:16]
;   字节 5-6 : attr & 0xF0FF | (limit[19:16] << 8)
;   字节 7   : base[31:24]
; ================================================================
%macro DescEntry 3
    dw  (%2) & 0FFFFh               ; limit 低 16 位
    dw  (%1) & 0FFFFh               ; base  低 16 位
    db  ((%1) >> 16) & 0FFh         ; base  第 3 字节
    dw  (((%2) >> 8) & 0F00h) | ((%3) & 0F0FFh)  ; 属性 + limit[19:16]
    db  ((%1) >> 24) & 0FFh         ; base  高字节
%endmacro

; ================================================================
; 宏 GateEntry sel, offset, dcount, attr
; 拼装 8 字节门描述符（中断门/调用门等）：
;   字节 0-1 : offset[15:0]
;   字节 2-3 : 目标段选择子
;   字节 4-5 : 参数个数 & 属性
;   字节 6-7 : offset[31:16]
; ================================================================
%macro GateEntry 4
    dw  (%2) & 0FFFFh               ; 目标偏移低 16 位
    dw  %1                          ; 目标代码段选择子
    dw  ((%3) & 1Fh) | (((%4) << 8) & 0FF00h)  ; 参数个数 | 属性字节
    dw  ((%2) >> 16) & 0FFFFh       ; 目标偏移高 16 位
%endmacro

; ================================================================
; 宏 LoadBase  src_label, desc_label
; 在实模式下计算 src_label 的线性地址（cs << 4 + offset），
; 并将其拆分写入 desc_label 所对应描述符的三个基址字段
; ================================================================
%macro LoadBase 2
    push    ebx
    xor     ebx, ebx
    mov     bx,  cs             ; 段基址 = cs × 16
    shl     ebx, 4
    add     ebx, %1             ; + 标号段内偏移 = 物理地址
    mov     word [%2 + 2], bx   ; 写 base[15:0]
    shr     ebx, 16
    mov     byte [%2 + 4], bl   ; 写 base[23:16]
    mov     byte [%2 + 7], bh   ; 写 base[31:24]
    pop     ebx
%endmacro

; ================================================================
; 宏 LoadTaskBase  n
; 批量为任务 n 的 TSS、LDT、代码段、数据段、内核栈、用户栈
; 描述符写入正确的段基址（须在进入保护模式前调用）
; ================================================================
%macro LoadTaskBase 1
    LoadBase  TSS%1_START,        TSS%1_DESC        ; 任务状态段
    LoadBase  TASK%1_LDT_START,   TASK%1_LDT_DESC   ; 局部描述符表
    LoadBase  TASK%1_CODE_START,  TASK%1_CODE_DESC   ; 代码段
    LoadBase  TASK%1_DATA_START,  TASK%1_DATA_DESC   ; 数据段
    LoadBase  TASK%1_STK0_START,  TASK%1_STK0_DESC   ; 内核栈（ring0）
    LoadBase  TASK%1_STK3_START,  TASK%1_STK3_DESC   ; 用户栈（ring3）
%endmacro

; ================================================================
; 宏 MkLDT  n
; 为任务 n 生成局部描述符表（LDT）及相应选择子常量
; LDT 内描述符顺序：Code -> Data -> Stk3 -> Stk0
; 选择子 = 描述符在 LDT 内字节偏移 | TI_LDT | RPL
; ================================================================
%macro MkLDT 1
[SECTION .ldt%1]
ALIGN 32
TASK%1_LDT_START:
; 代码段：32 位，ring3 可执行
TASK%1_CODE_DESC:   DescEntry  0, Task%1CodeSize - 1,  T_CODE  + F_SEG32 + RING3
; 数据段：ring3 可读写
TASK%1_DATA_DESC:   DescEntry  0, Task%1DataSize - 1,  T_RDWRA + RING3
; 用户栈段：32 位，ring3 可读写
TASK%1_STK3_DESC:   DescEntry  0, Task%1Stk3Top,       T_RDWRA + F_SEG32 + RING3
; 内核栈段：32 位，ring0 可读写（由 TSS SS0 字段引用）
TASK%1_STK0_DESC:   DescEntry  0, Task%1Stk0Top,       T_RDWRA + F_SEG32
TASK%1_LDT_SIZE     equ $ - TASK%1_LDT_START           ; LDT 总字节数

; LDT 内选择子（TI=1 表示从 LDT 查找）
SelT%1Code   equ  TASK%1_CODE_DESC - TASK%1_LDT_START + TI_LDT + RPL3
SelT%1Data   equ  TASK%1_DATA_DESC - TASK%1_LDT_START + TI_LDT + RPL3
SelT%1Stk3   equ  TASK%1_STK3_DESC - TASK%1_LDT_START + TI_LDT + RPL3
SelT%1Stk0   equ  TASK%1_STK0_DESC - TASK%1_LDT_START + TI_LDT  ; ring0 栈 RPL=0
%endmacro

; ================================================================
; 宏 MkTData  n, msg
; 为任务 n 生成数据段，段首存放以 0 结尾的输出字符串 msg
; msgTask%1 为字符串在数据段内的偏移（供代码段寻址）
; ================================================================
%macro MkTData 2
[SECTION .td%1]
ALIGN 32
[BITS 32]
TASK%1_DATA_START:
    _msgTask%1:   db  %2, 0         ; 输出字符串，末尾 NUL 终止
    msgTask%1     equ _msgTask%1 - $$  ; 段内偏移别名（供代码段通过 ds 访问）
Task%1DataSize    equ $ - TASK%1_DATA_START
%endmacro

; ================================================================
; 宏 MkTCode  n, row, attr
; 为任务 n 生成代码段：
;   将数据段字符串逐字符（字符+属性）写入第 row 行显存，然后无限循环。
;   gs 段已在 GoTask 切换前通过 GDT_VIDEO 指向 0xB8000 显存区域。
;   VGA 文本模式每个字符单元 2 字节：低字节=ASCII，高字节=颜色属性
; ================================================================
%macro MkTCode 3
[SECTION .tc%1]
ALIGN 32
[BITS 32]
TASK%1_CODE_START:
    mov     ah, %3              ; ah = 颜色属性，整个循环中保持不变
    xor     ecx, ecx           ; ecx = 字符串索引，从第 0 个字符开始
%%wloop:
    mov     al, [msgTask%1 + ecx]           ; 从数据段取第 ecx 个字符
    mov     [gs : (80 * %2 + ecx) * 2], ax ; 写显存：列=ecx，行=%2（每行 80 列）
    inc     ecx
    test    al, al              ; al=0 表示字符串末尾
    jnz     %%wloop             ; 未结束则继续输出下一个字符
    jmp     TASK%1_CODE_START   ; 全部输出完毕，跳回段首重新刷写（持续显示）
Task%1CodeSize  equ $ - TASK%1_CODE_START
%endmacro

; ================================================================
; 宏 MkKStack  n   —— 任务 n 内核栈（ring0，512 字节）
; CPU 从 ring3 陷入 ring0 时，自动将 SS:ESP 切换到
; TSS 中的 SS0:ESP0，即本段顶部地址（Task%1Stk0Top）
; ================================================================
%macro MkKStack 1
[SECTION .ts0_%1]
ALIGN 32
[BITS 32]
TASK%1_STK0_START:
    times 512 db 0              ; 512 字节内核栈空间，初始化为 0
Task%1Stk0Top   equ $ - TASK%1_STK0_START - 1  ; 栈顶偏移（高地址端）
%endmacro

; ================================================================
; 宏 MkUStack  n   —— 任务 n 用户栈（ring3，512 字节）
; GoTask 宏通过 iretd 将 Task%1Stk3Top 弹入 ESP，切换到此用户栈
; ================================================================
%macro MkUStack 1
[SECTION .ts3_%1]
ALIGN 32
[BITS 32]
TASK%1_STK3_START:
    times 512 db 0              ; 512 字节用户栈空间，初始化为 0
Task%1Stk3Top   equ $ - TASK%1_STK3_START - 1
%endmacro

; ================================================================
; 宏 MkTSS  n   —— 任务 n 的任务状态段（TSS）
; TSS 由 CPU 在特权级切换时自动读取，关键字段：
;   ESP0/SS0：从 ring3 陷入 ring0 时切换到的内核栈
;   I/O 位图偏移：指向 TSS 末尾，配合结束标记 0xFF 表示不限制端口访问
; ================================================================
%macro MkTSS 1
[SECTION .tss%1]
ALIGN 32
[BITS 32]
TSS%1_START:
    DD  0               ; 前一任务 TSS 选择子（链式切换用，此处置 0）
    DD  KSTACK_TOP      ; ESP0：ring0 内核栈顶（四个任务共用同一内核栈）
    DD  SelStack        ; SS0：ring0 栈段选择子
    DD  0               ; ESP1（未使用）
    DD  0               ; SS1
    DD  0               ; ESP2（未使用）
    DD  0               ; SS2
    DD  0               ; CR3（页目录基址，由 GoTask 宏通过 mov cr3 动态设置）
    DD  0               ; EIP（由 iretd 从栈帧设置）
    DD  0               ; EFLAGS
    DD  0               ; EAX
    DD  0               ; ECX
    DD  0               ; EDX
    DD  0               ; EBX
    DD  0               ; ESP
    DD  0               ; EBP
    DD  0               ; ESI
    DD  0               ; EDI
    DD  0               ; ES
    DD  0               ; CS
    DD  0               ; SS
    DD  0               ; DS
    DD  0               ; FS
    DD  0               ; GS
    DD  0               ; LDTR（LDT 选择子，由 GoTask 宏通过 lldt 动态加载）
    DW  0               ; 调试陷阱标志（T=0，不在任务切换时触发 #DB）
    DW  $ - TSS%1_START + 2  ; I/O 位图基址偏移（指向 TSS 末尾之后）
    DB  0FFh            ; I/O 位图结束标记（所有位为 1 = 所有端口允许访问）
TSS%1_SIZE  equ $ - TSS%1_START
%endmacro

; ================================================================
; 宏 MkTask  n, msg, row, attr
; 一次性构建任务 n 所需的全部二进制段，生成顺序：
;   TSS -> LDT -> 代码段 -> 数据段 -> 内核栈 -> 用户栈
; ================================================================
%macro MkTask 4
    MkTSS       %1              ; 任务状态段
    MkLDT       %1              ; 局部描述符表
    MkTCode     %1, %3, %4     ; 代码段（输出第 %3 行，颜色属性 %4）
    MkTData     %1, %2         ; 数据段（输出字符串 %2）
    MkKStack    %1              ; 内核栈（ring0，CPU 特权切换时自动使用）
    MkUStack    %1              ; 用户栈（ring3，iretd 切换时装入 SS:ESP）
%endmacro

; ================================================================
; 宏 BuildPT  n
; 在保护模式下为任务 n 构建页目录（PD）与页表（PT），
; 实现物理地址恒等映射（虚拟地址 = 物理地址）：
;   步骤 1：向页目录填写 ptCount 个 PDE，每个 PDE 指向一张页表
;   步骤 2：向页表填写 ptCount×1024 个 PTE，物理帧从 0 线性递增
; 调用前 es 必须已设为 SelFlatRW（4GB 平坦段），以便访问高地址物理页
; ================================================================
%macro BuildPT 1
PAGE_INIT%1:
    mov     ax,  SelFlatRW
    mov     es,  ax                         ; es = 4GB 平坦读写段，可寻址全部物理内存
    ; --- 步骤 1：填写页目录项（PDE） ---
    mov     edi, PageDir%1                  ; 目标：页目录物理地址
    mov     eax, PageTbl%1                  ; 第一张页表的物理起始地址
    or      eax, PF_P | PF_USR | PF_RW     ; 设置存在/用户可读/读写标志
    mov     ecx, [ptCount]                  ; 循环次数 = 需要的页表数量
%%pde:
    stosd                                   ; 将 eax 写入 es:edi，edi 自动 +4
    add     eax, 4096                       ; 下一张页表地址（每张页表占 4KB）
    loop    %%pde
    ; --- 步骤 2：填写页表项（PTE） ---
    mov     eax, [ptCount]
    imul    eax, 1024                       ; 总 PTE 数 = ptCount × 1024（每表 1K 项）
    mov     ecx, eax
    mov     edi, PageTbl%1                  ; 从第一张页表起始处写
    mov     eax, PF_P | PF_USR | PF_RW     ; 物理帧 0 的 PTE（地址域 = 0）
%%pte:
    stosd                                   ; 写入 PTE，物理地址已含在 eax 高 20 位
    add     eax, 4096                       ; 下一个物理页帧（+4KB）
    loop    %%pte
    ret
%endmacro

; ================================================================
; 宏 GoTask  n
; 通过构造 iretd 所需的五元素栈帧，从 ring0 跳转到任务 n 的 ring3 代码。
; iretd 从栈中依次弹出：EIP -> CS -> EFLAGS -> ESP -> SS
; 调用前提：ds=SelData（可访问调度数据区），esp=KSTACK_TOP（栈已重置）
; ================================================================
%macro GoTask 1
    mov     ax,  SelLDT%1           ; 取任务 n 的 LDT 选择子
    lldt    ax                      ; 将 LDTR 指向该任务的 LDT
    mov     eax, PageDir%1
    mov     cr3, eax                ; 切换页目录基址，同时刷新 TLB
    mov     eax, SelT%1Data
    mov     ds,  eax                ; ds 指向任务 n 数据段（代码段寻址字符串时使用）
    push    dword SelT%1Stk3        ; SS（ring3 用户栈段选择子）
    push    dword Task%1Stk3Top     ; ESP（用户栈顶偏移）
    pushfd                          ; 将当前 EFLAGS 压栈，准备修改
    pop     eax
    or      eax, 200h               ; 置位 IF（Interrupt Flag），进入用户态后允许中断
    push    eax                     ; 推回修改后的 EFLAGS
    push    dword SelT%1Code        ; CS（ring3 代码段选择子，DPL=3）
    push    dword 0                 ; EIP=0：从代码段偏移 0 处开始执行
    iretd                           ; 弹出栈帧，特权级降至 ring3，开始执行任务
%endmacro
