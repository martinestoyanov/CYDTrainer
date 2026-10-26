idf_component_register(
    SRCS "main.cpp" "usb_net.cpp" "web_server.cpp" "hw_task.cpp" "board.cpp" "display.cpp"
    INCLUDE_DIRS "."
)
# Packs ../data into the "storage" partition and flashes it with the app
littlefs_create_partition_image(storage ../data FLASH_IN_PROJECT)
