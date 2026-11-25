.text
.globl _intelPrivacyNativeSleepContinue
_intelPrivacyNativeSleepContinue:
    subq $8, %rsp
    callq _testSleepBody
    addq $8, %rsp
    popq %r14
    leave
    retq
.globl _intelPrivacyNativeWakeContinue
_intelPrivacyNativeWakeContinue:
    subq $8, %rsp
    callq _testWakeBody
    addq $8, %rsp
    popq %r14
    leave
    retq
.globl _intelPrivacyNativeStartContinue
_intelPrivacyNativeStartContinue:
    subq $8, %rsp
    callq _testStartBody
    addq $8, %rsp
    popq %r15
    leave
    retq
.globl _intelPrivacyNativeStopContinue
_intelPrivacyNativeStopContinue:
    subq $8, %rsp
    callq _testStopBody
    addq $8, %rsp
    popq %r15
    leave
    retq
.globl _intelPrivacyNativeRemoveContinue
_intelPrivacyNativeRemoveContinue:
    movl %ecx, %esi
    jmp _testRemoveBody
.globl _intelPrivacyNativeClearContinue
_intelPrivacyNativeClearContinue:
    jmp _testClearBody
.globl _intelPrivacyNativeReset
_intelPrivacyNativeReset:
    movzbl %al, %edx
    jmp _testResetBody

.section __TEXT,__const
.globl _intelPrivacyHookMarkers
_intelPrivacyHookMarkers:
    .quad _testMarker - _intelPrivacyHookMarkers
    .quad 0x78563412
    .byte 4
    .quad _testMarker + 4 - _intelPrivacyHookMarkers
    .quad 0x44332211
    .byte 4
.globl _intelPrivacyHookMarkerCount
_intelPrivacyHookMarkerCount:
    .long 2
.data
.globl _testMarker
_testMarker:
    .byte 0x12, 0x34, 0x56, 0x78, 0x11, 0x22, 0x33, 0x44
