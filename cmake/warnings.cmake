# Project warning level (Part 2.2): /W4 /WX on MSVC, -Wall -Wextra -Werror on Clang/GCC.
# Applied per target so third-party code is unaffected.
function(tachyon_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wno-gnu-zero-variadic-macro-arguments)
  endif()
  set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
endfunction()
