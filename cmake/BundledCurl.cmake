# Keep these pins in sync with the Arch source recipes.
set(WETYPE_CURL_VERSION "8.22.0")
FetchContent_Declare(wetypex_curl
  URL "https://curl.se/download/curl-${WETYPE_CURL_VERSION}.tar.xz"
  URL_HASH SHA256=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
)
FetchContent_GetProperties(wetypex_curl)
if(NOT wetypex_curl_POPULATED)
  FetchContent_Populate(wetypex_curl)
endif()
include(ExternalProject)
find_package(OpenSSL REQUIRED)
option(WETYPE_BUNDLED_OPENSSL "Use a private static OpenSSL 3 transport" OFF)
if(WETYPE_BUNDLED_OPENSSL OR OPENSSL_VERSION VERSION_LESS "3.0")
  include(${CMAKE_CURRENT_LIST_DIR}/BundledOpenSSL.cmake)
  set(WETYPE_CURL_SSL_LIBRARY "${WETYPE_CURL_OPENSSL_PREFIX}/lib/libssl.a")
  set(WETYPE_CURL_CRYPTO_LIBRARY "${WETYPE_CURL_OPENSSL_PREFIX}/lib/libcrypto.a")
  set(WETYPE_CURL_SSL_DEPENDENCY wetypex-openssl)
  set(WETYPE_CURL_SSL_OPTION "--with-openssl=${WETYPE_CURL_OPENSSL_PREFIX}")
  set(WETYPE_CURL_SSL_CMAKE_FLAGS "-DOPENSSL_ROOT_DIR=${WETYPE_CURL_OPENSSL_PREFIX}"
    -DOPENSSL_USE_STATIC_LIBS=ON)
else()
  set(WETYPE_CURL_SSL_LIBRARY "${OPENSSL_SSL_LIBRARY}")
  set(WETYPE_CURL_CRYPTO_LIBRARY "${OPENSSL_CRYPTO_LIBRARY}")
  set(WETYPE_CURL_SSL_OPTION --with-openssl)
endif()
set(WETYPE_CURL_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/wetypex-curl")
set(WETYPE_CURL_LIBRARY "${WETYPE_CURL_BINARY_DIR}/lib/libcurl.a")
set(WETYPE_CURL_PROTOCOL_OPTIONS)
foreach(protocol DICT FILE FTP GOPHER IMAP IPFS LDAP LDAPS MQTT POP3 RTSP SMTP TELNET TFTP)
  list(APPEND WETYPE_CURL_PROTOCOL_OPTIONS "-DCURL_DISABLE_${protocol}=ON")
endforeach()
if(CMAKE_VERSION VERSION_LESS "3.18")
  # curl's CMake build needs 3.18; its release tarball also provides configure.
  # This keeps Ubuntu 20.04's stock CMake usable without changing transport ABI.
  set(WETYPE_CURL_LIBRARY "${WETYPE_CURL_BINARY_DIR}/lib/.libs/libcurl.a")
  find_program(WETYPE_MAKE make)
  if(NOT WETYPE_MAKE)
    message(FATAL_ERROR "make is required for bundled curl on CMake before 3.18")
  endif()
  ExternalProject_Add(wetypex-curl
    DEPENDS ${WETYPE_CURL_SSL_DEPENDENCY}
    SOURCE_DIR "${wetypex_curl_SOURCE_DIR}"
    BINARY_DIR "${WETYPE_CURL_BINARY_DIR}"
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
      "CC=${CMAKE_C_COMPILER}"
      "CFLAGS=${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_RELEASE} -fPIC -fno-lto"
      "LDFLAGS=${CMAKE_EXE_LINKER_FLAGS} -fno-lto"
      "${wetypex_curl_SOURCE_DIR}/configure"
      --disable-shared --enable-static --enable-websockets --enable-threaded-resolver
      ${WETYPE_CURL_SSL_OPTION} --with-ca-fallback --without-ca-bundle --without-ca-path
      --without-zlib --without-brotli --without-zstd --without-libpsl
      --without-libidn2 --without-libssh2 --without-libssh --without-nghttp2
      --without-nghttp3 --without-ngtcp2 --without-quiche
      --disable-manual --disable-docs
    BUILD_COMMAND ${WETYPE_MAKE} -C <BINARY_DIR>/lib -j1
    BUILD_BYPRODUCTS "${WETYPE_CURL_LIBRARY}"
    INSTALL_COMMAND ""
  )
else()
  ExternalProject_Add(wetypex-curl
  DEPENDS ${WETYPE_CURL_SSL_DEPENDENCY}
  SOURCE_DIR "${wetypex_curl_SOURCE_DIR}"
  BINARY_DIR "${WETYPE_CURL_BINARY_DIR}"
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    # The bridge links with clang/libc++; GCC LTO objects cannot cross that
    # boundary. Preserve packaging/hardening flags but emit native code.
    "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS} -fno-lto"
    "-DCMAKE_C_FLAGS_RELEASE=${CMAKE_C_FLAGS_RELEASE} -fno-lto"
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
    -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF
    -DBUILD_TESTING=OFF -DCURL_DISABLE_INSTALL=ON
    -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF
    -DCURL_USE_OPENSSL=ON -DCURL_DISABLE_WEBSOCKETS=OFF
    ${WETYPE_CURL_SSL_CMAKE_FLAGS}
    -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none -DCURL_CA_FALLBACK=ON
    -DENABLE_THREADED_RESOLVER=ON
    -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF
    -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF
    -DUSE_LIBIDN2=OFF -DUSE_NGHTTP2=OFF
    ${WETYPE_CURL_PROTOCOL_OPTIONS}
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target libcurl_static --parallel 1
  BUILD_BYPRODUCTS "${WETYPE_CURL_LIBRARY}"
  INSTALL_COMMAND ""
)
endif()
set(WETYPE_CURL_COMPILE_FLAGS -DCURL_STATICLIB -DWETYPE_BUNDLED_CURL
  "-I${wetypex_curl_SOURCE_DIR}/include")
set(WETYPE_CURL_LINK_FLAGS "${WETYPE_CURL_LIBRARY}"
  "${WETYPE_CURL_SSL_LIBRARY}" "${WETYPE_CURL_CRYPTO_LIBRARY}" -ldl
  -Wl,--exclude-libs,ALL)
install(FILES "${wetypex_curl_SOURCE_DIR}/COPYING"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/fcitx5-wetypex RENAME curl-COPYING)
