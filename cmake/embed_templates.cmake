# cmake/embed_templates.cmake
# Converts resources/templates/**/*.inja into C++ headers with constexpr string_view.
#
# Generates:
#   - One header per .inja file (e.g., cli_cmakelists.inja.h)
#   - A master embedded_templates.h with a map of canonical ID -> content
#
# Output goes to ${CMAKE_CURRENT_BINARY_DIR}/generated/templates/
#
# Note: Runs at configure time. Editing .inja files requires re-running
# CMake configure to pick up changes.

set(TEMPLATE_SOURCE_DIR "${REXGLUE_ROOT}/resources/templates")
set(TEMPLATE_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/templates")

file(GLOB_RECURSE INJA_TEMPLATE_FILES "${TEMPLATE_SOURCE_DIR}/*.inja")

# Validate delimiter safety
set(FORBIDDEN_DELIMITER ")__TMPL__\"")

set(EMBEDDED_INCLUDES "")
set(EMBEDDED_MAP_ENTRIES "")

foreach(TEMPLATE_FILE ${INJA_TEMPLATE_FILES})
    # Read the template content
    file(READ "${TEMPLATE_FILE}" TEMPLATE_CONTENT)

    # Check for delimiter collision
    string(FIND "${TEMPLATE_CONTENT}" "${FORBIDDEN_DELIMITER}" DELIMITER_POS)
    if(NOT DELIMITER_POS EQUAL -1)
        message(FATAL_ERROR
            "Template file contains forbidden raw string delimiter ')__TMPL__\"':\n"
            "  ${TEMPLATE_FILE}\n"
            "Please remove or rename this sequence in the template.")
    endif()

    # Compute canonical ID: relative path without .inja extension
    file(RELATIVE_PATH REL_PATH "${TEMPLATE_SOURCE_DIR}" "${TEMPLATE_FILE}")
    string(REGEX REPLACE "\\.inja$" "" CANONICAL_ID "${REL_PATH}")

    # Compute C++ variable name: replace / and - with _
    string(REPLACE "/" "_" VAR_NAME "${CANONICAL_ID}")
    string(REPLACE "-" "_" VAR_NAME "${VAR_NAME}")

    # Generate individual header
    set(HEADER_FILE "${TEMPLATE_OUTPUT_DIR}/${VAR_NAME}.inja.h")
    file(WRITE "${HEADER_FILE}"
        "// Auto-generated from resources/templates/${REL_PATH} -- DO NOT EDIT\n"
        "#pragma once\n"
        "#include <string_view>\n\n"
        "namespace rex::codegen::embedded {\n"
        "inline constexpr std::string_view ${VAR_NAME} = R\"__TMPL__(\n"
        "${TEMPLATE_CONTENT}"
        ")__TMPL__\";\n"
        "}  // namespace rex::codegen::embedded\n"
    )

    # Accumulate for master header
    string(APPEND EMBEDDED_INCLUDES "#include \"${VAR_NAME}.inja.h\"\n")
    string(APPEND EMBEDDED_MAP_ENTRIES "    {\"${CANONICAL_ID}\", embedded::${VAR_NAME}},\n")
endforeach()

# Generate master header
file(WRITE "${TEMPLATE_OUTPUT_DIR}/embedded_templates.h"
    "// Auto-generated master template index -- DO NOT EDIT\n"
    "#pragma once\n\n"
    "#include <string_view>\n"
    "#include <unordered_map>\n"
    "#include <string>\n\n"
    "${EMBEDDED_INCLUDES}\n"
    "namespace rex::codegen {\n\n"
    "inline const std::unordered_map<std::string, std::string_view>& embeddedTemplates() {\n"
    "  static const std::unordered_map<std::string, std::string_view> map = {\n"
    "${EMBEDDED_MAP_ENTRIES}"
    "  };\n"
    "  return map;\n"
    "}\n\n"
    "}  // namespace rex::codegen\n"
)
