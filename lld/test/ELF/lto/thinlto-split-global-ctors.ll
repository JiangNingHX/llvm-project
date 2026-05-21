; REQUIRES: aarch64
;
; This is a regression test for ThinLTO split backend partitioning.
;
; Before the fix, llvm.global_ctors stayed in the owner partition, but ctor
; bodies referenced by that appending array could be assigned to another split
; partition. The owner precodegen module then reached codegen with
; declaration-only ctor entries, and the final ELF emitted a smaller
; .init_array.
;
; This test builds two independent roots so split=2 actually partitions the
; module, then checks both:
; 1. the owner precodegen partition still contains definitions for every ctor
;    named by llvm.global_ctors
; 2. the final ELF keeps all 7 init-array entries
;
; RUN: opt --thinlto-bc --thinlto-split-lto-unit -o %t.o %s
; RUN: ld.lld %t.o -shared -o %t.so -save-temps \
; RUN:   -mllvm -thinlto-split=true \
; RUN:   -mllvm -thinlto-split-partitions=2 \
; RUN:   -mllvm -thinlto-split-module-size-threshold=0 \
; RUN:   -mllvm -thinlto-split-threshold=0 \
; RUN:   -mllvm -thinlto-split-module-size-rate-threshold=2.0 \
; RUN:   -mllvm -parallel-cloneModule=false
; RUN: for f in %t.so.*.5.precodegen.bc; do \
; RUN:   if llvm-dis -o - "$f" | grep -q '@llvm.global_ctors'; then \
; RUN:     llvm-dis -o - "$f"; \
; RUN:   fi; \
; RUN: done | FileCheck %s --check-prefix=OWNER
; RUN: llvm-readelf -SW %t.so | FileCheck %s --check-prefix=FINAL

; OWNER: @llvm.global_ctors = appending global [7 x { i32, ptr, ptr }]
; OWNER-DAG: define hidden void @ctor1.{{.*}}(){{.*}}section ".text.startup"
; OWNER-DAG: define hidden void @ctor2.{{.*}}(){{.*}}section ".text.startup"
; OWNER-DAG: define hidden void @ctor3.{{.*}}(){{.*}}section ".text.startup"
; OWNER-DAG: define hidden void @ctor4.{{.*}}(){{.*}}section ".text.startup"
; OWNER-DAG: define hidden void @ctor5.{{.*}}(){{.*}}section ".text.startup"
; OWNER-DAG: define hidden void @ctor6.{{.*}}(){{.*}}section ".text.startup"
; OWNER-DAG: define hidden void @_GLOBAL__sub_I_mod.{{.*}}(){{.*}}section ".text.startup"

; FINAL: .init_array{{.*}}000038{{.*}}

target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-unknown-linux-gnu"

$g1 = comdat any
$g2 = comdat any
$g3 = comdat any
$g4 = comdat any
$g5 = comdat any
$g6 = comdat any

@g1 = weak_odr hidden global i32 0, comdat, align 4
@g2 = weak_odr hidden global i32 0, comdat, align 4
@g3 = weak_odr hidden global i32 0, comdat, align 4
@g4 = weak_odr hidden global i32 0, comdat, align 4
@g5 = weak_odr hidden global i32 0, comdat, align 4
@g6 = weak_odr hidden global i32 0, comdat, align 4

@llvm.used = appending global [6 x ptr] [
  ptr @g1, ptr @g2, ptr @g3, ptr @g4, ptr @g5, ptr @g6
], section "llvm.metadata"

@llvm.global_ctors = appending global [7 x { i32, ptr, ptr }] [
  { i32, ptr, ptr } { i32 65535, ptr @ctor1, ptr @g1 },
  { i32, ptr, ptr } { i32 65535, ptr @ctor2, ptr @g2 },
  { i32, ptr, ptr } { i32 65535, ptr @ctor3, ptr @g3 },
  { i32, ptr, ptr } { i32 65535, ptr @ctor4, ptr @g4 },
  { i32, ptr, ptr } { i32 65535, ptr @ctor5, ptr @g5 },
  { i32, ptr, ptr } { i32 65535, ptr @ctor6, ptr @g6 },
  { i32, ptr, ptr } { i32 65535, ptr @_GLOBAL__sub_I_mod, ptr null }
]

define internal void @ctor1() section ".text.startup" comdat($g1) {
  store volatile i32 1, ptr @g1, align 4
  ret void
}

define internal void @ctor2() section ".text.startup" comdat($g2) {
  store volatile i32 2, ptr @g2, align 4
  ret void
}

define internal void @ctor3() section ".text.startup" comdat($g3) {
  store volatile i32 3, ptr @g3, align 4
  ret void
}

define internal void @ctor4() section ".text.startup" comdat($g4) {
  store volatile i32 4, ptr @g4, align 4
  ret void
}

define internal void @ctor5() section ".text.startup" comdat($g5) {
  store volatile i32 5, ptr @g5, align 4
  ret void
}

define internal void @ctor6() section ".text.startup" comdat($g6) {
  store volatile i32 6, ptr @g6, align 4
  ret void
}

define internal void @_GLOBAL__sub_I_mod() section ".text.startup" {
  call void asm sideeffect "", ""()
  ret void
}

define void @rootA() {
  call void @ctor1()
  call void @ctor2()
  call void @ctor3()
  ret void
}

define void @rootB() {
  call void @ctor4()
  call void @ctor5()
  call void @ctor6()
  ret void
}
