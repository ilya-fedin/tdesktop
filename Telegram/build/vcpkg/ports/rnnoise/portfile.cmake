vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO desktop-app/rnnoise
    REF "${VERSION}"
    SHA512 23196c113f1e6f6288f2ef12b3342e088b7b940ce3b868c39d2f16dd428e62874089d9600c223c4f2ccaaf91775ae667e4f5dc8fd537200af9bb0c5e5d4d9e43
    HEAD_REF master
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME "rnnoise" CONFIG_PATH "lib/cmake/rnnoise")

vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
