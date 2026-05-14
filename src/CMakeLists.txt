# Source directory CMakeLists.txt for snmalloc

# Configure clang-format targets for the src directory
if (SNMALLOC_BUILD_TESTING)
  clangformat_targets(
    TARGET_NAME clangformat-src
    INCLUDE_PATTERNS "*.cc" "*.h" "*.hh"
  )
endif()
