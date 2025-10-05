add_executable(
    loader_cpu_dedup
    cpu_dedup.c
)

add_executable(
    loader_ema_replace_str
    ema_replace_str.c
)

add_executable(
    loader_io_rw
    io_load.c
)

set_target_properties(loader_cpu_dedup loader_ema_replace_str loader_io_rw
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)
