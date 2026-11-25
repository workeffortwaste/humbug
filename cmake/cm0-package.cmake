#
# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
#
# SPDX-License-Identifier: MIT
# Packaging and install layout for CardputerZero Debian builds.

include(GNUInstallDirs)

set(APP_DISPLAY_NAME
    "Humbug"
    CACHE STRING "Human-readable application name used by launchers and package filename")
set(APP_DEBIAN_REVISION
    "rev1"
    CACHE STRING "Debian package revision/vendor suffix")
set(APP_DEBIAN_ARCHITECTURE
    "arm64"
    CACHE STRING "Debian package architecture")
set(APP_MAINTAINER
    "Chris Johnson <hello@defaced.dev>"
    CACHE STRING "Debian package maintainer")
set(APP_PACKAGE_DESCRIPTION
    "Sonos and Cast now-playing monitor and track blocker"
    CACHE STRING "Debian package summary")
set(APP_INSTALL_SYSTEMD_SERVICE
    OFF
    CACHE BOOL "Install a systemd service file for embedded deployments")

set(APP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/package")
configure_file("${CMAKE_CURRENT_LIST_DIR}/templates/app.desktop.in"
               "${APP_GENERATED_DIR}/${PROJECT_NAME}.desktop" @ONLY)

if(APP_INSTALL_SYSTEMD_SERVICE)
    configure_file("${CMAKE_CURRENT_LIST_DIR}/templates/app.service.in"
                   "${APP_GENERATED_DIR}/${PROJECT_NAME}.service" @ONLY)
endif()

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/config/humbug.conf" DESTINATION "/etc")

install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/images/cp0-light.png"
          "${CMAKE_CURRENT_SOURCE_DIR}/assets/images/cp0-dark.png"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/${APP_NAME}/images")
install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/images/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/APPLaunch/share/images"
    FILES_MATCHING
    PATTERN "humbug*.png")

install(FILES "${APP_GENERATED_DIR}/${PROJECT_NAME}.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/APPLaunch/applications")

if(APP_INSTALL_SYSTEMD_SERVICE)
    install(FILES "${APP_GENERATED_DIR}/${PROJECT_NAME}.service"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/system")
endif()

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md" DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    RENAME copyright)
install(
    FILES "${tinyxml2_SOURCE_DIR}/LICENSE.txt"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    RENAME tinyxml2-license.txt)
install(
    FILES "${utf8proc_SOURCE_DIR}/LICENSE.md"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    RENAME utf8proc-license.md)
install(
    FILES "${mdns_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    RENAME mdns-license.txt)
if(nlohmann_json_SOURCE_DIR AND EXISTS "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT")
    install(
        FILES "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}"
        RENAME nlohmann-json-license.txt)
endif()
install(
    FILES "${lvgl_SOURCE_DIR}/LICENCE.txt"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    RENAME lvgl-license.txt)
install(FILES "${lvgl_SOURCE_DIR}/src/stdlib/builtin/LICENSE_TLSF.txt"
              "${lvgl_SOURCE_DIR}/src/stdlib/builtin/LICENSE_SPRINTF.txt"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(
    FILES "${lvgl_SOURCE_DIR}/src/libs/thorvg/LICENSE.txt"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    RENAME thorvg-license.txt)
install(
    FILES "${lvgl_SOURCE_DIR}/src/libs/tjpgd/LICENSE.txt"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    RENAME tjpgd-license.txt)
if(fmt_SOURCE_DIR AND EXISTS "${fmt_SOURCE_DIR}/LICENSE.rst")
    install(
        FILES "${fmt_SOURCE_DIR}/LICENSE.rst"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}"
        RENAME fmt-license.rst)
endif()
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_OUTPUT_FILE_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/dist")
set(CPACK_PACKAGE_NAME "${APP_DISPLAY_NAME}")
set(CPACK_PACKAGE_VENDOR "${APP_MAINTAINER}")
set(CPACK_PACKAGE_CONTACT "${APP_MAINTAINER}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${APP_PACKAGE_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "${APP_DISPLAY_NAME}_${PROJECT_VERSION}_${APP_DEBIAN_REVISION}-cpz_${APP_DEBIAN_ARCHITECTURE}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

string(TOLOWER "${APP_DISPLAY_NAME}" APP_DEBIAN_PACKAGE_NAME)
string(REGEX REPLACE "[^a-z0-9+.-]" "-" APP_DEBIAN_PACKAGE_NAME "${APP_DEBIAN_PACKAGE_NAME}")
set(CPACK_DEBIAN_PACKAGE_NAME "${APP_DEBIAN_PACKAGE_NAME}")
set(CPACK_DEBIAN_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_DEBIAN_PACKAGE_RELEASE "${APP_DEBIAN_REVISION}")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${APP_DEBIAN_ARCHITECTURE}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${APP_MAINTAINER}")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(APP_DEBIAN_DEPENDS
    "libc6, libstdc++6, libgcc-s1, libfreetype6, libpng16-16, libjpeg62-turbo, zlib1g, libssl3 | libssl3t64")
if(FMT_INCLUDE_DIR AND FMT_LIBRARY)
    string(APPEND APP_DEBIAN_DEPENDS ", libfmt10")
endif()
set(CPACK_DEBIAN_PACKAGE_DEPENDS "${APP_DEBIAN_DEPENDS}")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)

configure_file("${CMAKE_CURRENT_LIST_DIR}/templates/conffiles.in" "${APP_GENERATED_DIR}/conffiles" @ONLY)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${APP_GENERATED_DIR}/conffiles")

include(CPack)
