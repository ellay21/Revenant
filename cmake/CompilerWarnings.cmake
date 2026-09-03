# Applies the project warning set to a target. Position arithmetic is where sign and
# narrowing bugs live, so -Wconversion and -Wsign-conversion are not negotiable.
function(revenant_set_warnings target)
  set(warnings
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
      -Wshadow
      -Wold-style-cast
      -Wcast-align
      -Wcast-qual
      -Wnon-virtual-dtor
      -Woverloaded-virtual
      -Wdouble-promotion
      -Wformat=2
      -Wimplicit-fallthrough
      -Wmissing-declarations)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND warnings -Wduplicated-cond -Wduplicated-branches -Wlogical-op)
  endif()
  if(REVENANT_WARNINGS_AS_ERRORS)
    list(APPEND warnings -Werror)
  endif()
  target_compile_options(${target} PRIVATE ${warnings})
endfunction()
