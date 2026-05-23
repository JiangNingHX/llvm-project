; REQUIRES: x86-registered-target

; RUN: opt -thinlto-bc -o %t.o %s
; RUN: llvm-lto2 run -thinlto-distributed-indexes %t.o \
; RUN:   -o %t.index \
; RUN:   -r=%t.o,caller_a,px \
; RUN:   -r=%t.o,caller_b,px

; RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu \
; RUN:   -emit-obj -fthinlto-index=%t.o.thinlto.bc \
; RUN:   -o %t.split.o -x ir %t.o \
; RUN:   -mllvm -thinlto-split=true \
; RUN:   -mllvm -thinlto-split-partitions=2 \
; RUN:   -mllvm -thinlto-split-module-size-threshold=0 \
; RUN:   -mllvm -thinlto-split-module-size-rate-threshold=2.0 \
; RUN:   -thinlto-split-output-list=%t.split.rsp
; RUN: FileCheck %s --check-prefix=SPLIT-RSP --input-file=%t.split.rsp
; RUN: ld.lld -r -o %t.merged.o @%t.split.rsp
; RUN: llvm-nm %t.merged.o | FileCheck %s --check-prefix=NM

; RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu \
; RUN:   -emit-obj -fthinlto-index=%t.o.thinlto.bc \
; RUN:   -o %t.skip.o -x ir %t.o \
; RUN:   -mllvm -thinlto-split=true \
; RUN:   -mllvm -thinlto-split-partitions=2 \
; RUN:   -thinlto-split-output-list=%t.skip.rsp
; RUN: FileCheck %s --check-prefix=SKIP-RSP --input-file=%t.skip.rsp

; SPLIT-RSP: {{.*}}.split.o
; SPLIT-RSP-NEXT: {{.*}}.split.o.thinlto-split.1.o
; SPLIT-RSP-NOT: thinlto-split.2.o

; SKIP-RSP: {{.*}}.skip.o
; SKIP-RSP-NOT: thinlto-split

; NM-DAG: T caller_a
; NM-DAG: T caller_b
; NM: T {{.*shared[._][0-9a-f]+.*}}
; NM-NOT: T shared{{$}}

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define internal void @shared() {
entry:
  ret void
}

define void @caller_a() {
entry:
  call void @shared()
  ret void
}

define void @caller_b() {
entry:
  call void @shared()
  ret void
}
