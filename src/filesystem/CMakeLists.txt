add_library(rex-filesystem SHARED
    container_provider.cpp
    file_system.cpp
    host_provider.cpp
    module.cpp
    node.cpp
    node_file.cpp
    null_provider.cpp
    provider.cpp
    wildcard.cpp
)
add_library(rex::filesystem ALIAS rex-filesystem)

if(WIN32)
    target_sources(rex-filesystem PRIVATE host_file_win.cpp)
elseif(UNIX)
    target_sources(rex-filesystem PRIVATE host_file_posix.cpp)
endif()

set_target_properties(rex-filesystem PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
rexglue_add_version_resource(rex-filesystem DESCRIPTION "ReXGlue filesystem")

target_include_directories(rex-filesystem PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(rex-filesystem PRIVATE rex-core)
