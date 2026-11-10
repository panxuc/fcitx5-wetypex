# Apple ARM64 uses libc++'s alternate string layout. Keep this dependency
# private to the compatibility process; frontends use the distribution ABI.
set(WETYPE_DARWIN_LIBCXX_ROOT "" CACHE PATH "Prebuilt compatible ARM64 libc++ prefix")
set(WETYPE_DARWIN_LIBCXX_LICENSE "" CACHE FILEPATH "License for a prebuilt private libc++")
if(WETYPE_DARWIN_LIBCXX_ROOT)
  set(WETYPE_DARWIN_CXX_PREFIX "${WETYPE_DARWIN_LIBCXX_ROOT}")
  if(NOT EXISTS "${WETYPE_DARWIN_CXX_PREFIX}/include/c++/v1/string" OR
     NOT EXISTS "${WETYPE_DARWIN_CXX_PREFIX}/lib/libc++.so.1")
    message(FATAL_ERROR "The private ARM64 libc++ prefix is incomplete")
  endif()
  add_custom_target(wetypex-darwin-cxx)
else()
  include(ExternalProject)
  set(WETYPE_LLVM_VERSION 18.1.8)
  foreach(component libcxx libcxxabi cmake runtimes)
    if(component STREQUAL "libcxx")
      set(hash bdecf90be0072bc720fd5c9c8ab061cdb197edd0c8ad3e170dc3e6bfaa49f388)
    elseif(component STREQUAL "libcxxabi")
      set(hash 256c30d724eeb72713bc08ae1692f53aaf4ebe8a1d662c92bf59e69d6c53dce9)
    elseif(component STREQUAL "cmake")
      set(hash 59badef592dd34893cd319d42b323aaa990b452d05c7180ff20f23ab1b41e837)
    else()
      set(hash 9997c2e91e5438e2963306ba5019d85b5384b467535632738d8670ced8f07cb3)
    endif()
    FetchContent_Declare(wetypex_${component}
      URL "https://github.com/llvm/llvm-project/releases/download/llvmorg-${WETYPE_LLVM_VERSION}/${component}-${WETYPE_LLVM_VERSION}.src.tar.xz"
      URL_HASH "SHA256=${hash}")
    FetchContent_GetProperties(wetypex_${component})
    if(NOT wetypex_${component}_POPULATED)
      FetchContent_Populate(wetypex_${component})
    endif()
  endforeach()
  set(forest "${CMAKE_CURRENT_BINARY_DIR}/darwin-cxx-source")
  file(MAKE_DIRECTORY "${forest}")
  foreach(component libcxx libcxxabi cmake runtimes)
    if(NOT EXISTS "${forest}/${component}")
      execute_process(COMMAND "${CMAKE_COMMAND}" -E create_symlink
        "${wetypex_${component}_SOURCE_DIR}" "${forest}/${component}"
        RESULT_VARIABLE linked)
      if(NOT linked EQUAL 0)
        message(FATAL_ERROR "Unable to assemble the private libc++ source tree")
      endif()
    endif()
  endforeach()
  set(WETYPE_DARWIN_CXX_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/darwin-cxx")
  find_program(WETYPE_CLANGCC NAMES clang)
  find_library(WETYPE_CXXABI_LIBRARY NAMES c++abi)
  if(NOT WETYPE_CXXABI_LIBRARY OR NOT WETYPE_CLANGCC)
    message(FATAL_ERROR "clang and the target libc++abi development library are required")
  endif()
  get_filename_component(abi_directory "${WETYPE_CXXABI_LIBRARY}" DIRECTORY)
  set(target_arguments)
  if(CMAKE_CROSSCOMPILING)
    list(APPEND target_arguments
      "-DCMAKE_C_COMPILER_TARGET=${CMAKE_LIBRARY_ARCHITECTURE}"
      "-DCMAKE_CXX_COMPILER_TARGET=${CMAKE_LIBRARY_ARCHITECTURE}")
  endif()
  ExternalProject_Add(wetypex-darwin-cxx
    SOURCE_DIR "${forest}/libcxx"
    BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/darwin-cxx-build"
    DOWNLOAD_COMMAND "" UPDATE_COMMAND "" PATCH_COMMAND ""
    CMAKE_ARGS -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64
      "-DCMAKE_C_COMPILER=${WETYPE_CLANGCC}"
      "-DCMAKE_CXX_COMPILER=${WETYPE_CLANGXX}"
      "-DCMAKE_SYSROOT=${CMAKE_SYSROOT}"
      "-DCMAKE_BUILD_TYPE=Release"
      "-DCMAKE_INSTALL_PREFIX=${WETYPE_DARWIN_CXX_PREFIX}" -DCMAKE_INSTALL_LIBDIR=lib
      -DLIBCXX_ABI_DEFINES=_LIBCPP_ABI_ALTERNATE_STRING_LAYOUT
      -DLIBCXX_CXX_ABI=system-libcxxabi
      "-DLIBCXX_CXX_ABI_INCLUDE_PATHS=${forest}/libcxxabi/include"
      "-DLIBCXX_CXX_ABI_LIBRARY_PATH=${abi_directory}"
      "-DCMAKE_SHARED_LINKER_FLAGS=-L${abi_directory} -Wl,-rpath-link,${abi_directory}"
      -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF
      -DLIBCXX_ENABLE_SHARED=ON -DLIBCXX_ENABLE_STATIC=OFF
      -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=OFF ${target_arguments}
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --parallel 2
    INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR>
    BUILD_BYPRODUCTS "${WETYPE_DARWIN_CXX_PREFIX}/lib/libc++.so.1")
  set(WETYPE_DARWIN_LIBCXX_LICENSE "${wetypex_libcxx_SOURCE_DIR}/LICENSE.TXT")
endif()
set(WETYPE_DARWIN_CXX_COMPILE_FLAGS -nostdinc++
  "-I${WETYPE_DARWIN_CXX_PREFIX}/include/c++/v1")
set(WETYPE_DARWIN_CXX_LINK_FLAGS "-L${WETYPE_DARWIN_CXX_PREFIX}/lib")
find_library(WETYPE_CXXABI_LIBRARY NAMES c++abi)
if(NOT WETYPE_CXXABI_LIBRARY)
  message(FATAL_ERROR "The target libc++abi development library is required")
endif()
get_filename_component(real_abi_library "${WETYPE_CXXABI_LIBRARY}" REALPATH)
get_filename_component(real_abi_directory "${real_abi_library}" DIRECTORY)
list(APPEND WETYPE_DARWIN_CXX_LINK_FLAGS "-L${real_abi_directory}"
  "-Wl,-rpath-link,${real_abi_directory}")
install(DIRECTORY "${WETYPE_DARWIN_CXX_PREFIX}/lib/"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/fcitx5-wetypex/abi/lib"
  FILES_MATCHING PATTERN "libc++.so*")
if(WETYPE_DARWIN_LIBCXX_LICENSE)
  install(FILES "${WETYPE_DARWIN_LIBCXX_LICENSE}"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/fcitx5-wetypex" RENAME libcxx-LICENSE)
endif()
