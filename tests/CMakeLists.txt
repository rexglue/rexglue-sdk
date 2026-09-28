# ReXGlue Tests
# Test infrastructure for PPC instruction validation and unit tests

# Test modules for the module loader tests
add_subdirectory(modules)

# Unit tests for core components
add_subdirectory(unit)

# PPC instruction tests (requires PPC toolchain)
add_subdirectory(ppc)
