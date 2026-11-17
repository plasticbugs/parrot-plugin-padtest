# Pad Test (PADTEST.CVP): what the cart's USB host sees of a game pad, as
# text on the TV, for finding out why a pad isn't read: the devices and
# their interfaces, each pad's reports (rate, gaps, kinds, the last one in
# hex, the buttons read from it), the Switch pads' start step by step, and
# a log. It runs the SDK's cvpad as the emulator plugins do. RESET: the menu.
#
#   -DCVPAD_TINYUSB_SDK=ON              the pico-sdk's TinyUSB 0.18 instead of
#                                       sdk/tinyusb, to compare (PADTEST18.CVP)
#
#   cmake -B build -G Ninja -DPICO_SDK_PATH=$HOME/pico-sdk
#   ninja -C build                      -> build/PADTEST.CVP
#
# Copy PADTEST.CVP into the card's /PLUGINS folder.
cmake_minimum_required(VERSION 3.13)

# the cart's SDK: the copy in sdk/ (sdk/README.md)
set(PARROTCART_SDK ${CMAKE_CURRENT_LIST_DIR}/sdk/firmware/revc)
set(PICO_BOARD_HEADER_DIRS ${PARROTCART_SDK}/cvcart_revc/boards)
set(PICO_BOARD cvcart_revc)
set(PICO_PLATFORM rp2350-arm-s)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# TinyUSB's log level (CFG_TUSB_DEBUG): 1 = errors and failed asserts, in the screen's log
set(LOG 1)
include(${PARROTCART_SDK}/sdk/cvpad_tinyusb.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/pico_sdk_import.cmake)

project(paddiag C CXX ASM)
pico_sdk_init()
include(${PARROTCART_SDK}/sdk/cvplugin.cmake)

# The same source makes the same file, whenever it's built: the compiler's
# __DATE__ and __TIME__ (the build date in the binary info) come from
# SOURCE_DATE_EPOCH, fixed here at the tested build's, 2026-11-15 19:28:57 UTC.
set(BUILD_EPOCH 1794770937)
set(CMAKE_C_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env SOURCE_DATE_EPOCH=${BUILD_EPOCH})
set(CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env SOURCE_DATE_EPOCH=${BUILD_EPOCH})

add_executable(paddiag src/paddiag.c)
# src/ first: its tusb_config.h (cvpad's, and the log)
target_include_directories(paddiag BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
cvkernel_use(paddiag)
cvpad_use(paddiag OWN_TUSB_CONFIG)
if (CVPAD_TINYUSB_SDK)
  set(PADDIAG_TUSB_DEFAULT "0.18")
  set(PADDIAG_FILE PADTEST18.CVP)
  set(PADDIAG_NAME "Pad Test 0.18")
  set(PADDIAG_ID parrotcart.padtest18)
else()
  set(PADDIAG_TUSB_DEFAULT "0.21+")
  set(PADDIAG_FILE PADTEST.CVP)
  set(PADDIAG_NAME "Pad Test")
  set(PADDIAG_ID parrotcart.padtest)
endif()
set(PADDIAG_TUSB ${PADDIAG_TUSB_DEFAULT} CACHE STRING "the TinyUSB's name on the screen")
target_compile_definitions(paddiag PRIVATE PADDIAG_TUSB="${PADDIAG_TUSB}")
# a panic (TinyUSB's host driver has some): on the screen, not a silent stop
target_compile_definitions(paddiag PRIVATE PICO_PANIC_FUNCTION=paddiag_panic)
# paddiag.c's "../../midihello/mailbox.h" and tusb_config.h's
# "../../sdk/cvpad_tusb/tusb_config.h" are paths in the cart's own
# repository. From this directory, two levels below the SDK's firmware/revc,
# both find the SDK's copies.
target_include_directories(paddiag PRIVATE ${PARROTCART_SDK}/sdk/cvpad_tusb)
pico_enable_stdio_usb(paddiag 0)
pico_enable_stdio_uart(paddiag 0)
cvcart_plugin(paddiag ID ${PADDIAG_ID} NAME ${PADDIAG_NAME} VERSION 0.1.2 BY "Parrot Cart"
              USES usb-host ICON src/paddiag_icon.txt FILE ${PADDIAG_FILE} README CART-README.md)
