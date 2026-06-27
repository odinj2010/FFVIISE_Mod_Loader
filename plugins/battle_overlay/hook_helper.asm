.code

extern HookHandler : proc
extern HookHandlerATB : proc
extern g_OriginalInBattlePtr : qword
extern g_OriginalEnemyNoATB : qword

HookEntry proc
    ; Save all registers
    pushfq
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    push rbp
    push rdi
    push rsi

    ; Pass RDX (first arg to HookHandler -> RCX) and RAX (second arg -> RDX)
    mov rcx, rdx
    mov rdx, rax
    sub rsp, 28h    ; Allocate shadow space (32 bytes) + 8 bytes alignment
    call HookHandler
    add rsp, 28h

    ; Restore all registers
    pop rsi
    pop rdi
    pop rbp
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    popfq

    ; Jump to MinHook's trampoline
    jmp qword ptr [g_OriginalInBattlePtr]
HookEntry endp

HookEntryATB proc
    ; Save all registers
    pushfq
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    push rbp
    push rdi
    push rsi

    ; Pass RAX (which is the ATB pointer) in RCX
    mov rcx, rax
    sub rsp, 28h    ; Allocate shadow space (32 bytes) + 8 bytes alignment
    call HookHandlerATB
    add rsp, 28h

    ; Restore all registers
    pop rsi
    pop rdi
    pop rbp
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    popfq

    ; Jump to MinHook's trampoline
    jmp qword ptr [g_OriginalEnemyNoATB]
HookEntryATB endp

end
