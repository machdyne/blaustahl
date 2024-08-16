cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(blaustahl)

add_executable(blaustahl
        blaustahl.c
        fram.c
        editor.c
        crypt.c
        cdc_out.c
        )

add_executable(blaustahl_cdconly
        blaustahl.c
        fram.c
        editor.c
        crypt.c
        cdc_out.c
        )

target_compile_definitions(blaustahl_cdconly PRIVATE CDCONLY=1)

target_sources(blaustahl PUBLIC
	${CMAKE_CURRENT_LIST_DIR}/blaustahl.c
	${CMAKE_CURRENT_LIST_DIR}/usb_descriptors.c
	)

target_sources(blaustahl_cdconly PUBLIC
	${CMAKE_CURRENT_LIST_DIR}/blaustahl.c
	${CMAKE_CURRENT_LIST_DIR}/usb_descriptors_cdconly.c
	)

target_include_directories(blaustahl PUBLIC
   ${CMAKE_CURRENT_LIST_DIR}
	/home/phil/work/rp2040/pico-sdk/lib/mbedtls/include
   )

target_include_directories(blaustahl_cdconly PUBLIC
   ${CMAKE_CURRENT_LIST_DIR}
   )

pico_sdk_init()

target_link_libraries(blaustahl PRIVATE pico_stdlib hardware_resets hardware_uart hardware_irq hardware_spi hardware_pwm pico_multicore pico_stdio_usb hardware_dma tinyusb_device pico_mbedtls pico_rand)

target_link_libraries(blaustahl_cdconly PRIVATE pico_stdlib hardware_resets hardware_uart hardware_irq hardware_spi hardware_pwm pico_multicore pico_stdio_usb hardware_dma tinyusb_device pico_mbedtls pico_rand)

pico_add_extra_outputs(blaustahl)
pico_add_extra_outputs(blaustahl_cdconly)
