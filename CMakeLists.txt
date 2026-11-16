cmake_minimum_required(VERSION 3.20)
project(testhdr VERSION 0.5.0 LANGUAGES C CXX)

include(GNUInstallDirs)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(GLSLC_EXECUTABLE glslc REQUIRED)
find_program(WAYLAND_SCANNER_EXECUTABLE wayland-scanner REQUIRED)

pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(LCMS2 REQUIRED IMPORTED_TARGET lcms2)

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")

function(compile_spv_header SHADER_FILE VAR_NAME OUT_HEADER)
    set(SPV_FILE "${GEN_DIR}/${VAR_NAME}.spv")
    add_custom_command(
        OUTPUT "${SPV_FILE}" "${OUT_HEADER}"
        COMMAND "${GLSLC_EXECUTABLE}" -O "${SHADER_FILE}" -o "${SPV_FILE}"
        COMMAND "${Python3_EXECUTABLE}" -c
            "import sys; d=open(sys.argv[1],'rb').read(); open(sys.argv[2],'w').write('#pragma once\\n#include <cstdint>\\ninline constexpr uint8_t ${VAR_NAME}[] = {' + ','.join(str(b) for b in d) + '};\\n')"
            "${SPV_FILE}" "${OUT_HEADER}"
        DEPENDS "${SHADER_FILE}"
        COMMENT "Compiling SPIR-V shader ${SHADER_FILE}"
        VERBATIM
    )
endfunction()

compile_spv_header("${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert" "fullscreen_vert_spv" "${GEN_DIR}/fullscreen_vert_spv.h")
compile_spv_header("${CMAKE_CURRENT_SOURCE_DIR}/shaders/gradient.frag" "gradient_frag_spv" "${GEN_DIR}/gradient_frag_spv.h")

set(CM_XML "/usr/share/wayland-protocols/staging/color-management/color-management-v1.xml")
set(CM_HEADER "${GEN_DIR}/color-management-v1-client-protocol.h")
set(CM_CODE "${GEN_DIR}/color-management-v1-protocol.c")

add_custom_command(
    OUTPUT "${CM_HEADER}"
    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${CM_XML}" "${CM_HEADER}"
    DEPENDS "${CM_XML}"
    COMMENT "Generating Wayland color-management-v1 client header"
    VERBATIM
)
add_custom_command(
    OUTPUT "${CM_CODE}"
    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${CM_XML}" "${CM_CODE}"
    DEPENDS "${CM_XML}"
    COMMENT "Generating Wayland color-management-v1 protocol code"
    VERBATIM
)

add_custom_target(testhdr_codegen DEPENDS
    "${GEN_DIR}/fullscreen_vert_spv.h"
    "${GEN_DIR}/gradient_frag_spv.h"
    "${CM_HEADER}"
    "${CM_CODE}"
)

add_executable(testhdr
    src/main.cpp
    src/display_limits.cpp
    src/edid.cpp
    src/icc_gen.cpp
    src/overlay.cpp
    "${CM_CODE}"
)
add_dependencies(testhdr testhdr_codegen)
target_include_directories(testhdr PRIVATE src "${GEN_DIR}")
target_compile_definitions(testhdr PRIVATE TESTHDR_VERSION="${PROJECT_VERSION}")
target_link_libraries(testhdr PRIVATE PkgConfig::SDL3 PkgConfig::PANGOCAIRO PkgConfig::WAYLAND_CLIENT PkgConfig::LCMS2 m)
target_compile_options(testhdr PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>)

install(TARGETS testhdr RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(PROGRAMS bin/monique-set-cm DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES pkgbuild/testhdr.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES pkgbuild/testhdr.svg DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")

enable_testing()
add_executable(testhdr_tests
    tests/test_math.cpp
    src/display_limits.cpp
    src/edid.cpp
    src/icc_gen.cpp
    src/overlay.cpp
    "${CM_CODE}"
)
add_dependencies(testhdr_tests testhdr_codegen)
target_include_directories(testhdr_tests PRIVATE src "${GEN_DIR}")
target_link_libraries(testhdr_tests PRIVATE PkgConfig::PANGOCAIRO PkgConfig::WAYLAND_CLIENT PkgConfig::LCMS2 m)
target_compile_options(testhdr_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>)
add_test(NAME testhdr_math_edid COMMAND testhdr_tests)
