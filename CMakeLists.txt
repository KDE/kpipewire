cmake_minimum_required(VERSION 3.16)
project(recordme)


find_package(ECM 5.20 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${ECM_KDE_MODULE_DIR})
include(KDEInstallDirs)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDECMakeSettings)
include(ECMQtDeclareLoggingCategory)

find_package(Qt5 REQUIRED COMPONENTS DBus Quick)
find_package(KF5 REQUIRED COMPONENTS Wayland I18n CoreAddons)
find_package(Wayland REQUIRED COMPONENTS Client)

find_package(PlasmaWaylandProtocols REQUIRED)
find_package(QtWaylandScanner REQUIRED)

pkg_check_modules(PipeWire IMPORTED_TARGET libpipewire-0.3)

#
pkg_check_modules(AVCodec IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUtil IMPORTED_TARGET libavutil)
pkg_check_modules(AVFormat IMPORTED_TARGET libavformat)
pkg_check_modules(GBM IMPORTED_TARGET gbm)
pkg_check_modules(SWScale IMPORTED_TARGET libswscale)
find_package(epoxy 1.3 REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prevent EGL headers from including platform headers, in particular Xlib.h.
add_definitions(-DMESA_EGL_NO_X11_HEADERS)
add_definitions(-DEGL_NO_X11)
add_definitions(-DEGL_NO_PLATFORM_SPECIFIC_TYPES)

#############

ecm_add_qtwayland_client_protocol(Declarative_SRCS
    PROTOCOL ${PLASMA_WAYLAND_PROTOCOLS_DIR}/screencast.xml
    BASENAME zkde-screencast-unstable-v1
)

ecm_qt_declare_logging_category(Declarative_SRCS
                            HEADER logging.h
                            IDENTIFIER PIPEWIRE_LOGGING
                            CATEGORY_NAME kde_recordme_pipewire_logging
                            DESCRIPTION "PipeWire components for KDE"
                            EXPORT KDERecordmePipeWire)
ecm_qt_install_logging_categories(
    EXPORT KDERecordmePipeWire
    FILE recordme.categories
    DESTINATION "${KDE_INSTALL_LOGGINGCATEGORIESDIR}"
)


add_library(KPipewire STATIC
    libkpipewire/kpipewiredeclarativeplugin.cpp
    libkpipewire/pipewirecore.cpp
    libkpipewire/pipewirerecord.cpp
    libkpipewire/pipewiresourceitem.cpp
    libkpipewire/pipewiresourcestream.cpp
    libkpipewire/screencasting.cpp
    libkpipewire/screencastingrequest.cpp
    ${Declarative_SRCS}
)
target_link_libraries(KPipewire PRIVATE Qt::Quick Qt::GuiPrivate KF5::I18n
    Wayland::Client KF5::WaylandClient Qt5::Gui_EGL KF5::CoreAddons
    PkgConfig::AVCodec PkgConfig::AVUtil PkgConfig::AVFormat PkgConfig::GBM PkgConfig::SWScale epoxy::epoxy # PipeWireRecord
    PkgConfig::PipeWire)
if (${Qt5Gui_OPENGL_IMPLEMENTATION} STREQUAL "GLESv2")
    target_link_libraries(KPipewire PRIVATE Qt5::Gui_GLESv2)
else()
    target_link_libraries(KPipewire PRIVATE Qt5::Gui_GL)
endif()


#########

add_executable(recordme main.cpp recordme.qrc ${SRCS} PlasmaRecordMe.cpp)
target_link_libraries(recordme KF5::WaylandClient Wayland::Client Qt5::Quick Qt::DBus KPipewire)

configure_file(org.kde.recordme.desktop.cmake ${CMAKE_CURRENT_BINARY_DIR}/org.kde.recordme.desktop @ONLY)

install(TARGETS recordme DESTINATION ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/org.kde.recordme.desktop DESTINATION ${KDE_INSTALL_APPDIR} )

#########

qt5_add_dbus_interface(
   XDP_SRCS
   org.freedesktop.portal.ScreenCast.xml
   xdp_dbus_screencast_interface
)

add_executable(xdp-recordme xdp-main.cpp recordme.qrc ${XDP_SRCS} RecordMe.cpp)
target_link_libraries(xdp-recordme Qt5::DBus Qt5::Quick KPipewire)
install(TARGETS xdp-recordme DESTINATION ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})

