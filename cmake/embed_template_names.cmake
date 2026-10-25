# Names the template id and the generated header of a template file, relative to the template
# folder. Shared by the codegen target and embed_templates.cmake.
function (embed_template_names rel_path id_var header_var)
  string(REGEX REPLACE "\\.inja$" "" id "${rel_path}")
  string(REPLACE "/" "_" name "${id}")
  string(REPLACE "-" "_" name "${name}")
  string(REGEX REPLACE "_+" "_" name "${name}")
  set(${id_var}
      "${id}"
      PARENT_SCOPE
  )
  set(${header_var}
      "${name}.inja.h"
      PARENT_SCOPE
  )
endfunction ()
