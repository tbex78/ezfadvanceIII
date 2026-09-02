cmake_minimum_required(VERSION 3.20)
project(ezfadvanceIII VERSION 0.18.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

find_package(unofficial-libusb CONFIG QUIET)
if(TARGET unofficial-libusb::libusb)
    set(EZF_LIBUSB_TARGET unofficial-libusb::libusb)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
    endif()
endif()

if(EZF_LIBUSB_TARGET)
    # Selected from a CMake package above.
elseif(TARGET PkgConfig::LIBUSB)
    set(EZF_LIBUSB_TARGET PkgConfig::LIBUSB)
else()
    find_path(LIBUSB_INCLUDE_DIR
        NAMES libusb.h
        PATH_SUFFIXES libusb-1.0)
    find_library(LIBUSB_LIBRARY
        NAMES usb-1.0 libusb-1.0 libusb)
    if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
        message(FATAL_ERROR
            "libusb 1.0 was not found. Set LIBUSB_INCLUDE_DIR and "
            "LIBUSB_LIBRARY, install libusb through vcpkg/MSYS2, or provide "
            "a pkg-config libusb-1.0 package.")
    endif()
    add_library(ezf_libusb UNKNOWN IMPORTED)
    set_target_properties(ezf_libusb PROPERTIES
        IMPORTED_LOCATION "${LIBUSB_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBUSB_INCLUDE_DIR}")
    set(EZF_LIBUSB_TARGET ezf_libusb)
endif()

set(EZF_TRANSPORT_SOURCES
    src/usb_device.cpp
    src/protocol.cpp
    src/platform.cpp)
set(EZF_WRITER_SOURCES
    ${EZF_TRANSPORT_SOURCES}
    src/progress_bar.cpp
    src/card_write_presenter.cpp
    src/flash_window_selector.cpp
    src/save_bank_access.cpp
    src/save_bank_cleaner.cpp
    src/card_writer.cpp
    src/libusb_writer_backend.cpp
    src/cartridge_format.cpp
    src/cartridge_image_builder.cpp
    src/eeprom_mapping.cpp
    src/rom_analyzer.cpp
    src/verification_policy.cpp
    src/verification_session.cpp
    src/writer_options.cpp)
set(EZF_READ_ONLY_SOURCES
    ${EZF_TRANSPORT_SOURCES}
    src/cartridge_format.cpp
    src/ez3_catalog.cpp
    src/ez3_catalog_reader.cpp
    src/read_only_cartridge.cpp
    src/read_session_transition.cpp)

function(ezf_add_tool target main_source)
    add_executable(${target} ${main_source} ${ARGN})
    target_include_directories(${target} PRIVATE include)
    target_link_libraries(${target} PRIVATE ${EZF_LIBUSB_TARGET})
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

ezf_add_tool(ezfadvanceIII_multirom_writer
    ezfadvanceIII_multirom_writer.cpp ${EZF_WRITER_SOURCES})
ezf_add_tool(ezfadvanceIII_card_reader
    ezfadvanceIII_card_reader.cpp ${EZF_READ_ONLY_SOURCES}
    src/card_reader_options.cpp src/progress_bar.cpp)
ezf_add_tool(ezfadvanceIII_save_reader
    ezfadvanceIII_save_reader.cpp ${EZF_READ_ONLY_SOURCES}
    src/save_catalog_analyzer.cpp
    src/save_bank_access.cpp src/save_bank_cleaner.cpp
    src/save_memory_reader.cpp src/save_memory_writer.cpp
    src/save_bank_selector.cpp)
ezf_add_tool(ezfadvanceIII_wipe_card
    ezfadvanceIII_wipe_card.cpp ${EZF_TRANSPORT_SOURCES}
    src/flash_window_selector.cpp src/save_bank_access.cpp
    src/save_bank_cleaner.cpp)

include(CTest)
if(BUILD_TESTING)
    function(ezf_add_test name)
        add_executable(${name} ${ARGN})
        target_include_directories(${name} PRIVATE include)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()
    function(ezf_add_usb_test name)
        ezf_add_test(${name} ${ARGN})
        target_link_libraries(${name} PRIVATE ${EZF_LIBUSB_TARGET})
    endfunction()

    ezf_add_test(arm_branch_test tests/arm_branch_test.cpp src/cartridge_format.cpp)
    ezf_add_test(cartridge_format_test tests/cartridge_format_test.cpp src/cartridge_format.cpp)
    ezf_add_test(cartridge_image_builder_test tests/cartridge_image_builder_test.cpp src/cartridge_image_builder.cpp src/cartridge_format.cpp)
    ezf_add_test(card_writer_test tests/card_writer_test.cpp src/card_writer.cpp src/verification_policy.cpp)
    ezf_add_test(card_write_presenter_test tests/card_write_presenter_test.cpp src/card_write_presenter.cpp)
    ezf_add_test(ez3_catalog_test tests/ez3_catalog_test.cpp src/ez3_catalog.cpp src/cartridge_format.cpp)
    ezf_add_usb_test(ez3_catalog_reader_test tests/ez3_catalog_reader_test.cpp src/ez3_catalog_reader.cpp src/ez3_catalog.cpp src/read_only_cartridge.cpp src/read_session_transition.cpp src/cartridge_format.cpp src/protocol.cpp src/usb_device.cpp)
    ezf_add_test(eeprom_mapping_test tests/eeprom_mapping_test.cpp src/eeprom_mapping.cpp)
    ezf_add_test(rom_analyzer_test tests/rom_analyzer_test.cpp src/rom_analyzer.cpp src/cartridge_format.cpp src/eeprom_mapping.cpp)
    ezf_add_test(card_reader_options_test tests/card_reader_options_test.cpp src/card_reader_options.cpp)
    ezf_add_usb_test(protocol_test tests/protocol_test.cpp src/protocol.cpp src/usb_device.cpp)
    ezf_add_usb_test(flash_window_selector_test tests/flash_window_selector_test.cpp src/flash_window_selector.cpp src/protocol.cpp src/usb_device.cpp)
    ezf_add_usb_test(usb_device_test tests/usb_device_test.cpp src/usb_device.cpp)
    ezf_add_test(platform_test tests/platform_test.cpp src/platform.cpp)
    ezf_add_test(progress_bar_test tests/progress_bar_test.cpp src/progress_bar.cpp src/platform.cpp)
    ezf_add_test(read_session_transition_test tests/read_session_transition_test.cpp src/read_session_transition.cpp)
    ezf_add_usb_test(official_cartridge_read_test tests/official_cartridge_read_test.cpp src/ez3_catalog_reader.cpp src/ez3_catalog.cpp src/read_only_cartridge.cpp src/read_session_transition.cpp src/cartridge_format.cpp src/protocol.cpp src/usb_device.cpp)
    ezf_add_test(verification_policy_test tests/verification_policy_test.cpp src/verification_policy.cpp)
    foreach(geometry exact_8mib partial_12mib exact_16mib tiny_tail_above_16mib partial_20mib exact_24mib partial_28mib exact_32mib partial_first_window)
        ezf_add_usb_test(verification_${geometry}_test tests/verification_${geometry}_test.cpp src/verification_session.cpp src/protocol.cpp src/usb_device.cpp)
    endforeach()
    ezf_add_test(writer_options_test tests/writer_options_test.cpp src/writer_options.cpp)
    ezf_add_test(version_test tests/version_test.cpp)
    ezf_add_test(save_selection_test tests/save_selection_test.cpp)
    ezf_add_test(save_bank_selector_test tests/save_bank_selector_test.cpp src/save_bank_selector.cpp)
    ezf_add_usb_test(save_catalog_analyzer_test tests/save_catalog_analyzer_test.cpp src/save_catalog_analyzer.cpp src/ez3_catalog_reader.cpp src/ez3_catalog.cpp src/read_only_cartridge.cpp src/read_session_transition.cpp src/cartridge_format.cpp src/protocol.cpp src/usb_device.cpp)
    ezf_add_usb_test(save_memory_test tests/save_memory_test.cpp src/save_bank_access.cpp src/save_memory_reader.cpp src/save_memory_writer.cpp src/protocol.cpp src/usb_device.cpp)
    ezf_add_usb_test(save_bank_cleaner_test tests/save_bank_cleaner_test.cpp src/save_bank_access.cpp src/save_bank_cleaner.cpp src/protocol.cpp src/usb_device.cpp)
endif()
