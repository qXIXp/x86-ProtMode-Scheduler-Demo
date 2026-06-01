; utils.inc — 保护模式工具函数集
; 包含：8259A/8253A 初始化、屏幕输出、内存拷贝、内存信息显示
; 在 32 位代码段末尾通过 %include 引入，与主代码共享同一代码段描述符

; ================================================================
; IoWait — 通过连续 nop 制造短暂延迟
; 用于 I/O 端口操作之间的等待，防止过快写入导致芯片未响应
; ================================================================
IoWait:
    nop
    nop
    nop
    nop
    ret

; ================================================================
; Setup8259A — 初始化 8259A 可编程中断控制器（PIC）
; 采用主从级联模式：主片管理 IRQ0-IRQ7，从片管理 IRQ8-IRQ15
; 初始化后仅开放主片 IRQ0（8253 定时器），屏蔽其余所有中断线
; ================================================================
Setup8259A:
    ; ICW1：初始化命令字 1，发送给主/从片命令端口
    ; 0x11 = 边沿触发、级联模式、需要 ICW4
    mov     al, 011h
    out     020h, al            ; 写主片命令端口（0x20）
    call    IoWait
    out     0A0h, al            ; 写从片命令端口（0xA0）
    call    IoWait

    ; ICW2：中断向量偏移，发送给主/从片数据端口
    mov     al, 020h            ; 主片 IRQ0 映射到向量 0x20（避免与 CPU 异常 0x00-0x1F 冲突）
    out     021h, al
    call    IoWait
    mov     al, 028h            ; 从片 IRQ8 映射到向量 0x28
    out     0A1h, al
    call    IoWait

    ; ICW3：级联连接字
    mov     al, 004h            ; 主片：IR2 引脚连接从片（bit2=1）
    out     021h, al
    call    IoWait
    mov     al, 002h            ; 从片：从片 ID=2（与主片 IR2 对应）
    out     0A1h, al
    call    IoWait

    ; ICW4：工作模式字
    mov     al, 001h            ; 8086 模式，非自动 EOI
    out     021h, al
    call    IoWait
    out     0A1h, al            ; 从片同样设置为 8086 模式
    call    IoWait

    ; OCW1：中断屏蔽字，写入数据端口
    mov     al, 0FEh            ; 主片：0b11111110，仅开放 IRQ0（定时器），屏蔽其余
    out     021h, al
    call    IoWait
    mov     al, 0FFh            ; 从片：屏蔽全部中断（IRQ8-IRQ15 均不使用）
    out     0A1h, al
    call    IoWait
    ret

; ================================================================
; Restore8259A — 返回实模式前将 8259A 复位到 DOS 默认配置
; DOS 使用向量偏移 0x08，中断屏蔽状态从 savedIMR 恢复
; ================================================================
Restore8259A:
    mov     ax, SelData         ; 通过 fs 段访问数据段中保存的 IMR
    mov     fs, ax
    mov     al, 017h            ; 主片 ICW1（DOS 默认配置）
    out     020h, al
    call    IoWait
    mov     al, 008h            ; 主片 ICW2：IRQ0 映射到向量 0x08（DOS 默认）
    out     021h, al
    call    IoWait
    mov     al, 001h            ; 主片 ICW4：8086 模式
    out     021h, al
    call    IoWait
    mov     al, [fs:savedIMR]   ; 恢复进入保护模式前保存的中断屏蔽寄存器值
    out     021h, al
    call    IoWait
    ret

; ================================================================
; Setup8253A — 初始化 8253 定时器通道 0，产生 20Hz（50ms）方波
; 8253 基准时钟 1193180Hz，计数初值 = 1193180 / 20 ≈ 59659
; 通道 0 输出直接连接主片 IRQ0，每 50ms 触发一次时钟中断
; ================================================================
Setup8253A:
    ; 控制字 0x36 = 0b00110110：
    ;   bit7-6=00：通道 0
    ;   bit5-4=11：先写低字节后写高字节
    ;   bit3-1=011：方波发生器（Mode 3）
    ;   bit0=0：二进制计数
    mov     al, 036h
    out     043h, al            ; 写控制端口（0x43）
    call    IoWait
    mov     ax, 59659           ; 计数初值 59659（低 8 位先写）
    out     040h, al            ; 写通道 0 数据端口低字节
    call    IoWait
    mov     al, ah
    out     040h, al            ; 写通道 0 数据端口高字节
    call    IoWait
    ret

; ================================================================
; EraseScreen — 向 VGA 文本显存写零，清除整个屏幕（80×25=2000 字符）
; gs 段已指向显存基地址 0xB8000
; 每个字符单元 2 字节（字符码+属性），共 4000 字节 / 2000 个字
; ================================================================
EraseScreen:
    push    ecx
    push    ebx
    push    eax
    xor     ebx, ebx            ; ebx = 显存偏移，从 0 开始
    xor     eax, eax            ; ax = 0x0000：空字符 + 黑底黑字属性
    mov     ecx, 4000           ; 写 4000 次，每次写 2 字节 = 8000 字节 = 全屏
.clr:
    mov     [gs:ebx], ax        ; 写一个字符单元
    add     ebx, 2              ; 移动到下一个字符单元
    loop    .clr
    pop     eax
    pop     ebx
    pop     ecx
    ret

; ================================================================
; PrintByte — 将 AL 寄存器的值以 2 位十六进制形式输出到屏幕
; 先输出高 4 位，再输出低 4 位；输出后更新 dispPos（下一字符位置）
; ================================================================
PrintByte:
    push    ecx
    push    edx
    push    edi
    mov     edi, [dispPos]      ; 取当前输出位置（显存字节偏移）
    mov     dl,  al             ; dl 备份原始值，供第二轮取低 4 位使用
    mov     ah,  0Fh            ; 颜色属性：黑底白字
    shr     al,  4              ; 先处理高 4 位
    mov     ecx, 2              ; 共输出 2 个十六进制字符
.nb:
    and     al, 0Fh             ; 取低 4 位（第一轮=原高4位，第二轮=原低4位）
    cmp     al, 9
    ja      .alpha              ; > 9 则使用字母 A-F
    add     al, '0'             ; 0-9 → '0'-'9'
    jmp     .put
.alpha:
    sub     al, 0Ah
    add     al, 'A'             ; 10-15 → 'A'-'F'
.put:
    mov     [gs:edi], ax        ; 写字符+属性到显存
    add     edi, 2              ; 移到下一个字符位置
    mov     al,  dl             ; 第二轮：恢复原值取低 4 位
    loop    .nb
    mov     [dispPos], edi      ; 更新显示位置
    pop     edi
    pop     edx
    pop     ecx
    ret

; ================================================================
; PrintDword — 将栈上 32 位参数以 8 位十六进制输出，末尾追加 'h'
; 调用约定：push dword value ; call PrintDword ; add esp, 4
; ================================================================
PrintDword:
    mov     eax, [esp + 4]      ; 取参数
    shr     eax, 24             ; 输出最高字节（bits 31-24）
    call    PrintByte
    mov     eax, [esp + 4]
    shr     eax, 16             ; 输出次高字节（bits 23-16）
    call    PrintByte
    mov     eax, [esp + 4]
    shr     eax, 8              ; 输出次低字节（bits 15-8）
    call    PrintByte
    mov     eax, [esp + 4]      ; 输出最低字节（bits 7-0）
    call    PrintByte
    push    edi
    mov     edi, [dispPos]
    mov     ah,  07h            ; 灰色属性
    mov     al,  'h'            ; 输出后缀 'h' 表示十六进制
    mov     [gs:edi], ax
    add     edi, 4              ; 跳过 2 字节字符单元 + 2 字节空格
    mov     [dispPos], edi
    pop     edi
    ret

; ================================================================
; PrintStr — 输出栈上字符串指针所指的字符串到屏幕
; 0Ah（换行符）：计算并跳转到下一行起始位置
; 0x00（NUL）  ：字符串结束，返回
; 调用约定：push offset_of_string ; call PrintStr ; add esp, 4
; ================================================================
PrintStr:
    push    ebp
    mov     ebp, esp
    push    esi
    push    edi
    push    ebx
    mov     esi, [ebp + 8]      ; 取字符串段内偏移（ds 段）
    mov     edi, [dispPos]      ; 取当前显示位置（显存偏移）
    mov     ah,  0Fh            ; 默认颜色：黑底白字
.gc:
    lodsb                       ; al = [ds:esi]，esi 自动 +1
    test    al, al
    jz      .done               ; NUL：字符串结束
    cmp     al, 0Ah
    jne     .pc                 ; 非换行符，直接输出
    ; 处理换行：将 edi 对齐到下一行起始
    push    eax
    mov     eax, edi            ; 当前显存偏移
    mov     bl,  160            ; 每行 160 字节（80 列 × 2 字节）
    div     bl                  ; al = 当前行号，ah = 行内偏移
    and     eax, 0FFh           ; 取行号
    inc     eax                 ; 行号 +1（下一行）
    mul     bl                  ; 下一行首字节偏移
    mov     edi, eax
    pop     eax
    jmp     .gc
.pc:
    mov     [gs:edi], ax        ; 输出字符（al=字符，ah=属性）
    add     edi, 2
    jmp     .gc
.done:
    mov     [dispPos], edi      ; 更新光标位置
    pop     ebx
    pop     edi
    pop     esi
    pop     ebp
    ret

; ================================================================
; PrintNewLine — 输出换行符（通过 PrintStr 实现）
; ================================================================
PrintNewLine:
    push    msgCRLF             ; msgCRLF 为数据段中 0Ah, 0 的偏移
    call    PrintStr
    add     esp, 4
    ret

; ================================================================
; CopyMem — 字节级内存拷贝
; 原型：void CopyMem(void* es:dst, void* ds:src, int size)
; 注意：目标地址使用 es 段，源地址使用 ds 段
; ================================================================
CopyMem:
    push    ebp
    mov     ebp, esp
    push    ecx
    push    esi
    push    edi
    mov     edi, [ebp + 8]      ; 目标地址（es 段偏移）
    mov     esi, [ebp + 12]     ; 源地址（ds 段偏移）
    mov     ecx, [ebp + 16]     ; 拷贝字节数
.cp:
    test    ecx, ecx
    jz      .done
    mov     al,  [ds:esi]       ; 从源读一字节
    mov     [es:edi], al        ; 写入目标
    inc     esi
    inc     edi
    dec     ecx
    jmp     .cp
.done:
    mov     eax, [ebp + 8]      ; 返回目标地址（C 约定）
    pop     edi
    pop     esi
    pop     ecx
    mov     esp, ebp
    pop     ebp
    ret

; ================================================================
; ShowMemInfo — 遍历 E820 ARDS（地址范围描述符）数组
; 逐条打印内存区域信息（基址低/高、长度低/高、类型），
; 并统计所有类型为 1（可用内存）区域的最大上界，写入 memSize
; ================================================================
ShowMemInfo:
    push    ecx
    push    edi
    push    esi
    mov     esi, memBuf         ; esi 指向 E820 原始数据缓冲区起始
    mov     ecx, [mcrCount]     ; ecx = ARDS 条目总数
.row:
    ; 每条 ARDS 含 5 个 DWORD 字段（20 字节）
    mov     edx, 5              ; edx = 字段计数器
    mov     edi, ards           ; edi 指向结构体解析缓冲区
.col:
    push    dword [esi]         ; 压栈当前字段值，传给 PrintDword
    call    PrintDword
    pop     eax
    stosd                       ; 将字段值写入 ards 结构（es:edi 自动 +4）
    add     esi, 4              ; 移向原始缓冲区下一字段
    dec     edx
    jnz     .col
    call    PrintNewLine        ; 每条 ARDS 输出完毕换行

    ; 只统计类型=1（可用 RAM）的区域
    cmp     dword [memType], 1
    jne     .skip
    mov     eax, [baseAddrLo]   ; 区域起始地址（低 32 位）
    add     eax, [lenLo]        ; + 区域长度（低 32 位）= 区域末尾地址
    cmp     eax, [memSize]
    jb      .skip               ; 若未超过当前记录的最大值则跳过
    mov     [memSize], eax      ; 更新最大可用内存上界
.skip:
    loop    .row                ; 处理下一条 ARDS

    ; 输出统计到的 RAM 总大小
    call    PrintNewLine
    push    msgRamSize
    call    PrintStr
    add     esp, 4
    push    dword [memSize]
    call    PrintDword
    add     esp, 4
    pop     esi
    pop     edi
    pop     ecx
    ret
