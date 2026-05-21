; RUN: llvm-split -enable-split-module-CG=true -j2 -o %t %s
; RUN: llvm-dis -o - %t0 | FileCheck --check-prefix=CHECK0 %s

; CHECK0: @middle = alias void (), ptr @base
; CHECK0: @top = alias void (), ptr @middle
; CHECK0: define void @base()
; CHECK0; define void @caller()

@middle = alias void (), ptr @base
@top    = alias void (), ptr @middle

define void @base() { ret void }
define void @caller() {
  call void @top()
  ret void
}
