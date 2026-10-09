# Older distributions expose OpenSSL 1.1, while current curl needs OpenSSL 3.
# Keep the private transport self-contained instead of replacing system TLS.
FetchContent_Declare(wetypex_openssl
  URL https://github.com/openssl/openssl/releases/download/openssl-3.5.5/openssl-3.5.5.tar.gz
  URL_HASH SHA256=b28c91532a8b65a1f983b4c28b7488174e4a01008e29ce8e69bd789f28bc2a89)
FetchContent_GetProperties(wetypex_openssl)
if(NOT wetypex_openssl_POPULATED)
  FetchContent_Populate(wetypex_openssl)
endif()
find_program(WETYPE_OPENSSL_MAKE make)
if(NOT WETYPE_OPENSSL_MAKE)
  message(FATAL_ERROR "make is required for private OpenSSL")
endif()
set(WETYPE_CURL_OPENSSL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/private-openssl")
ExternalProject_Add(wetypex-openssl
  SOURCE_DIR "${wetypex_openssl_SOURCE_DIR}"
  BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/wetypex-openssl"
  CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env "CC=${CMAKE_C_COMPILER}"
    "CFLAGS=${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_RELEASE} -fPIC -fno-lto"
    "${wetypex_openssl_SOURCE_DIR}/Configure" linux-x86_64
    no-shared no-tests no-module no-dso
    "--prefix=${WETYPE_CURL_OPENSSL_PREFIX}" --libdir=lib --openssldir=/etc/ssl
  BUILD_COMMAND ${WETYPE_OPENSSL_MAKE} -j2
  INSTALL_COMMAND ${WETYPE_OPENSSL_MAKE} install_sw
  BUILD_BYPRODUCTS "${WETYPE_CURL_OPENSSL_PREFIX}/lib/libssl.a"
                   "${WETYPE_CURL_OPENSSL_PREFIX}/lib/libcrypto.a")
install(FILES "${wetypex_openssl_SOURCE_DIR}/LICENSE.txt"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/fcitx5-wetypex
  RENAME openssl-LICENSE.txt)
