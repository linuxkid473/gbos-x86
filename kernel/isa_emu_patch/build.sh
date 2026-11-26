#!/bin/sh
# incremental kernel build inside the gbos-kbuild image
C=$(ls -d /build/inputs/clang-r584948* 2>/dev/null | head -1)
export PATH=$C/bin:$PATH LD_LIBRARY_PATH=$C/lib:$LD_LIBRARY_PATH
make -C /build/src O=/build/out ARCH=x86_64 LLVM=$C/bin/ CC=$C/bin/clang-22 -j${JOBS:-6} bzImage modules 2>&1
echo EXIT=$?
