add_library(
    vtpc
    SHARED
    vtpc.c
)

target_include_directories(vtpc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(vtpc PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)
