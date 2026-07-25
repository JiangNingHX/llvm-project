; Test each TLS size option
; RUN: llc -mtriple=arm64-none-linux-gnu -verify-machineinstrs -show-mc-encoding -tls-size=12 < %s | FileCheck %s --check-prefix=CHECK-12
; RUN: llc -mtriple=arm64-none-linux-gnu -filetype=obj < %s -tls-size=12 | llvm-objdump -r - | FileCheck --check-prefix=CHECK-12-RELOC %s
; RUN: llc -mtriple=arm64-none-linux-gnu -verify-machineinstrs -show-mc-encoding -code-model=tiny -tls-size=24 < %s | FileCheck %s --check-prefix=CHECK-24
; RUN: llc -mtriple=arm64-none-linux-gnu -filetype=obj < %s -code-model=tiny -tls-size=24 | llvm-objdump -r - | FileCheck --check-prefix=CHECK-24-RELOC %s
; RUN: llc -mtriple=arm64-none-linux-gnu -verify-machineinstrs -show-mc-encoding -code-model=small -tls-size=32 < %s | FileCheck %s --check-prefix=CHECK-32
; RUN: llc -mtriple=arm64-none-linux-gnu -filetype=obj < %s -code-model=small -tls-size=32 | llvm-objdump -r - | FileCheck --check-prefix=CHECK-32-RELOC %s
; RUN: llc -mtriple=arm64-none-linux-gnu -verify-machineinstrs -show-mc-encoding -code-model=large -tls-size=48 < %s | FileCheck %s --check-prefix=CHECK-48
; RUN: llc -mtriple=arm64-none-linux-gnu -filetype=obj < %s -code-model=large -tls-size=48 | llvm-objdump -r - | FileCheck --check-prefix=CHECK-48-RELOC %s
;
; Test the maximum TLS size for each code model (fallback to a smaller size from the specified size)
; RUN: llc -mtriple=arm64-none-linux-gnu -verify-machineinstrs -show-mc-encoding -tls-size=32 < %s | FileCheck %s --check-prefix=CHECK-32
; RUN: llc -mtriple=arm64-none-linux-gnu -filetype=obj < %s -tls-size=32 | llvm-objdump -r - | FileCheck --check-prefix=CHECK-32-RELOC %s
; RUN: llc -mtriple=arm64-none-linux-gnu -verify-machineinstrs -show-mc-encoding -code-model=tiny -tls-size=32 < %s | FileCheck %s --check-prefix=CHECK-24
; RUN: llc -mtriple=arm64-none-linux-gnu -filetype=obj < %s -code-model=tiny -tls-size=32 | llvm-objdump -r - | FileCheck --check-prefix=CHECK-24-RELOC %s
; RUN: llc -mtriple=arm64-none-linux-gnu -verify-machineinstrs -show-mc-encoding -code-model=small -tls-size=48 < %s | FileCheck %s --check-prefix=CHECK-32
; RUN: llc -mtriple=arm64-none-linux-gnu -filetype=obj < %s -code-model=small -tls-size=48 | llvm-objdump -r - | FileCheck --check-prefix=CHECK-32-RELOC %s
;
; Test the default TLS size for each code model
; RUN: llc -mtriple=arm64-none-linux-gnu -verify-machineinstrs -show-mc-encoding < %s | FileCheck --check-prefix=CHECK-24 %s
; RUN: llc -mtriple=arm64-none-linux-gnu -filetype=obj < %s | llvm-objdump -r - | FileCheck --check-prefix=CHECK-24-RELOC %s
; RUN: llc -mtriple=arm64-none-linux-gnu -verify-machineinstrs -show-mc-encoding -code-model=tiny < %s | FileCheck %s --check-prefix=CHECK-24
; RUN: llc -mtriple=arm64-none-linux-gnu -filetype=obj < %s -code-model=tiny | llvm-objdump -r - | FileCheck --check-prefix=CHECK-24-RELOC %s
; RUN: llc -mtriple=arm64-none-linux-gnu -verify-machineinstrs -show-mc-encoding -code-model=small < %s | FileCheck %s --check-prefix=CHECK-24
; RUN: llc -mtriple=arm64-none-linux-gnu -filetype=obj < %s -code-model=small | llvm-objdump -r - | FileCheck --check-prefix=CHECK-24-RELOC %s
; RUN: llc -mtriple=arm64-none-linux-gnu -verify-machineinstrs -show-mc-encoding -code-model=large < %s | FileCheck %s --check-prefix=CHECK-24
; RUN: llc -mtriple=arm64-none-linux-gnu -filetype=obj < %s -code-model=large | llvm-objdump -r - | FileCheck --check-prefix=CHECK-24-RELOC %s

@local_exec_var = thread_local(localexec) global i32 0
@aligned_local_exec_var = thread_local(localexec) global [32 x i8] zeroinitializer, align 32
@under_aligned_local_exec_var = thread_local(localexec) global [24 x i8] zeroinitializer, align 8
@page_aligned_local_exec_var = thread_local(localexec) global [4104 x i8] zeroinitializer, align 4096

define i32 @test_local_exec() {
; CHECK-LABEL: test_local_exec:
  %val = load i32, ptr @local_exec_var

; CHECK-12: mrs x[[R1:[0-9]+]], TPIDR_EL0
; CHECK-12: add x[[R2:[0-9]+]], x[[R1]], :tprel_lo12:local_exec_var
; CHECK-12: ldr w0, [x[[R2]]]

; CHECK-12-RELOC: R_AARCH64_TLSLE_ADD_TPREL_LO12

; CHECK-24: mrs x[[R1:[0-9]+]], TPIDR_EL0
; CHECK-24: add x[[R2:[0-9]+]], x[[R1]], :tprel_hi12:local_exec_var
; CHECK-24: ldr w0, [x[[R2]], :tprel_lo12_nc:local_exec_var]

; CHECK-24-RELOC: R_AARCH64_TLSLE_ADD_TPREL_HI12
; CHECK-24-RELOC: R_AARCH64_TLSLE_LDST32_TPREL_LO12_NC

; CHECK-32: movz x[[R2:[0-9]+]], #:tprel_g1:local_exec_var
; CHECK-32: mrs x[[R1:[0-9]+]], TPIDR_EL0
; CHECK-32: movk x[[R2]], #:tprel_g0_nc:local_exec_var
; CHECK-32: ldr w0, [x[[R1]], x[[R2]]]

; CHECK-32-RELOC: R_AARCH64_TLSLE_MOVW_TPREL_G1
; CHECK-32-RELOC: R_AARCH64_TLSLE_MOVW_TPREL_G0_NC

; CHECK-48: movz x[[R2:[0-9]+]], #:tprel_g2:local_exec_var
; CHECK-48: mrs x[[R1:[0-9]+]], TPIDR_EL0
; CHECK-48: movk x[[R2]], #:tprel_g1_nc:local_exec_var
; CHECK-48: movk x[[R2]], #:tprel_g0_nc:local_exec_var
; CHECK-48: ldr w0, [x[[R1]], x[[R2]]]

; CHECK-48-RELOC: R_AARCH64_TLSLE_MOVW_TPREL_G2
; CHECK-48-RELOC: R_AARCH64_TLSLE_MOVW_TPREL_G1_NC
; CHECK-48-RELOC: R_AARCH64_TLSLE_MOVW_TPREL_G0_NC
  ret i32 %val
}

define ptr @test_local_exec_addr() {
; CHECK-LABEL: test_local_exec_addr:
  ret ptr @local_exec_var

; CHECK-12: mrs x[[R1:[0-9]+]], TPIDR_EL0
; CHECK-12: add x0, x[[R1]], :tprel_lo12:local_exec_var
; CHECK-12: ret

; CHECK-12-RELOC: R_AARCH64_TLSLE_ADD_TPREL_LO12

; CHECK-24: mrs x[[R1:[0-9]+]], TPIDR_EL0
; CHECK-24: add x[[R2:[0-9]+]], x[[R1]], :tprel_hi12:local_exec_var
; CHECK-24: add x0, x[[R2]], :tprel_lo12_nc:local_exec_var
; CHECK-24: ret

; CHECK-24-RELOC: R_AARCH64_TLSLE_ADD_TPREL_HI12
; CHECK-24-RELOC: R_AARCH64_TLSLE_ADD_TPREL_LO12_NC

; CHECK-32: movz x[[R2:[0-9]+]], #:tprel_g1:local_exec_var
; CHECK-32: mrs x[[R1:[0-9]+]], TPIDR_EL0
; CHECK-32: movk x[[R2]], #:tprel_g0_nc:local_exec_var
; CHECK-32: add x0, x[[R1]], x[[R2]]
; CHECK-32: ret

; CHECK-32-RELOC: R_AARCH64_TLSLE_MOVW_TPREL_G1
; CHECK-32-RELOC: R_AARCH64_TLSLE_MOVW_TPREL_G0_NC

; CHECK-48: movz x[[R2:[0-9]+]], #:tprel_g2:local_exec_var
; CHECK-48: mrs x[[R1:[0-9]+]], TPIDR_EL0
; CHECK-48: movk x[[R2]], #:tprel_g1_nc:local_exec_var
; CHECK-48: movk x[[R2]], #:tprel_g0_nc:local_exec_var
; CHECK-48: add x0, x[[R1]], x[[R2]]
; CHECK-48: ret

; CHECK-48-RELOC: R_AARCH64_TLSLE_MOVW_TPREL_G2
; CHECK-48-RELOC: R_AARCH64_TLSLE_MOVW_TPREL_G1_NC
; CHECK-48-RELOC: R_AARCH64_TLSLE_MOVW_TPREL_G0_NC
}

define i64 @test_local_exec_fields(i64 %expected) {
; CHECK-24-LABEL: test_local_exec_fields:
; CHECK-24: mrs x[[R1:[0-9]+]], TPIDR_EL0
; CHECK-24: add x[[R2:[0-9]+]], x[[R1]], :tprel_hi12:aligned_local_exec_var
; CHECK-24-NOT: add {{.*}}:tprel_lo12_nc:aligned_local_exec_var
; CHECK-24: ldr x[[R3:[0-9]+]], [x[[R2]], :tprel_lo12_nc:aligned_local_exec_var+8]
; CHECK-24: cmp x[[R3]], x0
; CHECK-24: b.ne
; CHECK-24-NOT: add {{.*}}:tprel_lo12_nc:aligned_local_exec_var
; CHECK-24: ldr x0, [x[[R2]], :tprel_lo12_nc:aligned_local_exec_var+16]

; CHECK-24-RELOC: R_AARCH64_TLSLE_ADD_TPREL_HI12 aligned_local_exec_var
; CHECK-24-RELOC: R_AARCH64_TLSLE_LDST64_TPREL_LO12_NC aligned_local_exec_var+0x8
; CHECK-24-RELOC: R_AARCH64_TLSLE_LDST64_TPREL_LO12_NC aligned_local_exec_var+0x10
entry:
  %base = call ptr @llvm.threadlocal.address.p0(ptr @aligned_local_exec_var)
  %field8 = getelementptr inbounds i8, ptr %base, i64 8
  %value8 = load i64, ptr %field8, align 8
  %matches = icmp eq i64 %value8, %expected
  br i1 %matches, label %hit, label %miss

hit:
  %field16 = getelementptr inbounds i8, ptr %base, i64 16
  %value16 = load volatile i64, ptr %field16, align 8
  ret i64 %value16

miss:
  ret i64 0
}

define i64 @test_local_exec_insufficient_alignment() {
; CHECK-24-LABEL: test_local_exec_insufficient_alignment:
; CHECK-24: mrs x[[R1:[0-9]+]], TPIDR_EL0
; CHECK-24: add x[[R2:[0-9]+]], x[[R1]], :tprel_hi12:under_aligned_local_exec_var
; CHECK-24: add x[[R3:[0-9]+]], x[[R2]], :tprel_lo12_nc:under_aligned_local_exec_var
; CHECK-24: ldr x0, [x[[R3]], #8]
  %base = call ptr @llvm.threadlocal.address.p0(ptr @under_aligned_local_exec_var)
  %field8 = getelementptr inbounds i8, ptr %base, i64 8
  %value8 = load i64, ptr %field8, align 8
  ret i64 %value8
}

define i64 @test_local_exec_page_crossing() {
; CHECK-24-LABEL: test_local_exec_page_crossing:
; CHECK-24: mrs x[[R1:[0-9]+]], TPIDR_EL0
; CHECK-24: add x[[R2:[0-9]+]], x[[R1]], :tprel_hi12:page_aligned_local_exec_var
; CHECK-24: add x[[R3:[0-9]+]], x[[R2]], :tprel_lo12_nc:page_aligned_local_exec_var
; CHECK-24: ldr x0, [x[[R3]], #4096]
  %base = call ptr @llvm.threadlocal.address.p0(ptr @page_aligned_local_exec_var)
  %field4096 = getelementptr inbounds i8, ptr %base, i64 4096
  %value4096 = load i64, ptr %field4096, align 8
  ret i64 %value4096
}

declare ptr @llvm.threadlocal.address.p0(ptr)
