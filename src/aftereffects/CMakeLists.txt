if(APPLE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -undefined dynamic_lookup")
endif()

#set(AE_SDK "${CMAKE_SOURCE_DIR}/ext/Adobe After Effects CS5 SDK")
set(AE_SDK "${CMAKE_SOURCE_DIR}/ext/aesdk")
message("Exteranl thing: ${EXTERNAL_INCLUDE_DIRS}")


# LCMS
# FIXME: Duplicated from src/apps/ocio2icc/CMakeLists.txt
set(LCMS_VERSION 2.1)
ExternalProject_Add(LCMS_FOR_AE
    URL ${CMAKE_SOURCE_DIR}/ext/lcms2-${LCMS_VERSION}.tar.gz
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ./configure --prefix=${PROJECT_BINARY_DIR}/ext/dist --without-jpeg --without-tiff --without-zlib
    BUILD_COMMAND make
    INSTALL_COMMAND make install
)
set(LCMS_INCLUDE_DIRS ${PROJECT_BINARY_DIR}/ext/dist/include)
set(LCMS_LIBRARY_DIRS ${PROJECT_BINARY_DIR}/ext/dist/lib)
set(LCMS_STATIC_LIBRARIES ${PROJECT_BINARY_DIR}/ext/dist/lib/liblcms2.a)


include_directories(
    ${CMAKE_SOURCE_DIR}/export/
    ${CMAKE_BINARY_DIR}/export/
    ${EXTERNAL_INCLUDE_DIRS}
    ${LCMS_INCLUDE_DIRS}

    "/Developer/Headers/FlatCarbon"
    "${AE_SDK}/Examples/Headers"
    "${AE_SDK}/Examples/Util"
    "${AE_SDK}/Examples/Headers/SP"
    "${AE_SDK}/Examples/Resources"
)

file(GLOB_RECURSE ae_cpp "*.cpp")
file(GLOB_RECURSE ae_mac_cpp "mac/*.cpp")

add_library(OpenColorIO_AE MODULE
    OpenColorIO_AE.cpp
    OpenColorIO_AE_ArbData.cpp
    OpenColorIO_AE_Context.cpp
    OpenColorIO_AE_UI.cpp
    DrawbotBot.cpp
    ${ae_mac_cpp}
)


target_link_libraries(OpenColorIO_AE
    OpenColorIO
)

add_dependencies(OpenColorIO_AE tinyxml YAML_CPP_LOCAL LCMS_FOR_AE)
set_target_properties(OpenColorIO_AE
    PROPERTIES

    BUNDLE 1
    BUNDLE_EXTENSION plugin
    XCODE_ATTRIBUTE_WRAPPER_EXTENSION plugin # Redundant? Needed for xcode project generated?
    XCODE_ATTRIBUTE_MACH_O_TYPE mh_bundle

    PREFIX ""
    OUTPUT_NAME "OpenColorIO"
    COMPILE_FLAGS "${EXTERNAL_COMPILE_FLAGS}"
    LINK_FLAGS "${EXTERNAL_LINK_FLAGS}"
)


# Carbon precompiled header
# From http://www.cmake.org/Bug/view.php?id=1260
include(PCHSupport)
add_precompiled_header(OpenColorIO_AE /Developer/Headers/FlatCarbon/Carbon.h)


###############################################################################
### AE Target ###
message("After Effects!")
#add_custom_target(aeplugin
#    DEPENDS OpenColorIO_AE
#)

install(TARGETS OpenColorIO_AE
    DESTINATION ${CMAKE_INSTALL_EXEC_PREFIX}/plugins/aftereffects)
