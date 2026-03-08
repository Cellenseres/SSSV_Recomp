if(NOT DEFINED INPUT_FILE)
  message(FATAL_ERROR "INPUT_FILE is required")
endif()

file(READ "${INPUT_FILE}" PATCH_SOURCE)
set(PATCH_SOURCE_ORIGINAL "${PATCH_SOURCE}")

# N64Recomp already renames these symbols for normal function emission, but patch
# reference-symbol calls can still come out as raw libc names on some platforms.
# Only rewrite call sites that use the patch ABI signature.
set(RENAMED_PATCH_CALLS
  "sincosf"
  "sinf"
  "cosf"
  "__sinf"
  "__cosf"
  "asinf"
  "acosf"
  "atanf"
  "atan2f"
  "tanf"
  "sqrt"
  "sqrtf"
  "memcpy"
  "memset"
  "memmove"
  "memcmp"
  "strcmp"
  "strcat"
  "strcpy"
  "strchr"
  "strlen"
  "strtok"
  "sprintf"
  "bzero"
  "bcopy"
  "bcmp"
  "setjmp"
  "longjmp"
  "ldiv"
)

foreach(FUNC_NAME IN LISTS RENAMED_PATCH_CALLS)
  string(REPLACE
    "${FUNC_NAME}(rdram, ctx);"
    "${FUNC_NAME}_recomp(rdram, ctx);"
    PATCH_SOURCE
    "${PATCH_SOURCE}"
  )
endforeach()

if(NOT PATCH_SOURCE STREQUAL PATCH_SOURCE_ORIGINAL)
  file(WRITE "${INPUT_FILE}" "${PATCH_SOURCE}")
endif()
