set(CPACK_GENERATOR "TGZ")
set(CPACK_PACKAGE_NAME "turtle-code")
set(CPACK_PACKAGE_VENDOR "Turtle Code contributors")
set(CPACK_PACKAGE_CONTACT "Turtle Code contributors <Askay49@users.noreply.github.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Askay49/TurtleCode")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native Qt code editor with an integrated terminal and debugger")
set(CPACK_PACKAGE_FILE_NAME "turtle-code-${PROJECT_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")

set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_SECTION "editors")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# QPA and Wayland are loaded by Qt at runtime and are invisible to dpkg-shlibdeps.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "qt6-wayland")

set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
set(CPACK_RPM_PACKAGE_LICENSE "MIT AND CC-BY-4.0 AND Vim")
set(CPACK_RPM_PACKAGE_RELEASE_DIST ON)
list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    "/usr/share/applications" "/usr/share/icons" "/usr/share/icons/hicolor"
    "/usr/share/licenses"
)

include(CPack)
