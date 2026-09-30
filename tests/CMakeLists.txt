# ReXGlue Tests
# Test infrastructure for PPC instruction validation and unit tests

# Test modules for the module loader tests
add_subdirectory(modules)

# Compile and run checks for the entry point macros
add_subdirectory(samples)

# Unit tests for core components
add_subdirectory(unit)

# PPC instruction tests (requires PPC toolchain)
add_subdirectory(ppc)
