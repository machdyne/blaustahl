cmake_minimum_required(VERSION 3.13)

# Requires the littlefs submodule to be checked out:
#   git submodule add https://github.com/littlefs-project/littlefs.git firmware/blaustahl/littlefs
#   git submodule update --init --recursive
# (only lfs.c/lfs.h/lfs_util.c/lfs_util.h are used -- the rest of that
# repo, tests/benches/scripts, is not part of the build)
#
# Also requires pico-sdk's own vendored mbedtls submodule (lib/mbedtls)
# to be checked out -- if `pico_mbedtls`/PSA headers aren't found, run
# `git submodule update --init` inside the pico-sdk checkout itself.
#
# Optional: te (real text editor) and ms (Machdyne Scheme), both
# reachable only from the CLI for now ("edit <filename>" / "scheme").
# Enabled by default -- flash has plenty of room, and both run in a
# blocking, self-contained mode (same as XMODEM already does) so they
# add no complexity to the core editor/menu/storage code paths, only
# to the CLI's dispatch table. Disable with -DBLAUSTAHL_ENABLE_APPS=OFF
# if you want to build without them at all.
#
# te.c and ms.c are real git submodules, each in its own subdirectory
# as usual; the stub/glue headers their embedded build modes expect
# (fs.h/te.h/zucker.h/ms_stdlib.h) live flat in this directory instead
# of nested under each submodule. This works with no special -I flags:
# both #include "fs.h" etc. by bare filename (after patching -- see
# below), which the C preprocessor first checks relative to te.c/ms.c's
# own directory (nothing there), then falls back to searching this
# target's normal include directories -- which already include this
# directory via target_include_directories() below, the same one every
# other source file in this project already uses.
#
#   git submodule add https://github.com/machdyne/te.git firmware/blaustahl/te
#   git submodule add https://github.com/machdyne/ms.git firmware/blaustahl/ms
#   git submodule update --init --recursive
#
# Each needs a small patch applied on top of the checkout -- see
# patches/*.patch for exactly what and why (each is a real, verified
# bug fix, not a style preference -- intended to be upstreamed once
# proven out here):
#
#   cd firmware/blaustahl
#   patch -p1 -d te < patches/te-embedded-null-check.patch
#   patch -p1 -d ms < patches/ms-lix-embedding-fixes.patch

option(BLAUSTAHL_ENABLE_APPS "Enable te/ms embedded apps (text editor + Scheme), CLI-only" ON)

include(pico_sdk_import.cmake)

project(blaustahl)

add_executable(blaustahl
        blaustahl.c
        fram.c
        editor.c
        menu.c
        browser.c
        view.c
        storage.c
        flash_storage.c
        vt100_input.c
        cli.c
        crypt.c
        xmodem.c
        srwp.c
        )

add_executable(blaustahl_cdconly
        blaustahl.c
        fram.c
        editor.c
        menu.c
        browser.c
        view.c
        storage.c
        flash_storage.c
        vt100_input.c
        cli.c
        crypt.c
        xmodem.c
        srwp.c
        )

target_compile_definitions(blaustahl_cdconly PRIVATE CDCONLY=1)

target_sources(blaustahl PUBLIC
	${CMAKE_CURRENT_LIST_DIR}/blaustahl.c
	${CMAKE_CURRENT_LIST_DIR}/usb_descriptors.c
	${CMAKE_CURRENT_LIST_DIR}/littlefs/lfs.c
	${CMAKE_CURRENT_LIST_DIR}/littlefs/lfs_util.c
	)

target_sources(blaustahl_cdconly PUBLIC
	${CMAKE_CURRENT_LIST_DIR}/blaustahl.c
	${CMAKE_CURRENT_LIST_DIR}/usb_descriptors_cdconly.c
	${CMAKE_CURRENT_LIST_DIR}/littlefs/lfs.c
	${CMAKE_CURRENT_LIST_DIR}/littlefs/lfs_util.c
	)

target_include_directories(blaustahl PUBLIC
   ${CMAKE_CURRENT_LIST_DIR}
   ${CMAKE_CURRENT_LIST_DIR}/littlefs
   ${PICO_SDK_PATH}/lib/mbedtls/include
   )

target_include_directories(blaustahl_cdconly PUBLIC
   ${CMAKE_CURRENT_LIST_DIR}
   ${CMAKE_CURRENT_LIST_DIR}/littlefs
   ${PICO_SDK_PATH}/lib/mbedtls/include
   )

if(BLAUSTAHL_ENABLE_APPS)

	# not adjustable via BLAUSTAHL_ENABLE_APPS itself -- these are
	# separate CMake cache variables (-DMS_HEAP_CELLS=N etc) so the
	# sizes can be tuned without editing this file. 3000 cells is a
	# considered starting point (real headroom above ms's own ~700-750
	# cell boot baseline for actual programs) rather than a measured
	# safe maximum -- ms_init()/ms_deinit() malloc/free this on demand,
	# so it's a transient cost only while "scheme" is actually running,
	# not a permanent reservation. Revisit both numbers once real
	# hardware free-RAM measurement is available.
	set(MS_HEAP_CELLS "3000" CACHE STRING "Machdyne Scheme heap size, in cells")
	set(MS_PROTECT_STACK_SLOTS "512" CACHE STRING "Machdyne Scheme protect stack size, in slots")

	target_sources(blaustahl PUBLIC
		${CMAKE_CURRENT_LIST_DIR}/te/te.c
		${CMAKE_CURRENT_LIST_DIR}/ms/ms.c
		${CMAKE_CURRENT_LIST_DIR}/te_glue.c
		${CMAKE_CURRENT_LIST_DIR}/ms_glue.c
		)
	target_sources(blaustahl_cdconly PUBLIC
		${CMAKE_CURRENT_LIST_DIR}/te/te.c
		${CMAKE_CURRENT_LIST_DIR}/ms/ms.c
		${CMAKE_CURRENT_LIST_DIR}/te_glue.c
		${CMAKE_CURRENT_LIST_DIR}/ms_glue.c
		)

	# te.c and ms.c each need their own build-mode define --
	# set_source_files_properties is the tool for per-file flags
	# within a single target, since these can't just go on the whole
	# target like everything else here. No -I needed for either: their
	# #include "fs.h" etc. (see patches/) fall back to this target's
	# normal include directories after checking their own submodule
	# directory first and finding nothing there -- and this directory
	# (where fs.h/te.h/zucker.h/ms_stdlib.h actually live) is already
	# one of those, via target_include_directories() above.
	set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/te/te.c
		PROPERTIES COMPILE_OPTIONS "-DEMBEDDED")
	set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/ms/ms.c
		PROPERTIES COMPILE_OPTIONS
		"-DLIX;-DMS_HEAP_SIZE=${MS_HEAP_CELLS};-DMS_PROTECT_STACK_SIZE=${MS_PROTECT_STACK_SLOTS}")

	target_compile_definitions(blaustahl PUBLIC BLAUSTAHL_APPS_ENABLED)
	target_compile_definitions(blaustahl_cdconly PUBLIC BLAUSTAHL_APPS_ENABLED)

endif()

# If the core0/core1 lockout handshake inside flash_safe_execute() ever
# fails to establish (multicore_lockout_ready() returns false), the
# pico-sdk's default behavior is `assert(false)` -- which halts the CPU
# outright rather than returning an error. That turns any lockout
# coordination problem into a hard, unrecoverable freeze (as opposed to
# flash_storage.c's rp2040_erase()/rp2040_prog() cleanly seeing a
# non-PICO_OK return and reporting LFS_ERR_IO, which the CLI already
# handles gracefully as "FORMAT FAILED"). Disabling the assert doesn't
# fix an underlying lockout problem if one exists, but it's the
# difference between a diagnosable error and BOOTSEL being the only way
# out -- worth keeping permanently, not just while debugging. Set
# globally, before pico_sdk_init() runs, so it reaches whichever internal
# SDK target actually compiles flash.c, regardless of pico_flash's own
# target type (which turned out to be INTERFACE, not something
# target_compile_definitions can target directly with PUBLIC/PRIVATE).

# This board has 4MB of flash, but nothing in this build tree currently
# declares that to the SDK (no PICO_BOARD/PICO_BOARD_HEADER_DIRS is set
# in this file, so if there's no dedicated board header elsewhere in the
# project, the SDK falls back to the generic "pico" board's 2MB default
# via boards/pico.h). That mismatch is a real, confirmed problem: it
# silently let this build compute flash addresses in the 2-4MB range
# that the SDK considers out of range, which is a strong candidate for
# undefined/hanging behavior in flash_range_erase()/flash_range_program()
# rather than a clean error.
#
# The proper, permanent fix is to declare this in an actual Blaustahl
# board header (boards/blaustahl.h), the same way pico2.h declares
# PICO_FLASH_SIZE_BYTES as (4 * 1024 * 1024) for its board -- if one
# already exists somewhere in this project and just has the wrong
# value (or is missing this line entirely), fix it there and this
# override becomes redundant, not wrong, since board headers guard
# with #ifndef and won't clobber an externally-supplied value.
# Overriding here as well guarantees correctness regardless of where
# (or whether) that header exists, and reaches every SDK-internal
# compilation unit that reads this value, not just our own target.
#
# Using the literal integer (4194304) rather than "(4 * 1024 * 1024)"
# deliberately -- CMake misparses parenthesized compile-definition
# values as function-style macro syntax and silently drops them,
# which is safe inside a real header's #define but not on a generated
# command line.
add_compile_definitions(PICO_FLASH_SIZE_BYTES=4194304)

add_compile_definitions(PICO_FLASH_ASSERT_ON_UNSAFE=0)

pico_sdk_init()

pico_enable_stdio_usb(blaustahl 1)
pico_enable_stdio_uart(blaustahl 0)
pico_enable_stdio_usb(blaustahl_cdconly 1)
pico_enable_stdio_uart(blaustahl_cdconly 0)

target_compile_definitions(blaustahl PUBLIC
	PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64
	LFS_NO_DEBUG
	LFS_NO_WARN
	LFS_NO_ERROR
	LFS_NO_TRACE
	MBEDTLS_CONFIG_FILE="mbedtls_config.h"
	)
target_compile_definitions(blaustahl_cdconly PUBLIC
	PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64
	LFS_NO_DEBUG
	LFS_NO_WARN
	LFS_NO_ERROR
	LFS_NO_TRACE
	MBEDTLS_CONFIG_FILE="mbedtls_config.h"
	)

target_link_libraries(blaustahl PRIVATE pico_stdlib hardware_resets hardware_uart hardware_irq hardware_spi hardware_pwm pico_multicore pico_stdio_usb hardware_dma hardware_flash pico_flash pico_mbedtls pico_rand pico_unique_id tinyusb_device)
target_link_libraries(blaustahl_cdconly PRIVATE pico_stdlib hardware_resets hardware_uart hardware_irq hardware_spi hardware_pwm pico_multicore pico_stdio_usb hardware_dma hardware_flash pico_flash pico_mbedtls pico_rand pico_unique_id tinyusb_device)

pico_add_extra_outputs(blaustahl)
pico_add_extra_outputs(blaustahl_cdconly)
