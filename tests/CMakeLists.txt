# ReXGlue Tests
# Test infrastructure for PPC instruction validation and unit tests

# Test modules for the module loader tests
add_subdirectory(modules)

# Compile and run checks for the entry point macros
add_subdirectory(samples)

add_subdirectory(core)

add_subdirectory(headers)

add_subdirectory(filesystem)

# Unit tests for core components
add_subdirectory(unit)

# PPC instruction tests (requires PPC toolchain)
if(REXGLUE_BUILD_PPC_TESTS)
    add_subdirectory(ppc)
endif()
