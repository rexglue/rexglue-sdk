# ReXGlue developer environment setup target.
# Provides the `config-rexdevenv` target that installs the pre-commit hook,
# VS terminal profile, and verifies the toolchain.
# Requires PowerShell 7 (pwsh) on all platforms.

set(_psrex_module "${REXGLUE_ROOT}/scripts/PSReX")
add_custom_target(
  config-rexdevenv
  COMMAND pwsh -NoProfile -ExecutionPolicy Bypass -Command
          "Import-Module '${_psrex_module}' -Force; Invoke-ReXSetup -RepoRoot '${REXGLUE_ROOT}'"
  WORKING_DIRECTORY "${REXGLUE_ROOT}"
  COMMENT "Configuring ReXGlue developer environment..."
  VERBATIM USES_TERMINAL
)
set_target_properties(config-rexdevenv PROPERTIES FOLDER "sdk/utility")
