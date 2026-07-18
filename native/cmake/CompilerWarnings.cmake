# CompilerWarnings.cmake — strict warning policy for HengYuan native core.
# Usage: hengyuan_set_warnings(<target>)

function(hengyuan_set_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
            -Wall -Wextra -Wpedantic -Werror
            -Wconversion -Wsign-conversion -Wshadow
            -Wnon-virtual-dtor -Wcast-align
            -Woverloaded-virtual
            -Wdouble-promotion -Wformat=2
        >
        $<$<CXX_COMPILER_ID:MSVC>:
            /W4 /WX /permissive- /utf-8
            /wd4324
        >
    )
endfunction()
