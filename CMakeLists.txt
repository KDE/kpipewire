cmake_minimum_required(VERSION 3.11)
project(xdgrecordme)


find_package(ECM 5.20 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${ECM_KDE_MODULE_DIR})
include(KDEInstallDirs)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDECMakeSettings)

find_package(Qt5 REQUIRED COMPONENTS DBus)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GStreamer IMPORTED_TARGET gstreamer-plugins-base-1.0)


qt5_add_dbus_interface(
   SRCS
   org.freedesktop.portal.ScreenCast.xml
   xdp_dbus_screencast_interface
)

add_executable(xdgrecordme main.cpp RecordMe.cpp PipelineItem.cpp ${SRCS})
target_link_libraries(xdgrecordme Qt5::DBus PkgConfig::GStreamer)

install(TARGETS xdgrecordme RUNTIME DESTINATION bin)
