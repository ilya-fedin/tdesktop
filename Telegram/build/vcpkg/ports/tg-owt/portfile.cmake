set(VCPKG_POLICY_ALLOW_EMPTY_FOLDERS enabled)
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO desktop-app/tg_owt
    REF ${VERSION}
    SHA512 ef2c2163213e2027aebfb5b1f23439a90b428d770fdef986257ab7314bdccc972cd3c2e7048a15092150f9a8479f8480932aed73e1b5ba7817acc1a4034a51b1
    HEAD_REF master
    PATCHES libyuv.patch libyuv-cmake.patch
)

vcpkg_find_acquire_program(PKGCONFIG)
set(ENV{PKG_CONFIG} "${PKGCONFIG}")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS -DTG_OWT_DLOPEN_PIPEWIRE=ON
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME "tg_owt" CONFIG_PATH "lib/cmake/tg_owt")

vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
