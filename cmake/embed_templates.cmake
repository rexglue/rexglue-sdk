if(NOT DEFINED TEMPLATE_SOURCE_DIR OR NOT DEFINED TEMPLATE_OUTPUT_DIR)
    message(FATAL_ERROR "embed_templates.cmake requires -DTEMPLATE_SOURCE_DIR and -DTEMPLATE_OUTPUT_DIR")
endif()

file(GLOB_RECURSE INJA_TEMPLATE_FILES "${TEMPLATE_SOURCE_DIR}/*.inja")

function(template_constant_name CANONICAL_ID OUT_VAR)
    string(REGEX REPLACE "[/_-]+" ";" WORDS "${CANONICAL_ID}")
    set(NAME "k")
    foreach(WORD ${WORDS})
        if(WORD STREQUAL "dll" OR WORD STREQUAL "pch" OR WORD STREQUAL "ppc" OR WORD STREQUAL "toml")
            string(TOUPPER "${WORD}" WORD)
        elseif(WORD STREQUAL "cmake")
            set(WORD "CMake")
        elseif(WORD STREQUAL "cmakelists")
            set(WORD "CMakeLists")
        else()
            string(SUBSTRING "${WORD}" 0 1 FIRST)
            string(SUBSTRING "${WORD}" 1 -1 REST)
            string(TOUPPER "${FIRST}" FIRST)
            set(WORD "${FIRST}${REST}")
        endif()
        string(APPEND NAME "${WORD}")
    endforeach()
    set(${OUT_VAR} "${NAME}" PARENT_SCOPE)
endfunction()

set(FORBIDDEN_DELIMITER ")__TMPL__\"")

set(EMBEDDED_INCLUDES "")
set(EMBEDDED_MAP_ENTRIES "")

foreach(TEMPLATE_FILE ${INJA_TEMPLATE_FILES})
    file(READ "${TEMPLATE_FILE}" TEMPLATE_CONTENT)

    string(FIND "${TEMPLATE_CONTENT}" "${FORBIDDEN_DELIMITER}" DELIMITER_POS)
    if(NOT DELIMITER_POS EQUAL -1)
        message(FATAL_ERROR
            "Template file contains forbidden raw string delimiter ')__TMPL__\"':\n"
            "  ${TEMPLATE_FILE}\n"
            "Please remove or rename this sequence in the template.")
    endif()

    file(RELATIVE_PATH REL_PATH "${TEMPLATE_SOURCE_DIR}" "${TEMPLATE_FILE}")
    string(REGEX REPLACE "\\.inja$" "" CANONICAL_ID "${REL_PATH}")

    string(REPLACE "/" "_" VAR_NAME "${CANONICAL_ID}")
    string(REPLACE "-" "_" VAR_NAME "${VAR_NAME}")
    string(REGEX REPLACE "_+" "_" VAR_NAME "${VAR_NAME}")

    template_constant_name("${CANONICAL_ID}" CONSTANT_NAME)

    set(HEADER_FILE "${TEMPLATE_OUTPUT_DIR}/${VAR_NAME}.inja.h")
    file(WRITE "${HEADER_FILE}"
        "// Auto-generated from resources/templates/${REL_PATH} -- DO NOT EDIT\n"
        "#pragma once\n"
        "#include <string_view>\n\n"
        "namespace rex::codegen::embedded {\n"
        "inline constexpr std::string_view ${CONSTANT_NAME} = R\"__TMPL__(${TEMPLATE_CONTENT})__TMPL__\";\n"
        "}  // namespace rex::codegen::embedded\n"
    )

    string(APPEND EMBEDDED_INCLUDES "#include \"${VAR_NAME}.inja.h\"\n")
    string(APPEND EMBEDDED_MAP_ENTRIES "    {\"${CANONICAL_ID}\", embedded::${CONSTANT_NAME}},\n")
endforeach()

file(WRITE "${TEMPLATE_OUTPUT_DIR}/embedded_templates.h"
    "// Auto-generated master template index -- DO NOT EDIT\n"
    "#pragma once\n\n"
    "#include <string_view>\n"
    "#include <unordered_map>\n"
    "#include <string>\n\n"
    "${EMBEDDED_INCLUDES}\n"
    "namespace rex::codegen {\n\n"
    "inline const std::unordered_map<std::string, std::string_view>& EmbeddedTemplates() {\n"
    "  static const std::unordered_map<std::string, std::string_view> kTemplates = {\n"
    "${EMBEDDED_MAP_ENTRIES}"
    "  };\n"
    "  return kTemplates;\n"
    "}\n\n"
    "}  // namespace rex::codegen\n"
)
