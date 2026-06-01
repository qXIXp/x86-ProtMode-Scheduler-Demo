; main.asm — 四任务优先数时间片调度演示
;             平台：Bochs + FreeDOS，以 COM 程序格式引导进入 x86 保护模式
;
; ────────────────────────────────────────────────────────────────
; 调度策略（优先数算法）：
;   时钟频率 20Hz（每 tick = 50ms）。
;   四个任务的优先级（初始时间片配额）为 16 / 10 / 8 / 6。
;   每次 IRQ0 时钟中断：
;     ① 递减当前任务剩余时间片 remTicks[curTask]
;     ② 若未归零 → 原任务继续运行（直接 iretd 返回）
;     ③ 若归零   → 在所有任务中选出 remTicks 最大者（优先级最高）
;     ④ 若全部归零 → 按 taskQuanta 重填 remTicks，再执行步骤③
;   单次可见持续时长正比于优先级：
;     VERY 800ms | LOVE 500ms | HUST 400ms | MRSU 300ms
; ────────────────────────────────────────────────────────────────

%include "defs.inc"

; ================================================================
; 各任务页目录（PD）与页表（PT）的物理基地址
; 从 2MB 处起，每组间隔 64KB，互不重叠
; ================================================================
PageDir0    equ 200000h     ; 任务 0 页目录：物理地址 2MB
PageTbl0    equ 201000h     ; 任务 0 页表  ：物理地址 2MB + 4KB
PageDir1    equ 210000h     ; 任务 1 页目录：物理地址 2MB + 64KB
PageTbl1    equ 211000h
PageDir2    equ 220000h     ; 任务 2 页目录：物理地址 2MB + 128KB
PageTbl2    equ 221000h
PageDir3    equ 230000h     ; 任务 3 页目录：物理地址 2MB + 192KB
PageTbl3    equ 231000h

org 0100h                   ; COM 程序从 CS:0100h 开始执行
    jmp     ENTRY_POINT     ; 跳过数据区，进入 16 位初始化代码

; ================================================================
; GDT — 全局描述符表
; 包含：空描述符、平坦段、32/16 位代码段、数据段、栈段、显存段
;       以及四个任务各自的 TSS 描述符和 LDT 描述符
; ================================================================
[SECTION .gdt]
GDT_START:
GDT_NULL:    DescEntry  0,        0,              0           ; 空描述符（索引 0 保留）
GDT_NORMAL:  DescEntry  0,        0ffffh,         T_RDWR      ; 16 位平坦数据段（切回实模式用）
GDT_FLAT_C:  DescEntry  0,        0fffffh,        T_CODER | F_SEG32 | F_GRAN4K  ; 4GB 平坦代码段
GDT_FLAT_RW: DescEntry  0,        0fffffh,        T_RDWR  | F_GRAN4K            ; 4GB 平坦读写段（建页表用）
GDT_CODE32:  DescEntry  0,        Code32Size - 1, T_CODER | F_SEG32  ; 32 位主代码段
GDT_DATA:    DescEntry  0,        DataSize   - 1, T_RDWR              ; 内核数据段
GDT_CODE16:  DescEntry  0,        0ffffh,         T_CODE              ; 16 位代码段（保护→实模式过渡）
GDT_STACK:   DescEntry  0,        KSTACK_TOP,     T_RDWRA | F_SEG32  ; 内核栈段
GDT_VIDEO:   DescEntry  0B8000h,  0ffffh,         T_RDWR  + RING3    ; VGA 显存段（ring3 可访问）

; 四个任务的 TSS 描述符（CPU 需要通过 ltr 加载，且每次特权切换时查找 SS0:ESP0）
TSS0_DESC:      DescEntry  0,  TSS0_SIZE - 1,       T_TSS386
TSS1_DESC:      DescEntry  0,  TSS1_SIZE - 1,       T_TSS386
TSS2_DESC:      DescEntry  0,  TSS2_SIZE - 1,       T_TSS386
TSS3_DESC:      DescEntry  0,  TSS3_SIZE - 1,       T_TSS386

; 四个任务的 LDT 描述符（lldt 指令通过这里找到各任务的 LDT）
TASK0_LDT_DESC: DescEntry  0,  TASK0_LDT_SIZE - 1, T_LDT
TASK1_LDT_DESC: DescEntry  0,  TASK1_LDT_SIZE - 1, T_LDT
TASK2_LDT_DESC: DescEntry  0,  TASK2_LDT_SIZE - 1, T_LDT
TASK3_LDT_DESC: DescEntry  0,  TASK3_LDT_SIZE - 1, T_LDT

GDT_LEN  equ $ - GDT_START
GdtPtr   dw  GDT_LEN - 1   ; GDT 界限（字节数 - 1）
         dd  0              ; GDT 基址（在实模式初始化时回填）

; ---- GDT 选择子（描述符在 GDT 中的字节偏移即为选择子值，RPL=0） ----
SelNormal  equ GDT_NORMAL  - GDT_START  ; 16 位平坦段（切回实模式时用）
SelFlatC   equ GDT_FLAT_C  - GDT_START  ; 4GB 平坦代码段
SelFlatRW  equ GDT_FLAT_RW - GDT_START  ; 4GB 平坦读写段
SelCode32  equ GDT_CODE32  - GDT_START  ; 32 位主代码段
SelData    equ GDT_DATA    - GDT_START  ; 内核数据段
SelCode16  equ GDT_CODE16  - GDT_START  ; 16 位过渡代码段
SelStack   equ GDT_STACK   - GDT_START  ; 内核栈段
SelVideo   equ GDT_VIDEO   - GDT_START  ; VGA 显存段
SelTSS0    equ TSS0_DESC   - GDT_START
SelTSS1    equ TSS1_DESC   - GDT_START
SelTSS2    equ TSS2_DESC   - GDT_START
SelTSS3    equ TSS3_DESC   - GDT_START
SelLDT0    equ TASK0_LDT_DESC - GDT_START
SelLDT1    equ TASK1_LDT_DESC - GDT_START
SelLDT2    equ TASK2_LDT_DESC - GDT_START
SelLDT3    equ TASK3_LDT_DESC - GDT_START

; ================================================================
; 生成四个任务的全部段结构
; 参数：任务号, 输出字符串, 输出行号, 颜色属性
; 所有任务均输出到第 20 行，颜色不同以区分各任务
; ================================================================
MkTask  0, "VERY", 20, 0Ch  ; 优先级 16，亮红色（属性 0x0C = 黑底亮红）
MkTask  1, "LOVE", 20, 0Dh  ; 优先级 10，亮品红（属性 0x0D）
MkTask  2, "HUST", 20, 0Eh  ; 优先级  8，亮黄色（属性 0x0E）
MkTask  3, "MRSU", 20, 0Fh  ; 优先级  6，亮白色（属性 0x0F）

; ================================================================
; IDT — 中断描述符表（共 128 个门描述符）
; 0x00-0x1F（共 32 个）：CPU 异常，全部指向伪中断处理程序
; 0x20        ：IRQ0 时钟中断，指向 timerISR（调度核心）
; 0x21-0x7F（共 95 个）：保留，指向伪中断处理程序
; 0x80        ：软中断（INT 80h），指向 swIntISR
; ================================================================
[SECTION .idt]
ALIGN 32
[BITS 32]
IDT_START:
%rep 32
    GateEntry  SelCode32, spuriousISR, 0, T_INTG386  ; CPU 异常（0x00-0x1F）
%endrep
    GateEntry  SelCode32, timerISR,    0, T_INTG386  ; IRQ0 时钟中断（0x20）
%rep 95
    GateEntry  SelCode32, spuriousISR, 0, T_INTG386  ; 保留向量（0x21-0x7F）
%endrep
    GateEntry  SelCode32, swIntISR,    0, T_INTG386  ; 软中断（0x80）

IDT_LEN  equ $ - IDT_START
IdtPtr   dw  IDT_LEN - 1   ; IDT 界限
         dd  0              ; IDT 基址（实模式初始化时回填）

; ================================================================
; 数据段
; ================================================================
[SECTION .data1]
ALIGN 32
[BITS 32]
DATA_START:

; ---- 屏幕输出字符串 ----
_msgProtMode:  db  "Protected Mode Active", 0Ah, 0Ah, 0     ; 进入保护模式提示
_msgMemHdr:    db  "BaseAddrL BaseAddrH LengthLow LengthHigh   Type", 0Ah, 0  ; 内存表头
_msgRamSize:   db  "RAM size:", 0                            ; 内存总量标签
_msgCRLF:      db  0Ah, 0                                    ; 换行符
_msgReady:     db  "Scheduling: VERY(16) LOVE(10) HUST(8) MRSU(6)", 0  ; 调度说明

; ---- 系统变量 ----
_wRealSP:      dw  0           ; 保存实模式下的 SP，返回时恢复
_mcrCount:     dd  0           ; E820 探测到的 ARDS（地址范围描述符）条目数
_dispPos:      dd  (80 * 6) * 2  ; 当前显示位置（显存字节偏移，初始第 6 行第 0 列）
_memSize:      dd  0           ; 可用内存最大上界（字节，由 ShowMemInfo 计算）

; ---- ARDS 结构体解析缓冲区（每次遍历一条 ARDS 时使用） ----
_ards:
    _baseAddrLo: dd  0         ; 内存区域起始地址低 32 位
    _baseAddrHi: dd  0         ; 内存区域起始地址高 32 位
    _lenLo:      dd  0         ; 内存区域长度低 32 位
    _lenHi:      dd  0         ; 内存区域长度高 32 位
    _memType:    dd  0         ; 区域类型（1=可用 RAM，2=保留等）

_ptCount:      dd  0           ; 覆盖全部内存所需的页表数量
_savedIDTR:    dd  0           ; 保存实模式 IDTR（低 32 位：界限+基址低位）
               dd  0           ; 保存实模式 IDTR（高 32 位：基址高位）
_savedIMR:     db  0           ; 保存 8259A 主片中断屏蔽寄存器（IMR）
_memBuf:       times 256 db 0  ; E820 探测原始数据缓冲区（最多 256/20 ≈ 12 条 ARDS）

; ================================================================
; 调度参数
; TICK_BASE=1：时钟 20Hz（50ms/tick），各任务单次运行时长（ms）：
;   任务 0（VERY）: 16×50 = 800   任务 1（LOVE）: 10×50 = 500
;   任务 2（HUST）:  8×50 = 400   任务 3（MRSU）:  6×50 = 300
; remTicks[i] 表示任务 i 当前剩余可用时间片数，初始等于 taskQuanta[i]
; ================================================================
%define TICK_BASE  1

_curTask:     dd  0                                     ; 当前正在运行的任务编号（0-3）
_taskQuanta:  dd  16*TICK_BASE, 10*TICK_BASE, 8*TICK_BASE, 6*TICK_BASE  ; 各任务初始时间片配额
_remTicks:    dd  0, 0, 0, 0                            ; 各任务剩余时间片（运行时动态更新）

; ---- 段内偏移别名（保护模式下通过 ds 段访问变量时使用 equ 偏移） ----
msgProtMode  equ _msgProtMode  - $$
msgMemHdr    equ _msgMemHdr    - $$
msgRamSize   equ _msgRamSize   - $$
msgCRLF      equ _msgCRLF      - $$
msgReady     equ _msgReady     - $$
dispPos      equ _dispPos      - $$
memSize      equ _memSize      - $$
mcrCount     equ _mcrCount     - $$
ards         equ _ards         - $$
baseAddrLo   equ _baseAddrLo   - $$
baseAddrHi   equ _baseAddrHi   - $$
lenLo        equ _lenLo        - $$
lenHi        equ _lenHi        - $$
memType      equ _memType      - $$
memBuf       equ _memBuf       - $$
savedIDTR    equ _savedIDTR    - $$
savedIMR     equ _savedIMR     - $$
ptCount      equ _ptCount      - $$
curTask      equ _curTask      - $$
taskQuanta   equ _taskQuanta   - $$
remTicks     equ _remTicks     - $$
DataSize     equ $ - DATA_START

; ================================================================
; 内核栈段（ring0，512 字节，四个任务共用）
; CPU 从任意任务的 ring3 陷入 ring0 时，均切换到此栈
; 每次任务切换后由 timerISR 将 esp 重置到 KSTACK_TOP
; ================================================================
[SECTION .gs]
ALIGN 32
[BITS 32]
KSTACK_START:
    times 512 db 0
KSTACK_TOP  equ $ - KSTACK_START - 1  ; 栈顶偏移（相对于段基址）

; ================================================================
; 16 位代码段：COM 程序入口 / 实模式系统初始化
; ================================================================
[SECTION .s16]
[BITS 16]
ENTRY_POINT:
    ; 初始化所有段寄存器为 cs（COM 程序加载时 cs=ds=es=ss）
    mov     ax, cs
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0100h               ; 实模式栈指针（COM 入口地址之下）
    mov     [JUMP_BACK + 3], ax     ; 回填 JUMP_BACK 中的实模式段地址（jmp 0:xxxx）
    mov     [_wRealSP], sp          ; 保存实模式 SP，返回时恢复

    ; ---- 使用 INT 15h E820 功能探测物理内存布局 ----
    xor     ebx, ebx                ; EBX=0：从第一条记录开始探测
    mov     di,  _memBuf            ; ES:DI 指向接收缓冲区
.probe:
    mov     eax, 0E820h             ; INT 15h E820 子功能号
    mov     ecx, 20                 ; 每条 ARDS 20 字节
    mov     edx, 0534D4150h         ; 魔数 'SMAP'（校验标志）
    int     15h
    jc      .probeFail              ; CF=1 表示 BIOS 不支持 E820 或探测结束
    add     di,  20                 ; 移向缓冲区下一条 ARDS 槽位
    inc     dword [_mcrCount]       ; 有效条目数 +1
    test    ebx, ebx                ; EBX=0 表示这是最后一条记录
    jnz     .probe                  ; 未结束则继续探测
    jmp     .probeDone
.probeFail:
    mov     dword [_mcrCount], 0    ; 探测失败，条目数置 0
.probeDone:

    ; ---- 回填各描述符的段基址（保护模式进入前必须完成） ----
    LoadBase  CODE16_START,  GDT_CODE16  ; 16 位过渡代码段
    LoadBase  CODE32_START,  GDT_CODE32  ; 32 位主代码段
    LoadBase  DATA_START,    GDT_DATA    ; 内核数据段
    LoadBase  KSTACK_START,  GDT_STACK   ; 内核栈段

    LoadTaskBase 0  ; 回填任务 0 的 TSS/LDT/Code/Data/Stk0/Stk3 描述符基址
    LoadTaskBase 1
    LoadTaskBase 2
    LoadTaskBase 3

    ; ---- 计算并写入 GDT 物理基址到 GdtPtr ----
    xor     eax, eax
    mov     ax,  ds
    shl     eax, 4                  ; ds << 4 = 段物理基址
    add     eax, GDT_START
    mov     dword [GdtPtr + 2], eax

    ; ---- 计算并写入 IDT 物理基址到 IdtPtr ----
    xor     eax, eax
    mov     ax,  ds
    shl     eax, 4
    add     eax, IDT_START
    mov     dword [IdtPtr + 2], eax

    sidt    [_savedIDTR]            ; 保存实模式 IDTR（返回实模式时恢复）
    in      al,  21h
    mov     [_savedIMR], al         ; 保存 8259A 主片 IMR

    lgdt    [GdtPtr]                ; 加载 GDT 寄存器
    cli                             ; 关中断（切换期间禁止中断）
    lidt    [IdtPtr]                ; 加载 IDT 寄存器

    ; ---- 打开 A20 地址线（通过端口 0x92 快速 A20） ----
    in      al,  92h
    or      al,  02h                ; 置位 bit1 = 开启 A20
    out     92h, al

    ; ---- 切换到保护模式（置位 CR0.PE） ----
    mov     eax, cr0
    or      eax, 1
    mov     cr0, eax
    jmp     dword SelCode32:0       ; 远跳刷新指令队列，进入 32 位保护模式

; ---- 保护模式返回实模式后的入口（由 16 位过渡段跳转而来） ----
REALMODE_ENTRY:
    mov     ax, cs
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, [_wRealSP]          ; 恢复实模式栈指针
    lidt    [_savedIDTR]            ; 恢复实模式 IDTR
    mov     al, [_savedIMR]
    out     21h, al                 ; 恢复 8259A 中断屏蔽状态
    in      al,  92h
    and     al,  0FDh               ; 清除 bit1 = 关闭 A20
    out     92h, al
    sti
    mov     ax,  4C00h
    int     21h                     ; DOS 正常退出

; ================================================================
; 32 位保护模式主代码段
; ================================================================
[SECTION .s32]
[BITS 32]
CODE32_START:
    ; 初始化所有段寄存器
    mov     ax,  SelData
    mov     ds,  ax
    mov     es,  ax
    mov     ax,  SelVideo
    mov     gs,  ax             ; gs 指向 VGA 显存段（后续直接 [gs:xxx] 写屏）
    mov     ax,  SelStack
    mov     ss,  ax
    mov     esp, KSTACK_TOP     ; 建立内核栈

    call    Setup8253A          ; 初始化定时器（20Hz，50ms/tick）
    call    Setup8259A          ; 初始化 8259A（仅开放 IRQ0）
    call    EraseScreen         ; 清屏

    ; 输出进入保护模式提示
    push    msgProtMode
    call    PrintStr
    add     esp, 4

    ; 输出内存信息表头
    push    msgMemHdr
    call    PrintStr
    add     esp, 4

    ; 遍历 ARDS 并显示内存布局，同时计算 memSize
    call    ShowMemInfo

    ; ---- 计算覆盖全部物理内存所需的页表数量 ----
    ; 每张页表管理 1024 页 × 4KB = 4MB，向上取整
    xor     edx, edx
    mov     eax, [memSize]
    mov     ebx, 400000h        ; 4MB
    div     ebx
    test    edx, edx
    jz      .exact
    inc     eax                 ; 余数不为零则多分配一张
.exact:
    mov     [ptCount], eax

    ; ---- 为四个任务各自建立页目录与页表（恒等映射）----
    call    PAGE_INIT0
    call    PAGE_INIT1
    call    PAGE_INIT2
    call    PAGE_INIT3

    ; ---- 初始化 remTicks 数组（等于各任务初始配额）----
    xor     ecx, ecx
.fill:
    mov     eax, [taskQuanta + ecx*4]
    mov     [remTicks + ecx*4], eax
    inc     ecx
    cmp     ecx, 4
    jl      .fill

    sti                         ; 开中断（允许时钟中断触发调度）

    ; ---- 开启分页 ----
    mov     eax, PageDir0
    mov     cr3, eax            ; 加载任务 0 的页目录（初始以任务 0 页表运行）
    mov     ax,  SelTSS0
    ltr     ax                  ; 将 TR 指向任务 0 的 TSS（为 ring3→ring0 切换准备）
    mov     eax, cr0
    or      eax, 80000000h      ; 置位 CR0.PG，开启分页机制
    mov     cr0, eax
    jmp     short $+2           ; 短跳刷新预取指令队列（确保后续指令使用分页地址）

    ; ---- 在第 19 行显示调度参数说明 ----
    xor     ecx, ecx
    mov     ah,  07h            ; 灰色属性
.showMsg:
    mov     al,  [msgReady + ecx]
    test    al,  al
    jz      .msgDone
    mov     [gs : (80*19 + ecx)*2], ax  ; 写入第 19 行 ecx 列
    inc     ecx
    jmp     .showMsg
.msgDone:

    GoTask  0                   ; 通过 iretd 跳入任务 0，开始调度循环

    ; 以下两行在正常运行路径中不会被执行（GoTask 永不返回）
    call    Restore8259A
    jmp     SelCode16:0

; ================================================================
; IRQ0 时钟中断处理程序（优先数调度核心）
;
; 进入条件：已从 ring3 任务代码硬件切换到 ring0（CPU 自动压入中断帧）
; 寄存器保护：使用 pushad/popad 保存 ring0 上下文
;             （注意：任务切换路径会直接重置 esp，不走 popad）
; ================================================================
_timerISR:
timerISR  equ _timerISR - $$   ; 段内偏移别名（IDT 门描述符填写时使用）

    pushad                      ; 保存所有通用寄存器（EAX ECX EDX EBX ESP EBP ESI EDI）
    push    ds                  ; 保存 ds（中断时 ds 仍为任务数据段，需切换到内核数据段）

    mov     ax,  SelData        ; 切换 ds 到内核数据段（以便访问调度变量）
    mov     ds,  ax

    mov     al,  20h
    out     20h, al             ; 向 8259A 主片发送 EOI（End of Interrupt），允许后续中断

    ; ================================================================
    ; 步骤 1-2：递减当前任务时间片，若未归零则直接返回
    ; ================================================================
    mov     edx, [curTask]                  ; edx = 当前任务编号（0-3）
    sub     dword [remTicks + edx*4], 1     ; 剩余时间片 -1
    jnz     .ret                            ; 仍有剩余 → 当前任务继续运行

    ; ================================================================
    ; 步骤 3：当前任务时间片归零，检查是否所有任务均已耗尽
    ; ================================================================
    mov     eax, [remTicks]
    or      eax, [remTicks +  4]
    or      eax, [remTicks +  8]
    or      eax, [remTicks + 12]            ; 四个 remTicks 按位或
    jnz     .pick                           ; 结果非零 → 还有任务有剩余片，直接选调

    ; ================================================================
    ; 步骤 4：所有任务时间片均归零，按优先级重填 remTicks
    ; ================================================================
    xor     ecx, ecx
.refill:
    mov     eax, [taskQuanta + ecx*4]       ; 读取任务 ecx 的初始配额
    mov     [remTicks + ecx*4], eax         ; 重置剩余时间片
    inc     ecx
    cmp     ecx, 4
    jl      .refill

    ; ================================================================
    ; 步骤 5：线性扫描，选出 remTicks 最大的任务（最高优先级）
    ; 变量说明：eax=扫描索引(0-3)，ecx=当前已知最大值，ebx=最优任务号
    ; ================================================================
.pick:
    xor     ebx, ebx            ; 初始候选：任务 0
    xor     ecx, ecx            ; 初始最大值：0（任何正值都会胜出）
    xor     eax, eax
.scan:
    mov     esi, [remTicks + eax*4]     ; 读取任务 eax 的剩余时间片
    cmp     esi, ecx
    jle     .next                       ; 未超过当前最大值，跳过
    mov     ecx, esi                    ; 更新最大值
    mov     ebx, eax                    ; 记录胜出任务号
.next:
    inc     eax
    cmp     eax, 4
    jl      .scan
    ; ebx = remTicks 最大的任务号（即优先级最高的就绪任务）

    ; ================================================================
    ; 步骤 6：更新 curTask，重置内核栈，切换到新任务
    ; 重置内核栈（esp = KSTACK_TOP）而非执行 popad/pop ds：
    ;   这样可丢弃本次 ISR 的 pushad 帧，避免内核栈每次切换泄漏 36 字节。
    ;   任务代码段均从 EIP=0 的无限循环开始，重新进入无任何副作用。
    ; ================================================================
    mov     [curTask], ebx              ; 记录新的当前任务（ds 仍为 SelData）
    mov     esp, KSTACK_TOP             ; 重置内核栈指针，防止栈溢出
    ; GoTask 宏内部会再次设置 ds、cr3、ldtr，因此此处不需要额外处理
    cmp     ebx, 1
    je      .t1
    cmp     ebx, 2
    je      .t2
    cmp     ebx, 3
    je      .t3
    GoTask  0                           ; 切换到任务 0（ebx=0，默认）
.t1: GoTask  1
.t2: GoTask  2
.t3: GoTask  3

    ; ================================================================
    ; 正常返回路径：当前任务时间片未耗尽，恢复寄存器并 iretd 回任务
    ; ================================================================
.ret:
    pop     ds                  ; 恢复任务数据段
    popad                       ; 恢复所有通用寄存器
    iretd                       ; 弹出 EIP/CS/EFLAGS（ring3 任务继续执行）

; ================================================================
; INT 80h 软中断处理程序（演示用，在屏幕左上角输出字符 'I'）
; ================================================================
_swIntISR:
swIntISR  equ _swIntISR - $$
    mov     ah,  0Ch            ; 属性：亮红色
    mov     al,  'I'            ; 字符 'I'（表示软中断触发）
    mov     [gs : (80*0 + 70)*2], ax  ; 写到第 0 行第 70 列
    iretd

; ================================================================
; 伪中断处理程序（所有未使用的中断向量均指向此处）
; 在屏幕左上角输出字符 '!' 以提示有意外中断触发
; ================================================================
_spuriousISR:
spuriousISR  equ _spuriousISR - $$
    mov     ah,  0Ch
    mov     al,  '!'
    mov     [gs : (80*0 + 75)*2], ax  ; 写到第 0 行第 75 列
    iretd

; ---- 调用页目录/页表初始化宏，生成四个任务的页表构建函数 ----
BuildPT 0
BuildPT 1
BuildPT 2
BuildPT 3

%include "utils.inc"            ; 引入工具函数（与本段共享代码段描述符）

Code32Size  equ $ - CODE32_START  ; 用于计算 GDT_CODE32 的 limit 字段

; ================================================================
; 16 位过渡段：关闭分页和保护模式，跳回实模式
; 进入条件：已通过 jmp SelCode16:0 切换到此 16 位段
; ================================================================
[SECTION .s16code]
ALIGN 32
[BITS 16]
CODE16_START:
    ; 将所有段寄存器切换到 GDT_NORMAL（16 位平坦段，可访问全部实模式地址）
    ; 目的：使 CPU 缓存正确的段界限，避免切回实模式后越界
    mov     ax, SelNormal
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax

    ; 同时关闭分页（CR0.PG）和保护模式（CR0.PE）
    mov     eax, cr0
    and     eax, 7ffffffeh      ; 清除 bit31（PG）和 bit0（PE）
    mov     cr0, eax

    ; 远跳到实模式入口（段地址在程序启动时由 ENTRY_POINT 回填）
JUMP_BACK:
    jmp     0:REALMODE_ENTRY

Code16Size  equ $ - CODE16_START
