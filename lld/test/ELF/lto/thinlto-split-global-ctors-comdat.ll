; REQUIRES: aarch64
;
; Regression test for ThinLTO split backend partitioning when an init-array ctor
; lives in a COMDAT group with other members.
;
; Before the fix, the ctor itself was anchored to the owner partition, but other
; members of the same COMDAT could still follow the old COMDAT owner selection
; and end up in another split partition. That produced split objects with COMDAT
; members in different files and ld.lld failed while merging them.
;
; Also check that the associated data COMDAT key is preserved even when the
; ctor is referenced through an alias.
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
; RUN: llvm-readelf -Ws %t.so | FileCheck %s --check-prefix=SYMS

; OWNER: $grp = comdat any
; OWNER-DAG: @g = weak_odr hidden global i32 0, comdat($grp), align 4
; OWNER-DAG: @key = weak_odr hidden global i32 0, comdat($keygrp), align 4
; OWNER: @llvm.global_ctors = appending global [2 x { i32, ptr, ptr }]
; OWNER-DAG: define {{.*}} @helper{{.*}}(){{.*}}comdat($grp)
; OWNER-DAG: define {{.*}} @ctor{{.*}}(){{.*}}comdat($grp)
; OWNER-DAG: define {{.*}} @key_owner{{.*}}(){{.*}}comdat($keygrp)
; OWNER-DAG: define {{.*}} @ctor_with_key{{.*}}(){{.*}}section ".text.startup"

; FINAL: .init_array{{.*}}000010{{.*}}
; SYMS-NOT: UND helper
; SYMS-NOT: UND key_owner

target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-unknown-linux-gnu"

$grp = comdat any
$keygrp = comdat any

@g = weak_odr hidden global i32 0, comdat($grp), align 4
@key = weak_odr hidden global i32 0, comdat($keygrp), align 4
@llvm.used = appending global [2 x ptr] [ptr @g, ptr @key], section "llvm.metadata"

@llvm.global_ctors = appending global [2 x { i32, ptr, ptr }] [
  { i32, ptr, ptr } { i32 65535, ptr @ctor, ptr @g },
  { i32, ptr, ptr } { i32 65535, ptr @ctor_alias, ptr @key }
]

@ctor_alias = internal alias void (), ptr @ctor_with_key

define internal void @helper() noinline optnone comdat($grp) {
  store volatile i32 7, ptr @g, align 4
  ret void
}

define internal void @key_owner() noinline optnone comdat($keygrp) {
  store volatile i32 8, ptr @key, align 4
  ret void
}

define internal void @ctor() section ".text.startup" comdat($grp) {
  store volatile i32 9, ptr @g, align 4
  ret void
}

define internal void @ctor_with_key() section ".text.startup" {
  store volatile i32 10, ptr @key, align 4
  ret void
}

define void @rootA() {
  call void @helper()
  call void @key_owner()
  ret void
}

define void @rootB() {
  call void asm sideeffect "", ""()
  ret void
}
