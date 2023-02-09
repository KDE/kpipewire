include(ECMGenerateExportHeader)
include(ECMQmlModule)
include(ECMGenerateHeaders)
include(ECMSetupVersion)

add_definitions(-DTRANSLATION_DOMAIN=\"kpipewire5\")

add_library(KPipeWire)

ecm_qt_declare_logging_category(kpipewire_SRCS
                            HEADER logging.h
                            IDENTIFIER PIPEWIRE_LOGGING
                            CATEGORY_NAME kpipewire_logging
                            DESCRIPTION "PipeWire components for KDE"
                            EXPORT KPipeWire)
ecm_qt_declare_logging_category(kpipewiredmabuf_SRCS
                            HEADER logging_dmabuf.h
                            IDENTIFIER PIPEWIREDMABUF_LOGGING
                            CATEGORY_NAME kpipewire_dmabuf_logging
                            DESCRIPTION "PipeWire and DMABuf components for KDE"
                            EXPORT KPipeWireDmaBuf)
ecm_qt_declare_logging_category(kpipewirerecord_SRCS
                            HEADER logging_record.h
                            IDENTIFIER PIPEWIRERECORD_LOGGING
                            CATEGORY_NAME kpipewire_record_logging
                            DESCRIPTION "PipeWire and FFmpeg components for KDE"
                            EXPORT KPipeWireRecord)
ecm_qt_install_logging_categories(
    EXPORT KPipeWire
    FILE kpipewire.categories
    DESTINATION "${KDE_INSTALL_LOGGINGCATEGORIESDIR}"
)

ecm_qt_install_logging_categories(
    EXPORT KPipeWireRecord
    FILE kpipewirerecord.categories
    DESTINATION "${KDE_INSTALL_LOGGINGCATEGORIESDIR}"
)

target_sources(KPipeWire PRIVATE
    pipewirecore.cpp
    pipewiresourceitem.cpp
    pipewiresourcestream.cpp
    glhelpers.cpp
    pwhelpers.cpp
    ${kpipewire_SRCS}
)

target_link_libraries(KPipeWire PRIVATE Qt::Quick Qt::GuiPrivate KF5::I18n
    KF5::CoreAddons PkgConfig::PipeWire epoxy::epoxy PkgConfig::GBM)

target_compile_definitions(KPipeWire INTERFACE -DKPW_WITH_SOURCESIZE=1)

if (QT_MAJOR_VERSION EQUAL "5")
    target_link_libraries(KPipeWire PRIVATE Qt5::Gui_EGL)
    if (${Qt5Gui_OPENGL_IMPLEMENTATION} STREQUAL "GLESv2")
        target_link_libraries(KPipeWire PRIVATE Qt5::Gui_GLESv2)
    else()
        target_link_libraries(KPipeWire PRIVATE Qt5::Gui_GL)
    endif()
else()
    target_link_libraries(KPipeWire PRIVATE EGL::EGL)
endif()

target_include_directories(KPipeWire PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
                                     PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>" ${PipeWire_INCLUDE_DIRS}
                                     INTERFACE "$<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}/KPipeWire>")
set_target_properties(KPipeWire PROPERTIES
                             VERSION ${PROJECT_VERSION}
                             SOVERSION ${PROJECT_VERSION_MAJOR}
                      )

ecm_generate_export_header(KPipeWire
    BASE_NAME KPipeWire
    VERSION ${PROJECT_VERSION}
)

ecm_generate_headers(KPipeWire_HEADERS
  HEADER_NAMES
  PipeWireSourceStream
  PipeWireSourceItem
  REQUIRED_HEADERS KPipeWire_HEADERS
)

install(FILES
  ${KPipeWire_HEADERS}
  ${CMAKE_CURRENT_BINARY_DIR}/kpipewire_export.h
  DESTINATION ${KDE_INSTALL_INCLUDEDIR}/KPipeWire COMPONENT Devel
)

add_library(KPipeWireDmaBuf dmabufhandler.cpp ${kpipewiredmabuf_SRCS})
target_link_libraries(KPipeWireDmaBuf PUBLIC KPipeWire
    PRIVATE Qt::Core Qt::Gui PkgConfig::GBM
    epoxy::epoxy Libdrm::Libdrm Qt::GuiPrivate
)

set_target_properties(KPipeWireDmaBuf PROPERTIES
                             VERSION ${PROJECT_VERSION}
                             SOVERSION ${PROJECT_VERSION_MAJOR}
                      )

ecm_generate_export_header(KPipeWireDmaBuf
    BASE_NAME KPipeWireDmaBuf
    VERSION ${PROJECT_VERSION}
)

ecm_generate_headers(KPipeWireDmaBuf_HEADERS
  HEADER_NAMES
    DmaBufHandler
  REQUIRED_HEADERS KPipeWireDmaBuf_HEADERS
)

install(FILES
  ${KPipeWireDmaBuf_HEADERS}
  ${CMAKE_CURRENT_BINARY_DIR}/kpipewiredmabuf_export.h
  DESTINATION ${KDE_INSTALL_INCLUDEDIR}/KPipeWire COMPONENT Devel
)

add_library(KPipeWireRecord pipewirerecord.cpp ${kpipewirerecord_SRCS})
target_link_libraries(KPipeWireRecord PUBLIC KPipeWire
    PRIVATE Qt::Core Qt::Gui KF5::CoreAddons KPipeWireDmaBuf
    PkgConfig::AVCodec PkgConfig::AVUtil PkgConfig::AVFormat PkgConfig::GBM PkgConfig::SWScale
    epoxy::epoxy Libdrm::Libdrm Qt::GuiPrivate
)

target_compile_definitions(KPipeWireRecord INTERFACE -DKPW_WITH_SUGGESTED=1)

ecm_generate_headers(KPipeWireRecord_HEADERS
  HEADER_NAMES
  PipeWireRecord
  REQUIRED_HEADERS KPipeWireRecord_HEADERS
)
set_target_properties(KPipeWireRecord PROPERTIES
                             VERSION ${PROJECT_VERSION}
                             SOVERSION ${PROJECT_VERSION_MAJOR}
                      )

install(FILES
  ${KPipeWireRecord_HEADERS}
  DESTINATION ${KDE_INSTALL_INCLUDEDIR}/KPipeWire COMPONENT Devel
)

install(TARGETS KPipeWire KPipeWireRecord KPipeWireDmaBuf EXPORT KPipeWireTargets ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})


ecm_setup_version(PROJECT VARIABLE_PREFIX KPIPEWIRE
                        VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/kpipewire_version.h"
                        PACKAGE_VERSION_FILE "${CMAKE_CURRENT_BINARY_DIR}/KPipeWireConfigVersion.cmake"
                        SOVERSION 5)

set(CMAKECONFIG_INSTALL_DIR "${KDE_INSTALL_CMAKEPACKAGEDIR}/KPipeWire")
install(EXPORT KPipeWireTargets DESTINATION "${CMAKECONFIG_INSTALL_DIR}" FILE KPipeWireTargets.cmake NAMESPACE K::)

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

