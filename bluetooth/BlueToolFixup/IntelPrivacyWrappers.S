.text

.globl _intelPrivacyRetry
_intelPrivacyRetry:
    pushq %rdi
    callq _intelPrivacyRefreshRetry
    popq %rdi
    testl %eax, %eax
    jz LretryStopped
    jmp _intelPrivacyNativeRetry
LretryStopped:
    retq

.globl _intelPrivacySleep
_intelPrivacySleep:
    pushq %rdi
    xorl %edi, %edi
    callq _intelPrivacySetEnabled
    popq %rdi
    jmp _intelPrivacySleepOriginal

.globl _intelPrivacyStop
_intelPrivacyStop:
    pushq %rdi
    xorl %edi, %edi
    callq _intelPrivacySetEnabled
    popq %rdi
    jmp _intelPrivacyStopOriginal

.globl _intelPrivacyWake
_intelPrivacyWake:
    pushq %rdi
    callq _intelPrivacyWakeOriginal
    movl $1, %edi
    callq _intelPrivacySetEnabled
    popq %rdi
    retq

.globl _intelPrivacyStart
_intelPrivacyStart:
    pushq %rdi
    callq _intelPrivacyStartOriginal
    movl $1, %edi
    callq _intelPrivacySetEnabled
    popq %rdi
    retq

.globl _intelPrivacyRemove
_intelPrivacyRemove:
    pushq %rdi
    callq _intelPrivacyInvalidate
    popq %rdi
    jmp _intelPrivacyRemoveOriginal

.globl _intelPrivacyClear
_intelPrivacyClear:
    subq $8, %rsp
    callq _intelPrivacyInvalidate
    addq $8, %rsp
    jmp _intelPrivacyClearOriginal

.globl _intelPrivacyReset
_intelPrivacyReset:
    pushq %rdi
    pushq %rsi
    subq $8, %rsp
    callq _intelPrivacyInvalidate
    addq $8, %rsp
    popq %rsi
    popq %rdi
    xorl %eax, %eax
    jmp _intelPrivacyNativeReset

// The four observer entries begin with these complete, position-independent
// instructions. The two host-cache entries instead load RIP-relative globals.
.globl _intelPrivacySleepOriginal
_intelPrivacySleepOriginal:
    pushq %rbp
    movq %rsp, %rbp
    pushq %r14
    jmp _intelPrivacyNativeSleepContinue
.globl _intelPrivacyWakeOriginal
_intelPrivacyWakeOriginal:
    pushq %rbp
    movq %rsp, %rbp
    pushq %r14
    jmp _intelPrivacyNativeWakeContinue
.globl _intelPrivacyStartOriginal
_intelPrivacyStartOriginal:
    pushq %rbp
    movq %rsp, %rbp
    pushq %r15
    jmp _intelPrivacyNativeStartContinue
.globl _intelPrivacyStopOriginal
_intelPrivacyStopOriginal:
    pushq %rbp
    movq %rsp, %rbp
    pushq %r15
    jmp _intelPrivacyNativeStopContinue
.globl _intelPrivacyRemoveOriginal
_intelPrivacyRemoveOriginal:
    movzwl _intelPrivacyNativeIRKCount(%rip), %ecx
    jmp _intelPrivacyNativeRemoveContinue
.globl _intelPrivacyClearOriginal
_intelPrivacyClearOriginal:
    movq _intelPrivacyNativeIRKTable(%rip), %rdi
    jmp _intelPrivacyNativeClearContinue

.globl _intelPrivacyInstalledControllerGate
_intelPrivacyInstalledControllerGate:
    leaq _intelPrivacyHookMarkers(%rip), %r8
    movl _intelPrivacyHookMarkerCount(%rip), %r9d
LnextMarker:
    testl %r9d, %r9d
    jz Lverified
    movq (%r8), %rdx
    leaq _intelPrivacyHookMarkers(%rip), %rax
    addq %rax, %rdx
    movzbl 16(%r8), %ecx
    testl %ecx, %ecx
    jz LnotInstalled
    movq 8(%r8), %rax
LnextByte:
    cmpb %al, (%rdx)
    jne LnotInstalled
    shrq $8, %rax
    incq %rdx
    decl %ecx
    jnz LnextByte
    addq $17, %r8
    decl %r9d
    jmp LnextMarker
Lverified:
    jmp _intelPrivacyActiveController
LnotInstalled:
    xorl %eax, %eax
    retq
