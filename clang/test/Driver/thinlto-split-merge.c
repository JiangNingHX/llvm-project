// RUN: %clang -### -target x86_64-unknown-linux-gnu -c -fthinlto-index=foo.thinlto.bc -x ir %s -o foo.o \
// RUN:   -mllvm -thinlto-split=true \
// RUN:   -mllvm -thinlto-split-partitions=2 2>&1 | FileCheck %s --check-prefix=MERGE
// RUN: %clang -### -target x86_64-unknown-linux-gnu -B%S/Inputs/lld -fuse-ld=lld \
// RUN:   -c -fthinlto-index=foo.thinlto.bc -x ir %s -o foo.o \
// RUN:   -mllvm -thinlto-split=true 2>&1 | FileCheck %s --check-prefix=LLD

// MERGE: "-cc1"
// MERGE-SAME: "-fthinlto-index=foo.thinlto.bc"
// MERGE-SAME: "-thinlto-split-output-list=[[RSP:[^"]+\.thinlto-split\.rsp]]"
// MERGE-SAME: "-o" "[[TEMP_O:[^"]+\.o]]"
// MERGE: "{{.*}}ld{{.*}}" "-r" "-o" "foo.o" "@[[RSP]]"

// LLD: "-cc1"
// LLD-SAME: "-thinlto-split-output-list=[[LLD_RSP:[^"]+\.thinlto-split\.rsp]]"
// LLD: "{{.*}}/Inputs/lld/ld.lld" "-r" "-o" "foo.o" "@[[LLD_RSP]]"
