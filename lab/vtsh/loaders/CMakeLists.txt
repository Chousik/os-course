set(EMA_REPLACE_COMMON
    ${CMAKE_CURRENT_SOURCE_DIR}/ema_replace_common.c
)

add_executable(
    loader_ema_replace_orig
    ema_replace_posix.c
    ${EMA_REPLACE_COMMON}
)

target_include_directories(loader_ema_replace_orig
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}
)

add_executable(
    loader_ema_replace_vtpc
    ema_replace_vtpc.c
    ${EMA_REPLACE_COMMON}
)

target_include_directories(loader_ema_replace_vtpc
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(loader_ema_replace_vtpc
    PRIVATE
        vtpc
)

set_target_properties(
    loader_ema_replace_orig
    loader_ema_replace_vtpc
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)
