; RUN: llvm-split -enable-split-module-CG=true -j2 -o %t %s
; RUN: llvm-dis -o - %t0 | FileCheck --check-prefix=CHECK0 %s
; RUN: llvm-dis -o - %t1 | FileCheck --check-prefix=CHECK1 %s

; CHECK0-DAG: @foo_a.ifunc = weak_odr ifunc void (), ptr @foo_a.resolver
; CHECK0-DAG: @foo_b.ifunc = hidden ifunc void (), ptr @foo_b.resolver
; CHECK1-DAG: declare void @foo_a.ifunc()
; CHECK1-DAG: declare void @foo_b.ifunc()

@foo_a.ifunc = weak_odr ifunc void (), ptr @foo_a.resolver
@foo_b.ifunc = internal ifunc void (), ptr @foo_b.resolver

; CHECK0-DAG: define hidden void @foo.impl()
; CHECK1-DAG: define available_externally hidden void @foo.impl()

define internal void @foo.impl() {
entry:
  ret void
}

; CHECK0-DAG: define hidden ptr @foo_a.resolver()
; CHECK0-DAG: define hidden ptr @foo_b.resolver()
; CHECK1-DAG: define available_externally hidden ptr @foo_a.resolver()
; CHECK1-DAG: declare hidden ptr @foo_b.resolver()

define internal ptr @foo_a.resolver() {
entry:
  ret ptr @foo.impl
}

define internal ptr @foo_b.resolver() {
entry:
  ret ptr @foo.impl
}

; CHECK0-DAG: declare void @bar_a()
; CHECK0-DAG: define void @bar_b()
; CHECK1-DAG: define void @bar_a()
; CHECK1-DAG: declare void @bar_b()

define void @bar_a() {
entry:
  call void @foo_a.ifunc()
  ret void
}

define void @bar_b() {
entry:
  call void @foo_b.ifunc()
  ret void
}
