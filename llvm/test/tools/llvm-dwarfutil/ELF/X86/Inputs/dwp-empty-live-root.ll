target triple = "x86_64-unknown-linux-gnu"

@x = external dso_local global i8, align 1, !dbg !0

!llvm.dbg.cu = !{!2}
!llvm.module.flags = !{!6, !7}
!llvm.ident = !{!8}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "x", scope: !2, file: !3, line: 1, isLocal: false, isDefinition: false)
!2 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !3, producer: "clang version 8.0.0", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug, enums: !4, globals: !5, splitDebugInlining: false, nameTableKind: None)
!3 = !DIFile(filename: "empty.cpp", directory: "/home/test/PRs/PR38990")
!4 = !{}
!5 = !{!0}
!6 = !{i32 2, !"Dwarf Version", i32 4}
!7 = !{i32 2, !"Debug Info Version", i32 3}
!8 = !{!"clang version 8.0.0"}
