# compiler-rt builtins for the GBA build, which has no sysroot, compiled
# from source on demand: see sm_add_builtins().
#
# They come from an llvm-project checkout at the tag matching the compiler.
# By default it is sparse-cloned into the build tree at configure time; set
# SM_LLVM_SOURCE to use an existing checkout instead.

set(SM_LLVM_TAG "llvmorg-${CMAKE_C_COMPILER_VERSION}" CACHE STRING
    "llvm-project tag to take compiler-rt builtins from")
set(SM_LLVM_SOURCE "" CACHE PATH
    "Existing llvm-project checkout (default: sparse-clone SM_LLVM_TAG)")

if(SM_LLVM_SOURCE)
    set(llvm_source ${SM_LLVM_SOURCE})
else()
    set(llvm_source ${CMAKE_BINARY_DIR}/_deps/llvm-project)
    if(NOT EXISTS ${llvm_source}/compiler-rt/lib/builtins)
        message(STATUS "Fetching llvm-project ${SM_LLVM_TAG} (sparse)")
        execute_process(
            COMMAND git clone --quiet --depth 1 --branch ${SM_LLVM_TAG}
                    --filter=blob:none --sparse
                    https://github.com/llvm/llvm-project.git ${llvm_source}
            COMMAND_ERROR_IS_FATAL ANY)
        execute_process(
            COMMAND git -C ${llvm_source} sparse-checkout set
                    compiler-rt/lib/builtins
            COMMAND_ERROR_IS_FATAL ANY)
    endif()
endif()

# Adds a static library of the named compiler-rt builtins (file names under
# compiler-rt/lib/builtins, e.g. "udivsi3.c" or "arm/aeabi_uidivmod.S").
#
# Everything is built as Thumb, assembly included. ARMv4T's POP {pc} doesn't
# interwork, and helpers such as __aeabi_uldivmod return that way, so they
# must be in the same instruction set as their callers: Thumb, like all of
# the C outside *.iwram.c. ARM-mode code (crt0, *.iwram.c) must not call
# them; check with llvm-nm on its object when it starts dividing.
function(sm_add_builtins target)
    list(TRANSFORM ARGN PREPEND ${llvm_source}/compiler-rt/lib/builtins/)
    add_library(${target} STATIC ${ARGN})
    target_compile_options(${target} PRIVATE -mthumb -ffreestanding -fno-builtin)
endfunction()
