# Sanitizers apply to every target in the build (including GoogleTest), so this module must be
# included before any target is created.
set(REVENANT_SANITIZER
    ""
    CACHE STRING "Sanitizers for the whole build: 'address;undefined' or 'thread'")

if(REVENANT_SANITIZER)
  list(JOIN REVENANT_SANITIZER "," _revenant_sanitizers)
  add_compile_options(-fsanitize=${_revenant_sanitizers} -fno-omit-frame-pointer
                      -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${_revenant_sanitizers})
  message(STATUS "revenant: sanitizers enabled: ${_revenant_sanitizers}")
endif()
