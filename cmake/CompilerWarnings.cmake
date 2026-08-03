# Applies a shared, strict warning set to a target.
function(softplc_apply_warnings target)
    target_compile_options(${target} INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wshadow>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
    )
endfunction()
