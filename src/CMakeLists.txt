include(ECMGenerateExportHeader)
include(ECMQmlModule)
include(ECMGenerateHeaders)
include(ECMSetupVersion)

ecm_qt_declare_logging_category(kpipewire_SRCS
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

ecm_add_qtwayland_client_protocol(kpipewire_SRCS
    PROTOCOL ${PLASMA_WAYLAND_PROTOCOLS_DIR}/screencast.xml
    BASENAME zkde-screencast-unstable-v1
)

add_library(KPipeWire
    pipewirecore.cpp
    pipewiresourceitem.cpp
    pipewiresourcestream.cpp
    screencasting.cpp
    screencastingrequest.cpp
    ${kpipewire_SRCS}
)

target_link_libraries(KPipeWire PRIVATE Qt::Quick Qt::GuiPrivate KF5::I18n
    Wayland::Client KF5::WaylandClient Qt5::Gui_EGL KF5::CoreAddons
    PkgConfig::PipeWire Qt::WaylandClient)

if (${Qt5Gui_OPENGL_IMPLEMENTATION} STREQUAL "GLESv2")
    target_link_libraries(KPipeWire PRIVATE Qt5::Gui_GLESv2)
else()
    target_link_libraries(KPipeWire PRIVATE Qt5::Gui_GL)
endif()

target_include_directories(KPipeWire PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(KPipeWire INTERFACE "$<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}/KPipeWire>")

ecm_generate_export_header(KPipeWire
    BASE_NAME KPipeWire
    VERSION ${PROJECT_VERSION}
)

ecm_generate_headers(KPipeWire_HEADERS
  HEADER_NAMES
  PipeWireCore
  PipeWireSourceItem
  Screencasting
  ScreencastingRequest
)

install(FILES
  ${KPipeWire_HEADERS}
  ${CMAKE_CURRENT_BINARY_DIR}/kpipewire_export.h
  DESTINATION ${KDE_INSTALL_INCLUDEDIR}/KPipeWire COMPONENT Devel
)

add_library(KPipeWireRecord pipewirerecord.cpp)
target_link_libraries(KPipeWireRecord PUBLIC KPipeWire
    PRIVATE Qt::Core Qt::Gui KF5::CoreAddons
    PkgConfig::AVCodec PkgConfig::AVUtil PkgConfig::AVFormat PkgConfig::GBM PkgConfig::SWScale epoxy::epoxy # PipeWireRecord
)

ecm_generate_headers(KPipeWireRecord_HEADERS
  HEADER_NAMES
  PipeWireRecord
)

install(FILES
  ${KPipeWireRecord_HEADERS}
  DESTINATION ${KDE_INSTALL_INCLUDEDIR}/KPipeWire COMPONENT Devel
)

install(TARGETS KPipeWire KPipeWireRecord EXPORT KPipeWireTargets ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})


ecm_setup_version(PROJECT VARIABLE_PREFIX KPIPEWIRE
                        VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/kpipewire_version.h"
                        PACKAGE_VERSION_FILE "${CMAKE_CURRENT_BINARY_DIR}/KPipeWireConfigVersion.cmake"
                        SOVERSION 5)

set(CMAKECONFIG_INSTALL_DIR "${KDE_INSTALL_CMAKEPACKAGEDIR}/KPipeWire")
install(EXPORT KPipeWireTargets DESTINATION "${CMAKECONFIG_INSTALL_DIR}" FILE KPipewireTargets.cmake NAMESPACE K::)

configure_package_config_file("${CMAKE_CURRENT_SOURCE_DIR}/KPipeWireConfig.cmake.in"
                              "${CMAKE_CURRENT_BINARY_DIR}/KPipeWireConfig.cmake"
                              INSTALL_DESTINATION  ${CMAKECONFIG_INSTALL_DIR}
                              )
install(FILES  "${CMAKE_CURRENT_BINARY_DIR}/KPipeWireConfig.cmake"
               "${CMAKE_CURRENT_BINARY_DIR}/KPipeWireConfigVersion.cmake"
        DESTINATION "${CMAKECONFIG_INSTALL_DIR}"
        COMPONENT Devel )


#################
# Declarative

ecm_add_qml_module(KPipeWireDeclarative URI "org.kde.pipewire" VERSION 1.0)
target_sources(KPipeWireDeclarative PRIVATE kpipewiredeclarativeplugin.cpp)
target_link_libraries(KPipeWireDeclarative PRIVATE KPipeWire Qt::Quick)
ecm_finalize_qml_module(KPipeWireDeclarative DESTINATION ${KDE_INSTALL_QMLDIR})

ecm_add_qml_module(KPipeWireRecordDeclarative URI "org.kde.pipewire.record" VERSION 1.0)
target_sources(KPipeWireRecordDeclarative PRIVATE kpipewirerecorddeclarativeplugin.cpp)
target_link_libraries(KPipeWireRecordDeclarative PRIVATE KPipeWire KPipeWireRecord Qt::Quick)
ecm_finalize_qml_module(KPipeWireRecordDeclarative DESTINATION ${KDE_INSTALL_QMLDIR})
