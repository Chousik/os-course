add_library(
    libvtsh
    STATIC
    vtsh.c
        ../loaders/io_load.c
)

target_compile_definitions(
    libvtsh
    PRIVATE
    VTSH_NO_STANDALONE=1
)

target_include_directories(
    libvtsh
    PUBLIC
    .
)
