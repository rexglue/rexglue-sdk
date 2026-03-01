# ReXGlue 

add_executable(rexglue
    main.cpp
    commands/codegen_command.cpp
    commands/init_command.cpp
    commands/migrate_command.cpp
    commands/test_recompiler.cpp
)

target_include_directories(rexglue
    PRIVATE
        ${REXGLUE_ROOT}/src
        ${REXGLUE_ROOT}/thirdparty/disasm
)

target_link_libraries(rexglue
    PRIVATE
        rexcodegen
)

add_executable(rex::rexglue ALIAS rexglue)
