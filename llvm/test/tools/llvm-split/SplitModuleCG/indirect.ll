; RUN: llvm-split -enable-split-module-CG=true -j2 -o %t %s
; RUN: llvm-dis -o - %t0 | FileCheck --check-prefix=CHECK0 %s
; RUN: llvm-dis -o - %t1 | FileCheck --check-prefix=CHECK1 %s

; CHECK0-DAG: declare void @callee_a()
; CHECK0-DAG: declare void @caller_a(ptr)
; CHECK0-DAG: declare void @func_a()
; CHECK0-DAG: define void @callee_b()
; CHECK0-DAG: define void @func_b(ptr %0)

; CHECK1-DAG: define void @callee_a()
; CHECK1-DAG: define void @caller_a(ptr %0)
; CHECK1-DAG: define void @func_a()
; CHECK1-DAG: declare void @callee_b()
; CHECK1-DAG: declare void @func_b(ptr)

define void @callee_a() {
entry:
    ret void
}

define void @caller_a(ptr %call) {
entry:
    call void %call()
    ret void
}

define void @func_a() {
entry:
    call void @caller_a(ptr @callee_a)
    ret void
}

define void @callee_b() {
entry:
    ret void
}

define void @func_b(ptr %call) {
entry:
    call void %call(), !callees !0
    ret void
}

!0 = !{ptr @callee_b}
