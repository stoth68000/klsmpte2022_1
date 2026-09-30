cmake_minimum_required(VERSION 3.16)

project(klsmpte2022_1
    VERSION 0.1.0
    DESCRIPTION "Standalone C SMPTE ST 2022-1 FEC library for RTP MPEG-TS"
    LANGUAGES C)

option(SMPTE2022_BUILD_TESTS "Build smpte2022 tests" ON)
option(SMPTE2022_BUILD_EXAMPLES "Build smpte2022 examples" ON)
option(SMPTE2022_BUILD_DOCS "Add Doxygen documentation target" ON)

add_library(klsmpte2022_1
    src/fec.c)

add_library(klsmpte2022_1::klsmpte2022_1 ALIAS klsmpte2022_1)

set_target_properties(klsmpte2022_1 PROPERTIES
    OUTPUT_NAME klsmpte2022_1)

target_include_directories(klsmpte2022_1
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(klsmpte2022_1 PUBLIC c_std_99)

if(MSVC)
    target_compile_options(klsmpte2022_1 PRIVATE /W4)
else()
    target_compile_options(klsmpte2022_1 PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)

install(TARGETS klsmpte2022_1
    EXPORT klsmpte2022_1Targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})

install(EXPORT klsmpte2022_1Targets
    NAMESPACE klsmpte2022_1::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/klsmpte2022_1)

include(CMakePackageConfigHelpers)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/klsmpte2022_1ConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/klsmpte2022_1Config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/klsmpte2022_1Config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/klsmpte2022_1)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/klsmpte2022_1Config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/klsmpte2022_1ConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/klsmpte2022_1)

if(SMPTE2022_BUILD_DOCS)
    find_package(Doxygen QUIET)
    if(DOXYGEN_FOUND)
        set(SMPTE2022_DOXYGEN_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/docs")
        set(SMPTE2022_DOXYGEN_INPUT
            "${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/README.md")
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Doxyfile.in"
            "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            @ONLY)
        add_custom_target(docs
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Generating API documentation with Doxygen"
            VERBATIM)
    else()
        message(STATUS "Doxygen not found; docs target will not be available")
    endif()
endif()

if(SMPTE2022_BUILD_TESTS)
    enable_testing()
    add_executable(klsmpte2022-1_fec_test tests/test_fec.c)
    target_link_libraries(klsmpte2022-1_fec_test PRIVATE klsmpte2022_1::klsmpte2022_1)
    add_test(NAME klsmpte2022-1_fec_test COMMAND klsmpte2022-1_fec_test)
endif()

if(SMPTE2022_BUILD_EXAMPLES)
    add_executable(klsmpte2022-1_generate_fec examples/generate_fec.c)
    target_link_libraries(klsmpte2022-1_generate_fec PRIVATE klsmpte2022_1::klsmpte2022_1)

    add_executable(klsmpte2022-1_transmit_demo examples/transmit_demo.c)
    target_link_libraries(klsmpte2022-1_transmit_demo PRIVATE klsmpte2022_1::klsmpte2022_1)

    add_executable(klsmpte2022-1_receive_demo examples/receive_demo.c)
    target_link_libraries(klsmpte2022-1_receive_demo PRIVATE klsmpte2022_1::klsmpte2022_1)
endif()
