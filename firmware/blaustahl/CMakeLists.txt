cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(blaustahl)

add_executable(blaustahl
        blaustahl.c
        fram.c
        editor.c
        cdc_out.c
        )

target_sources(blaustahl PUBLIC
	${CMAKE_CURRENT_LIST_DIR}/blaustahl.c
	${CMAKE_CURRENT_LIST_DIR}/usb_descriptors.c
	)

target_include_directories(blaustahl PUBLIC
   ${CMAKE_CURRENT_LIST_DIR}
   )

pico_sdk_init()

target_link_libraries(blaustahl PRIVATE pico_stdlib hardware_resets hardware_uart hardware_irq hardware_spi hardware_pwm pico_multicore pico_stdio_usb hardware_dma tinyusb_device)

pico_add_extra_outputs(blaustahl)
