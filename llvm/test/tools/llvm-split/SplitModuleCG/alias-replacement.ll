; RUN: llvm-split -enable-split-module-CG=true -j2 -o %t %s
; RUN: llvm-dis -o - %t0 | FileCheck --check-prefix=CHECK0 %s
; RUN: llvm-dis -o - %t1 | FileCheck --check-prefix=CHECK1 %s

; CHECK0-DAG: @foo = dso_local global [2 x i64] [i64 100, i64 200], align 8
; CHECK0-DAG: @afoo = dso_local alias [2 x ptr], ptr @foo
; CHECK0-DAG: @abar = dso_local alias void (), ptr @bar
; CHECK1-DAG: @foo = external dso_local global [2 x i64], align 8
; CHECK1-DAG: @afoo = external global [2 x ptr]

@foo = dso_local global [2 x i64] [i64 100, i64 200], align 8

@afoo = dso_local alias [2 x ptr], ptr @foo
@abar = dso_local alias void (), ptr @bar

; CHECK0-DAG: define dso_local void @bar()
; CHECK0-DAG: declare void @call_abar()
; CHECK0-DAG: define void @call_afoo()
; CHECK0-Next: call void @foo()
; CHECK1-DAG: define available_externally dso_local void @bar()
; CHECK1-DAG: define void @call_abar()
; CHECK1-Next: call void @bar()
; CHECK1-DAG: declare void @call_afoo()

define dso_local void @bar() {
entry:
  ret void
}

define void @call_abar() {
entry:
  call void @abar()
  ret void
}

define void @call_afoo() {
entry:
  call void @afoo()
  ret void
}
