cmake_minimum_required(VERSION 3.11)
project(recordme)


find_package(ECM 5.20 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${ECM_KDE_MODULE_DIR})
include(KDEInstallDirs)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDECMakeSettings)
include(ECMQtDeclareLoggingCategory)

find_package(Qt5 REQUIRED COMPONENTS DBus Quick)
find_package(KF5Wayland REQUIRED)
find_package(KF5 REQUIRED COMPONENTS I18n Declarative)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GStreamer IMPORTED_TARGET gstreamer-plugins-base-1.0)

find_package(Wayland REQUIRED)
find_package(PlasmaWaylandProtocols REQUIRED)
find_package(QtWaylandScanner REQUIRED)

pkg_check_modules(PipeWire IMPORTED_TARGET libpipewire-0.3)
add_feature_info(PipeWire PipeWire_FOUND "Required for screencast portal")

#########

qt5_add_dbus_interface(
   SRCS
   org.freedesktop.portal.ScreenCast.xml
   xdp_dbus_screencast_interface
)

ecm_add_qtwayland_client_protocol(SRCS
    PROTOCOL ${PLASMA_WAYLAND_PROTOCOLS_DIR}/screencast.xml
    BASENAME zkde-screencast-unstable-v1
)

ecm_qt_declare_logging_category(SRCS HEADER logging.h IDENTIFIER PIPEWIRE_LOGGING CATEGORY_NAME recordme_pipewire_logging)

add_executable(recordme main.cpp PipeWireSourceItem.cpp pipewirestream.cpp ${SRCS}
                            PlasmaRecordMe.cpp screencasting.cpp
#                             RecordMe.cpp
                            )
target_link_libraries(recordme Qt5::DBus PkgConfig::PipeWire KF5::WaylandClient Wayland::Client Qt5::Quick KF5::I18n KF5::Declarative Qt5::Gui_EGL Qt5::Gui_GL Qt5::GuiPrivate)

configure_file(org.kde.recordme.desktop.cmake ${CMAKE_CURRENT_BINARY_DIR}/org.kde.recordme.desktop @ONLY)

install(TARGETS recordme DESTINATION ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/org.kde.recordme.desktop DESTINATION ${KDE_INSTALL_APPDIR} )
