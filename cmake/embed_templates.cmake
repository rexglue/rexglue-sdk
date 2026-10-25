if (NOT DEFINED TEMPLATE_SOURCE_DIR OR NOT DEFINED TEMPLATE_OUTPUT_DIR)
  message(
    FATAL_ERROR "embed_templates.cmake requires -DTEMPLATE_SOURCE_DIR and -DTEMPLATE_OUTPUT_DIR"
  )
endif ()

include("${CMAKE_CURRENT_LIST_DIR}/embed_template_names.cmake")

file(GLOB_RECURSE INJA_TEMPLATE_FILES "${TEMPLATE_SOURCE_DIR}/*.inja")

# Names the embedded string constant of a template id, such as kInitCMakeLists.
function (template_constant_name canonical_id out_var)
  string(REGEX REPLACE "[/_-]+" ";" words "${canonical_id}")
  set(name "k")
  foreach (word ${words})
    if (word STREQUAL "dll"
        OR word STREQUAL "pch"
        OR word STREQUAL "ppc"
        OR word STREQUAL "toml"
    )
      string(TOUPPER "${word}" word)
    elseif (word STREQUAL "cmake")
      set(word "CMake")
    elseif (word STREQUAL "cmakelists")
      set(word "CMakeLists")
    else ()
      string(SUBSTRING "${word}" 0 1 first)
      string(SUBSTRING "${word}" 1 -1 rest)
      string(TOUPPER "${first}" first)
      set(word "${first}${rest}")
    endif ()
    string(APPEND name "${word}")
  endforeach ()
  set(${out_var}
      "${name}"
      PARENT_SCOPE
  )
endfunction ()

set(FORBIDDEN_DELIMITER ")__TMPL__\"")

set(EMBEDDED_INCLUDES "")
set(EMBEDDED_MAP_ENTRIES "")

foreach (template_file ${INJA_TEMPLATE_FILES})
  file(READ "${template_file}" TEMPLATE_CONTENT)

  string(FIND "${TEMPLATE_CONTENT}" "${FORBIDDEN_DELIMITER}" DELIMITER_POS)
  if (NOT DELIMITER_POS EQUAL -1)
    message(
      FATAL_ERROR "Template file contains forbidden raw string delimiter ')__TMPL__\"':\n"
                  "  ${template_file}\n" "Please remove or rename this sequence in the template."
    )
  endif ()

  file(RELATIVE_PATH REL_PATH "${TEMPLATE_SOURCE_DIR}" "${template_file}")
  embed_template_names("${REL_PATH}" CANONICAL_ID HEADER_NAME)

  template_constant_name("${CANONICAL_ID}" CONSTANT_NAME)

  set(HEADER_FILE "${TEMPLATE_OUTPUT_DIR}/${HEADER_NAME}")
  file(
    WRITE "${HEADER_FILE}"
    "// Auto-generated from resources/templates/${REL_PATH}. DO NOT EDIT\n"
    "#pragma once\n"
    "#include <string_view>\n\n"
    "namespace rex::codegen::embedded {\n"
    "inline constexpr std::string_view ${CONSTANT_NAME} = "
    "R\"__TMPL__(${TEMPLATE_CONTENT})__TMPL__\";\n"
    "}  // namespace rex::codegen::embedded\n"
  )

  string(APPEND EMBEDDED_INCLUDES "#include \"${HEADER_NAME}\"\n")
  string(APPEND EMBEDDED_MAP_ENTRIES "    {\"${CANONICAL_ID}\", embedded::${CONSTANT_NAME}},\n")
endforeach ()

file(
  WRITE "${TEMPLATE_OUTPUT_DIR}/embedded_templates.h"
  "// Auto-generated master template index. DO NOT EDIT\n"
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
