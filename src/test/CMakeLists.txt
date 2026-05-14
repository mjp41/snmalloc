# Test directory CMakeLists.txt for snmalloc

set(TESTDIR ${CMAKE_CURRENT_SOURCE_DIR})

if(SNMALLOC_BUILD_TESTING)
  enable_testing()
  # The test layout is <category>/<test>/*.cc; one glob over
  # the matching depth captures every test source. A test directory
  # may contain several .cc files, all linked into the same
  # executable by aux_source_directory below (e.g.
  # func/two_alloc_types/{alloc1,alloc2,main}.cc). Parsing the first
  # two path segments of each match yields a (category, test) pair;
  # duplicates from multi-source tests are collapsed with
  # REMOVE_DUPLICATES.
  #
  # CONFIGURE_DEPENDS is intentionally anchored on the .cc files
  # aux_source_directory() picks up inside each test dir, so the set
  # of paths recorded in CMake's VerifyGlobs.cmake stays in lock-step
  # with the inputs that actually affect build.ninja. A glob over
  # ${TESTDIR}/* (files+dirs) would also react to unrelated files
  # sitting next to the category dirs - editor swap files, READMEs,
  # the snmalloc_testlib.cc helper - and trip VerifyGlobs without
  # producing any change in build.ninja, leaving ninja wedged in a
  # cmake-regenerate loop until the build dir is cleared (RERUN_CMAKE
  # / VERIFY_GLOBS in rules.ninja do not have restat=1).
  file(GLOB _test_sources
    CONFIGURE_DEPENDS LIST_DIRECTORIES false RELATIVE ${TESTDIR}
    ${TESTDIR}/*/*/*.cc)
  set(TEST_CATEGORIES "")
  foreach(_src ${_test_sources})
    string(REGEX MATCH "^([^/]+)/([^/]+)/" _ ${_src})
    set(_category ${CMAKE_MATCH_1})
    set(_test ${CMAKE_MATCH_2})
    if(NOT _category IN_LIST TEST_CATEGORIES)
      list(APPEND TEST_CATEGORIES ${_category})
    endif()
    # Per-category test list written to a directory-scoped variable
    # so make_tests below can read it via function-scope fall-through.
    list(APPEND TESTS_${_category} ${_test})
  endforeach()
  foreach(_category ${TEST_CATEGORIES})
    list(REMOVE_DUPLICATES TESTS_${_category})
  endforeach()
else()
  set(TEST_CATEGORIES "")
endif()
list(REVERSE TEST_CATEGORIES)

# Tests that only include snmalloc_testlib.h (no snmalloc.h or snmalloc_core.h).
# These are mitigation-independent and can be compiled once, then linked
# against both fast and check testlib variants.
set(TESTLIB_ONLY_TESTS
  bits first_operation memory memory_usage multi_atexit multi_threadatexit
  redblack statistics teardown
  contention external_pointer large_alloc lotsofthreads post_teardown
  singlethread startup
)

function(make_tests TAG DEFINES)
  foreach(TEST_CATEGORY ${TEST_CATEGORIES})
    message(VERBOSE "Adding ${TAG}/${TEST_CATEGORY} tests")
    foreach(TEST ${TESTS_${TEST_CATEGORY}})
      unset(SRC)
      aux_source_directory(${TESTDIR}/${TEST_CATEGORY}/${TEST} SRC)
      set(TESTNAME "${TEST_CATEGORY}-${TEST}-${TAG}")

      # Check if this test is testlib-only (mitigation-independent).
      # If so, compile the source once into an object library and reuse
      # across flavours.
      list(FIND TESTLIB_ONLY_TESTS ${TEST} _testlib_only_idx)
      set(OBJLIBNAME "${TEST_CATEGORY}-${TEST}-obj")
      if (NOT ${_testlib_only_idx} EQUAL -1 AND NOT TARGET ${OBJLIBNAME})
        # First flavour to see this test creates the object library.
        add_library(${OBJLIBNAME} OBJECT ${SRC})
        target_link_libraries(${OBJLIBNAME} snmalloc)
        target_compile_definitions(${OBJLIBNAME} PRIVATE "SNMALLOC_USE_${TEST_CLEANUP}")
        add_warning_flags(${OBJLIBNAME})
        if(SNMALLOC_SANITIZER)
          target_compile_options(${OBJLIBNAME} PRIVATE -g -fsanitize=${SNMALLOC_SANITIZER} -fno-omit-frame-pointer)
          if (${SNMALLOC_SANITIZER} MATCHES "thread")
            target_compile_definitions(${OBJLIBNAME} PRIVATE SNMALLOC_THREAD_SANITIZER_ENABLED)
          endif()
        endif()
      endif()

      if (NOT ${_testlib_only_idx} EQUAL -1)
        # Testlib-only: link the shared object library against this
        # flavour's testlib.
        add_executable(${TESTNAME} $<TARGET_OBJECTS:${OBJLIBNAME}>)
      else()
        add_executable(${TESTNAME} ${SRC})
      endif()

      # Track every test binary in a global property so the `coverage`
      # custom target can pass them all to llvm-cov without globbing.
      set_property(GLOBAL APPEND PROPERTY SNMALLOC_TEST_BINARIES ${TESTNAME})

      if(SNMALLOC_SANITIZER)
        target_compile_options(${TESTNAME} PRIVATE -g -fsanitize=${SNMALLOC_SANITIZER} -fno-omit-frame-pointer)
        target_link_libraries(${TESTNAME} -fsanitize=${SNMALLOC_SANITIZER})
        if (${SNMALLOC_SANITIZER} MATCHES "thread")
          target_compile_definitions(${TESTNAME} PRIVATE SNMALLOC_THREAD_SANITIZER_ENABLED)
        endif()
      endif()

      add_warning_flags(${TESTNAME})

      target_link_libraries(${TESTNAME} snmalloc)
      # Link the pre-compiled test library into all tests.  For tests
      # that include snmalloc.h directly the linker will simply discard
      # the duplicate inline definitions (ODR-safe).
      target_link_libraries(${TESTNAME} snmalloc-testlib-${TAG})
      target_compile_definitions(${TESTNAME} PRIVATE "SNMALLOC_USE_${TEST_CLEANUP}")
      
      if (NOT DEFINES STREQUAL " ")
        target_compile_definitions(${TESTNAME} PRIVATE ${DEFINES})
      endif()

      if (${TEST} MATCHES "release-.*")
        message(VERBOSE "Adding test: ${TESTNAME} only for release configs")
        add_test(NAME ${TESTNAME} COMMAND ${TESTNAME} --smoke
                 CONFIGURATIONS "Release")
      else()
        message(VERBOSE "Adding test: ${TESTNAME}")
        add_test(NAME ${TESTNAME} COMMAND ${TESTNAME} --smoke)
      endif()
      if (${TEST_CATEGORY} MATCHES "perf")
        message(VERBOSE "Single threaded test: ${TESTNAME}")
        set_tests_properties(${TESTNAME} PROPERTIES PROCESSORS 4)
      endif()
      if (SNMALLOC_COVERAGE)
        # Per-test LLVM_PROFILE_FILE so cached coverage runs can
        # invalidate one test's profiles without touching the rest.
        set_tests_properties(${TESTNAME} PROPERTIES ENVIRONMENT
          "LLVM_PROFILE_FILE=${CMAKE_BINARY_DIR}/profiles/${TESTNAME}/%p-%m.profraw")
      endif()
      if(WIN32)
        # On Windows these tests use a lot of memory as it doesn't support
        # lazy commit.
        if (${TEST} MATCHES "two_alloc_types")
          message(VERBOSE "Single threaded test: ${TESTNAME}")
          set_tests_properties(${TESTNAME} PROPERTIES PROCESSORS 4)
        endif()
        if (${TEST} MATCHES "fixed_region")
          message(VERBOSE "Single threaded test: ${TESTNAME}")
          set_tests_properties(${TESTNAME} PROPERTIES PROCESSORS 4)
        endif()
        if (${TEST} MATCHES "memory")
          message(VERBOSE "Single threaded test: ${TESTNAME}")
          set_tests_properties(${TESTNAME} PROPERTIES PROCESSORS 4)
        endif()
      endif()
    endforeach()
  endforeach()
endfunction()

function(build_test_library TAG DEFINES)
  set(LIBNAME snmalloc-testlib-${TAG})
  add_library(${LIBNAME} STATIC snmalloc_testlib.cc)
  target_link_libraries(${LIBNAME} PRIVATE snmalloc)
  set_target_properties(${LIBNAME} PROPERTIES INTERFACE_LINK_LIBRARIES "")
  target_compile_definitions(${LIBNAME} PRIVATE "SNMALLOC_USE_${TEST_CLEANUP}")
  if (NOT DEFINES STREQUAL " ")
    target_compile_definitions(${LIBNAME} PRIVATE ${DEFINES})
  endif()
  if(SNMALLOC_SANITIZER)
    target_compile_options(${LIBNAME} PRIVATE -g -fsanitize=${SNMALLOC_SANITIZER} -fno-omit-frame-pointer)
    if (${SNMALLOC_SANITIZER} MATCHES "thread")
      target_compile_definitions(${LIBNAME} PRIVATE SNMALLOC_THREAD_SANITIZER_ENABLED)
    endif()
  endif()
  add_warning_flags(${LIBNAME})
endfunction()

if (SNMALLOC_BUILD_TESTING)
  set(FLAVOURS fast;check)

  foreach(FLAVOUR ${FLAVOURS})
    if (${FLAVOUR} STREQUAL "check")
      set(DEFINES SNMALLOC_CHECK_CLIENT)
    endif()
    if (${FLAVOUR} STREQUAL "fast")
      set(DEFINES " ")
    endif()

    build_test_library(${FLAVOUR} ${DEFINES})
    make_tests(${FLAVOUR} ${DEFINES})
  endforeach()
endif()

# Install test headers
install(FILES
    measuretime.h
    opt.h
    setup.h
    usage.h
    xoroshiro.h
  DESTINATION include/snmalloc/test
)
