; RUN: llvm-split -enable-split-module-CG=true -j3 -o %t %s
; RUN: llvm-dis -o - %t0 | FileCheck --check-prefix=CHECK0 %s
; RUN: llvm-dis -o - %t1 | FileCheck --check-prefix=CHECK1 %s
; RUN: llvm-dis -o - %t2 | FileCheck --check-prefix=CHECK2 %s


; CHECK0-DAG: @global_b = global i64 11
; CHECK0-DAG: @global_c = global i64 12
; CHECK0-DAG: declare i64 @func_a()
; CHECK0-DAG: declare i64 @func_b()
; CHECK0-DAG: define i64 @func_c()

; CHECK1-DAG: @global_b = external global i64
; CHECK1-DAG: @global_c = external global i64
; CHECK1-DAG: declare i64 @func_a()
; CHECK1-DAG: define i64 @func_b()
; CHECK1-DAG: declare i64 @func_c()

; CHECK2-DAG: @global_b = external global i64
; CHECK2-DAG: @global_c = external global i64
; CHECK2-DAG: define i64 @func_a()
; CHECK2-DAG: declare i64 @func_b()
; CHECK2-DAG: declare i64 @func_c()

@global_a = internal global i32 10 #0
@global_b = global i64 11
@global_c = global i64 12

define i64 @func_a() {
entry:
  %val = load i64, ptr @global_a
  ret i64 %val
}

define i64 @func_b() {
entry:
  %val = load i64, ptr @global_b
  ret i64 %val
}

define i64 @func_c() {
entry:
  %val = load i64, ptr @global_c
  ret i64 %val
}

attributes #0 = { "thinlto-internalize" }
