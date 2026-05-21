; RUN: llvm-split -enable-split-module-CG=true -j4 -o %t %s
; RUN: llvm-dis -o - %t0 | FileCheck --check-prefix=CHECK0 %s
; RUN: llvm-dis -o - %t1 | FileCheck --check-prefix=CHECK1 %s
; RUN: llvm-dis -o - %t2 | FileCheck --check-prefix=CHECK2 %s
; RUN: llvm-dis -o - %t3 | FileCheck --check-prefix=CHECK3 %s


; CHECK0-DAG: @vtable_with_metadata = global [1 x ptr] [ptr @func_a], align 8, !type !0
; CHECK0-DAG: define void @func_a()
; CHECK0-DAG: define void @user_of_metadata_vtable()
; CHECK1-DAG: @vtable_with_metadata = external global [1 x ptr], align 8, !type !0
; CHECK1-DAG: declare void @func_a()
; CHECK1-DAG: declare void @user_of_metadata_vtable()
; CHECK2-DAG: @vtable_with_metadata = external global [1 x ptr], align 8, !type !0
; CHECK2-DAG: declare void @func_a()
; CHECK2-DAG: declare void @user_of_metadata_vtable()
; CHECK3-DAG: @vtable_with_metadata = external global [1 x ptr], align 8, !type !0
; CHECK3-DAG: declare void @func_a()
; CHECK3-DAG: declare void @user_of_metadata_vtable()

@vtable_with_metadata = global [1 x ptr] [ptr @func_a], align 8, !type !0

define void @func_a() {
entry:
  ret void
}

define void @user_of_metadata_vtable() {
entry:
  %val = load ptr, ptr @vtable_with_metadata, align 8
  ret void
}


; CHECK0-DAG: @_ZTV10MyCppClass = global [1 x ptr] [ptr @func_b], align 8
; CHECK0-DAG: declare void @func_b()
; CHECK0-DAG: declare void @user_of_cpp_vtable_0()
; CHECK0-DAG: declare void @user_of_cpp_vtable_1()
; CHECK1-DAG: @_ZTV10MyCppClass = available_externally global [1 x ptr] [ptr @func_b], align 8
; CHECK1-DAG: define void @func_b()
; CHECK1-DAG: declare void @user_of_cpp_vtable_0()
; CHECK1-DAG: define void @user_of_cpp_vtable_1()
; CHECK2-DAG: @_ZTV10MyCppClass = available_externally global [1 x ptr] [ptr @func_b], align 8
; CHECK2-DAG: define available_externally void @func_b()
; CHECK2-DAG: define void @user_of_cpp_vtable_0()
; CHECK2-DAG: declare void @user_of_cpp_vtable_1()
; CHECK4: external global [1 x ptr], align 8
; CHECK4: declare void @func_b()
; CHECK4: declare void @user_of_cpp_vtable_0()
; CHECK4: declare void @user_of_cpp_vtable_1()

@_ZTV10MyCppClass = global [1 x ptr] [ptr @func_b], align 8

define void @func_b() {
entry:
  ret void
}

define void @user_of_cpp_vtable_0() {
entry:
  %val = load ptr, ptr @_ZTV10MyCppClass, align 8
  ret void
}

define void @user_of_cpp_vtable_1() {
entry:
  %val = load ptr, ptr @_ZTV10MyCppClass, align 8
  ret void
}


; CHECK0-DAG: @normal_global = global [1 x ptr] [ptr @func_c], align 8
; CHECK0-DAG: declare void @func_c()
; CHECK0-DAG: declare void @user_of_normal_global()
; CHECK1-DAG: @normal_global = external global [1 x ptr], align 8
; CHECK1-DAG: declare void @func_c()
; CHECK1-DAG: declare void @user_of_normal_global()
; CHECK2-DAG: @normal_global = external global [1 x ptr], align 8
; CHECK2-DAG: declare void @func_c()
; CHECK2-DAG: declare void @user_of_normal_global()
; CHECK3-DAG: @normal_global = external global [1 x ptr], align 8
; CHECK3-DAG: define void @func_c()
; CHECK3-DAG: define void @user_of_normal_global()

@normal_global = global [1 x ptr] [ptr @func_c], align 8

define void @func_c() {
entry:
  ret void
}

define void @user_of_normal_global() {
entry:
  %val0 = load ptr, ptr @normal_global, align 8
  %val1 = load ptr, ptr @normal_global, align 8
  ret void
}

!0 = !{i64 0, !"_ZTS1A"}
