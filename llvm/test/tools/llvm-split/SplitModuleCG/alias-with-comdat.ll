; RUN: llvm-split -enable-split-module-CG=true -j3 -o %t %s
; RUN: llvm-dis -o - %t0 | FileCheck --check-prefix=CHECK0 %s
; RUN: llvm-dis -o - %t1 | FileCheck --check-prefix=CHECK1 %s
; RUN: llvm-dis -o - %t2 | FileCheck --check-prefix=CHECK2 %s

; CHECK0-DAG: $group = comdat any
; CHECK0-DAG: @abar = alias void (), ptr @bar
; CHECK0-DAG: define void @foo() comdat($group)
; CHECK0-DAG: define void @bar() comdat($group)
; CHECK0-DAG: define void @baz()
; CHECK0-DAG: declare void @call_foo()
; CHECK0-DAG: declare void @call_abar()

; CHECK1-DAG: define available_externally void @foo()
; CHECK1-DAG: define available_externally void @bar()
; CHECK1-DAG: declare void @baz()
; CHECK1-DAG: define void @call_foo()
; CHECK1-DAG: declare void @call_abar()
; CHECK1-DAG: declare void @abar()

; CHECK2-DAG: define available_externally void @foo()
; CHECK2-DAG: define available_externally void @bar()
; CHECK2-DAG: declare void @baz()
; CHECK2-DAG: declare void @call_foo()
; CHECK2-DAG: define void @call_abar()
; CHECK2-DAG: declare void @abar()

$group = comdat any

@abar = alias void (), ptr @bar

define void @foo() comdat($group) {
entry:
  ret void
}

define void @bar() comdat($group) {
entry:
  ret void
}

define void @baz() {
entry:
  ret void
}

define void @call_foo() {
entry:
  call void @foo()
  ret void
}

define void @call_abar() {
entry:
  call void @abar()
  ret void
}
