include(ExternalProject)

SET(RE2_VERSION "2018-10-01")

if (ISMACOS)
    ExternalProject_Add(re2
        DOWNLOAD_COMMAND git clone --quiet --depth 1 --branch ${RE2_VERSION} --config advice.detachedHead=false https://github.com/google/re2.git
        TIMEOUT 5
        INSTALL_COMMAND ""
        CONFIGURE_COMMAND ""
        BUILD_IN_SOURCE TRUE
        BUILD_COMMAND ${CMAKE_COMMAND} -E env LDFLAGS=-arch\ ${OSX_ARCH} ARFLAGS=-r\ -s\ -c CXXFLAGS=-O3\ -g\ -fPIC\ -target\ ${OSX_ARCH}-apple-darwin${CMAKE_HOST_SYSTEM_VERSION}\ -D_GLIBCXX_USE_CXX11_ABI=0 make -j
        BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/re2-prefix/src/re2/obj/libre2.a
    )
elseif(ISLINUX)
    ExternalProject_Add(re2
        DOWNLOAD_COMMAND git clone --quiet --depth 1 --branch ${RE2_VERSION} --config advice.detachedHead=false https://github.com/google/re2.git
        TIMEOUT 5
        INSTALL_COMMAND ""
        CONFIGURE_COMMAND ""
        BUILD_IN_SOURCE TRUE
        BUILD_COMMAND ${CMAKE_COMMAND} -E env ARFLAGS=-r\ -s\ -c CXXFLAGS=-O3\ -g\ -fPIC\ -D_GLIBCXX_USE_CXX11_ABI=0 make -j
        BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/re2-prefix/src/re2/obj/libre2.a
    )
endif()

ExternalProject_Get_property(re2 SOURCE_DIR)

get_property(FOLDERS_TO_DELETE DIRECTORY PROPERTY ADDITIONAL_MAKE_CLEAN_FILES)
set_property(DIRECTORY PROPERTY ADDITIONAL_MAKE_CLEAN_FILES "${FOLDERS_TO_DELETE};${SOURCE_DIR}")

add_library(re2-lib STATIC IMPORTED)

set_target_properties(re2-lib PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_BINARY_DIR}/re2-prefix/src/re2/
    IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/re2-prefix/src/re2/obj/libre2.a
)

add_dependencies(re2-lib re2)