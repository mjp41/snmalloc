##
# clangformat_targets([VERSION <ver>]
#                     [TARGET_NAME <name>]
#                     [INCLUDE_PATTERNS <glob> ...]
#                     [EXCLUDE_PATTERNS <regex> ...])
#
# Generate a CMake target that runs clang-format on a set of source files.
# A companion <name>-check target is also created that verifies formatting
# without modifying files (--dry-run --Werror), suitable for CI.
#
# This function can be called multiple times to create multiple format/check targets.
# Each call with a unique TARGET_NAME will accumulate files for that target.
#
# Options:
#   VERSION          - clang-format version to search for (default: 15).
#                      Falls back to the unversioned 'clang-format' with a
#                      warning if the requested version is not found.
#   TARGET_NAME      - Name of the generated targets (default: clangformat).
#   INCLUDE_PATTERNS - Glob patterns passed to file(GLOB_RECURSE) to collect
#                      source files (default: src/*.cc src/*.h src/*.hh).
#   EXCLUDE_PATTERNS - Regex patterns used to exclude files from the collected
#                      list (default: src/[^/]*/[^/]*_concept\.h$).
function(clangformat_targets)
  cmake_parse_arguments(
    CLANGFMT
    ""
    "VERSION;TARGET_NAME"
    "INCLUDE_PATTERNS;EXCLUDE_PATTERNS"
    ${ARGN}
  )

  # Default inclusion globs match this project's source layout.
  if (NOT CLANGFMT_INCLUDE_PATTERNS)
    set(CLANGFMT_INCLUDE_PATTERNS src/*.cc src/*.h src/*.hh)
  endif()

  # Default exclusion regexes skip concept headers that clang-format
  # does not yet handle well.  See https://reviews.llvm.org/D79773
  if (NOT CLANGFMT_EXCLUDE_PATTERNS)
    set(CLANGFMT_EXCLUDE_PATTERNS "src/[^/]*/[^/]*_concept\\.h$")
  endif()

  # Default clang-format version.
  if (NOT CLANGFMT_VERSION)
    set(CLANGFMT_VERSION 15)
  endif()

  # Default target name.
  if (NOT CLANGFMT_TARGET_NAME)
    set(CLANGFMT_TARGET_NAME clangformat)
  endif()

  # Use global properties to accumulate globs and exclusions for each target name.
  set(_clangfmt_globs_prop "CLANGFORMAT_GLOBS_${CLANGFMT_TARGET_NAME}")
  set(_clangfmt_excludes_prop "CLANGFORMAT_EXCLUDES_${CLANGFMT_TARGET_NAME}")

  # Append new globs and exclusions to the global property.
  get_property(_old_globs GLOBAL PROPERTY ${_clangfmt_globs_prop})
  get_property(_old_excludes GLOBAL PROPERTY ${_clangfmt_excludes_prop})
  if(NOT _old_globs)
    set(_old_globs "")
  endif()
  if(NOT _old_excludes)
    set(_old_excludes "")
  endif()
  set(_new_globs "${_old_globs};${CLANGFMT_INCLUDE_PATTERNS}")
  set(_new_excludes "${_old_excludes};${CLANGFMT_EXCLUDE_PATTERNS}")
  set_property(GLOBAL PROPERTY ${_clangfmt_globs_prop} "${_new_globs}")
  set_property(GLOBAL PROPERTY ${_clangfmt_excludes_prop} "${_new_excludes}")

  # The clang-format tool is installed under a variety of different names.  Try
  # to find a sensible one.  Look for the requested version first, then fall
  # back to the unversioned name.
  find_program(CLANG_FORMAT NAMES
    clang-format${CLANGFMT_VERSION}0
    clang-format-${CLANGFMT_VERSION})

  if (${CLANG_FORMAT} STREQUAL "CLANG_FORMAT-NOTFOUND")
    find_program(CLANG_FORMAT NAMES clang-format)
    if (NOT ${CLANG_FORMAT} STREQUAL "CLANG_FORMAT-NOTFOUND")
      message(WARNING "Could not find clang-format version ${CLANGFMT_VERSION}, falling back to ${CLANG_FORMAT}")
    endif()
  endif()

  # If we've found a clang-format tool, generate a target for it, otherwise emit
  # a warning.
  if (${CLANG_FORMAT} STREQUAL "CLANG_FORMAT-NOTFOUND")
    message(WARNING "Not generating ${CLANGFMT_TARGET_NAME} target, no clang-format tool found")
  else ()
    # Gather all files from all accumulated globs, then apply all exclusions.
    set(_all_globs "")
    get_property(_all_globs GLOBAL PROPERTY ${_clangfmt_globs_prop})
    set(_all_excludes "")
    get_property(_all_excludes GLOBAL PROPERTY ${_clangfmt_excludes_prop})
    # Flatten lists
    separate_arguments(_all_globs UNIX_COMMAND "${_all_globs}")
    separate_arguments(_all_excludes UNIX_COMMAND "${_all_excludes}")
    set(ALL_SOURCE_FILES "")
    foreach(glob IN LISTS _all_globs)
      file(GLOB_RECURSE _files CONFIGURE_DEPENDS ${glob})
      list(APPEND ALL_SOURCE_FILES ${_files})
    endforeach()
    foreach(pattern IN LISTS _all_excludes)
      list(FILTER ALL_SOURCE_FILES EXCLUDE REGEX "${pattern}")
    endforeach()
    # Remove old targets if they exist (to allow redefinition)
    if(TARGET ${CLANGFMT_TARGET_NAME})
      set_property(GLOBAL PROPERTY TARGETS_TO_REMOVE "${CLANGFMT_TARGET_NAME}")
      set_property(GLOBAL PROPERTY TARGETS_TO_REMOVE "${CLANGFMT_TARGET_NAME}-check")
      # CMake does not support removing targets, but redefining is safe in most cases
    endif()
    add_custom_target(
      ${CLANGFMT_TARGET_NAME}
      COMMAND ${CLANG_FORMAT}
      -i
      ${ALL_SOURCE_FILES})
    add_custom_target(
      ${CLANGFMT_TARGET_NAME}-check
      COMMAND ${CLANG_FORMAT}
      --dry-run --Werror
      ${ALL_SOURCE_FILES})
  endif()
endfunction()