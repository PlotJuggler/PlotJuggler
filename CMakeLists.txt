cmake_minimum_required(VERSION 3.10)

PROJECT(plotjuggler)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH}  "${CMAKE_CURRENT_LIST_DIR}/cmake")

add_definitions(
    -DPJ_MAJOR_VERSION="3"
    -DPJ_MINOR_VERSION="2"
    -DPJ_PATCH_VERSION="0" )

include_directories( plotjuggler_base/include )

# http://answers.ros.org/question/230877/optionally-build-a-package-with-catkin/
if( CATKIN_DEVEL_PREFIX OR catkin_FOUND OR CATKIN_BUILD_BINARY_PACKAGE)
    set(COMPILING_WITH_CATKIN 1)

    message(STATUS "--------------------------------------------------")
    message(STATUS "PlotJuggler is being built using CATKIN.")
    message(STATUS "--------------------------------------------------")

    find_package(catkin REQUIRED COMPONENTS roslib )

    catkin_package(
        INCLUDE_DIRS  plotjuggler_base/include
        LIBRARIES  plotjuggler_plugin_base
        CATKIN_DEPENDS roslib
    )

message(${catkin_INCLUDE_DIRS})

    include_directories(${catkin_INCLUDE_DIRS})
    add_definitions(-DCOMPILED_WITH_CATKIN)

elseif( DEFINED ENV{AMENT_PREFIX_PATH})
    set(COMPILING_WITH_AMENT 1)

    find_package(ament_index_cpp REQUIRED)
    include_directories(${ament_index_cpp_INCLUDE_DIRS})

    message(STATUS "--------------------------------------------------")
    message(STATUS "PlotJuggler is being built using AMENT.")
    message(STATUS "--------------------------------------------------")

    add_definitions(-DCOMPILED_WITH_AMENT)

else()
    message(STATUS "--------------------------------------------------")
    message(STATUS "PlotJuggler is being built WITHOUT any ROS support")
    message(STATUS "--------------------------------------------------")
endif()

cmake_policy (SET CMP0020 NEW)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
SET(CMAKE_AUTOUIC ON)


if (NOT WIN32)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fno-omit-frame-pointer")
endif()

if(APPLE AND EXISTS /usr/local/opt/qt5)
    # Homebrew installs Qt5 (up to at least 5.9.1) in
    # /usr/local/qt5, ensure it can be found by CMake since
    # it is not in the default /usr/local prefix.
    # source: https://github.com/Homebrew/homebrew-core/issues/8392#issuecomment-325226494
    list(APPEND CMAKE_PREFIX_PATH "/usr/local/opt/qt5")
    set(CMAKE_MACOSX_RPATH 1)
endif()


find_package(Qt5 REQUIRED COMPONENTS
    Core
    Widgets
    PrintSupport
    Concurrent
    Xml
    Svg
    OpenGL
    WebSockets
    Network)

set( QT_LINK_LIBRARIES
    Qt5::Core
    Qt5::Widgets
    Qt5::PrintSupport
    Qt5::Xml
    Qt5::Concurrent
    Qt5::Svg
    Qt5::OpenGL
    Qt5::WebSockets
    )


if (NOT CMAKE_BUILD_TYPE)
    message(STATUS "No build type selected, default to RelWithDebInfo")
    set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()

IF (NOT WIN32)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}  -fPIC")
else()
    set(CMAKE_WIN32_EXECUTABLE ON)
    set(GUI_TYPE WIN32)
### Embedd Icon to Plotjuggler.exe
    set(BACKWARD_ENABLE ${BACKWARD_ENABLE} ../../Plotjuggler.rc)
ENDIF()

find_package(ZMQ)

include_directories(
    ${Qt5Core_INCLUDE_DIRS}
    ${Qt5Widgets_INCLUDE_DIRS}
    ${Qt5Concurrent_INCLUDE_DIRS}
    ${Qt5PrintSupport_INCLUDE_DIRS}
    ${Qt5Xml_INCLUDE_DIRS}
    ${Qt5WebSockets_INCLUDE_DIRS}
    ${Qt5Svg_INCLUDE_DIRS}
    ${Qt5OpenGL_INCLUDE_DIRS}
    ./3rdparty/
    ./3rdparty/color_widgets/include
    ./3rdparty/Qt-Advanced-Docking/include
    ./3rdparty/qwt/src
    ./3rdparty/sol
    ./3rdparty/lua-5.4.1/src
)

#########################  BASE library  ####################################

qt5_wrap_cpp(PLOTJUGGLER_BASE_MOCS
  plotjuggler_base/include/PlotJuggler/dataloader_base.h
  plotjuggler_base/include/PlotJuggler/datastreamer_base.h
  plotjuggler_base/include/PlotJuggler/statepublisher_base.h
  plotjuggler_base/include/PlotJuggler/transform_function.h
)

add_library( plotjuggler_plugin_base STATIC ${PLOTJUGGLER_BASE_MOCS} )

#########################  INSTALL  ####################################

if(COMPILING_WITH_CATKIN)

    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CATKIN_DEVEL_PREFIX}/${CATKIN_PACKAGE_LIB_DESTINATION})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CATKIN_DEVEL_PREFIX}/${CATKIN_PACKAGE_BIN_DESTINATION})
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CATKIN_DEVEL_PREFIX}/${CATKIN_PACKAGE_BIN_DESTINATION})
    set(PJ_PLUGIN_INSTALL_DIRECTORY ${CATKIN_PACKAGE_BIN_DESTINATION} )

elseif(COMPILING_WITH_AMENT)

    find_package(ament_cmake REQUIRED)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(PJ_PLUGIN_INSTALL_DIRECTORY lib/${PROJECT_NAME} )

    ament_export_targets(export_plotjuggler_plugin_base HAS_LIBRARY_TARGET)
    ament_package()

else()

    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(PJ_PLUGIN_INSTALL_DIRECTORY bin)

endif()

set(DESKTOP_ICON_DIR "${CMAKE_INSTALL_PREFIX}/share/icons")

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/PlotJuggler.desktop
        DESTINATION  "${CMAKE_INSTALL_PREFIX}/share/applications"  )

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/plotjuggler.svg
        DESTINATION ${DESKTOP_ICON_DIR}  )

install(
     TARGETS plotjuggler_plugin_base
     EXPORT export_plotjuggler_plugin_base
     LIBRARY DESTINATION lib
     ARCHIVE DESTINATION lib
     RUNTIME DESTINATION bin
     INCLUDES DESTINATION include )

install(DIRECTORY plotjuggler_base/include/ DESTINATION include )

######################## Main App, Libraries amd Plugins ###################################

if (NOT WIN32)
  add_subdirectory( 3rdparty/backward-cpp )
endif()

add_subdirectory( 3rdparty/color_widgets )
add_subdirectory( 3rdparty/Qt-Advanced-Docking )
add_subdirectory( 3rdparty/qwt/src )
add_subdirectory( 3rdparty/lua-5.4.1 )

add_subdirectory( plotjuggler_app )

add_subdirectory( plotjuggler_plugins/DataLoadCSV )
add_subdirectory( plotjuggler_plugins/DataLoadULog )
add_subdirectory( plotjuggler_plugins/DataStreamSample )
add_subdirectory( plotjuggler_plugins/DataStreamWebsocket )
add_subdirectory( plotjuggler_plugins/DataStreamUDP )
add_subdirectory( plotjuggler_plugins/StatePublisherCSV )

if( ZMQ_FOUND )
  add_subdirectory( plotjuggler_plugins/DataStreamZMQ )
endif()
