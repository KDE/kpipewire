ecm_add_test(TEST_NAME TestEncoder
    TestEncoder.cpp

    ${CMAKE_SOURCE_DIR}/src/encoder.cpp
    ${CMAKE_SOURCE_DIR}/src/gifencoder.cpp
    ${CMAKE_SOURCE_DIR}/src/h264vaapiencoder.cpp
    ${CMAKE_SOURCE_DIR}/src/libx264encoder.cpp
    ${CMAKE_SOURCE_DIR}/src/libopenh264encoder.cpp
    ${CMAKE_SOURCE_DIR}/src/libvpxencoder.cpp
    ${CMAKE_SOURCE_DIR}/src/libvpxvp9encoder.cpp
    ${CMAKE_SOURCE_DIR}/src/libwebpencoder.cpp

    ${CMAKE_SOURCE_DIR}/src/pipewireproduce.cpp
    ${CMAKE_SOURCE_DIR}/src/pipewirebaseencodedstream.cpp
    ${CMAKE_SOURCE_DIR}/src/vaapiutils.cpp

    ${CMAKE_BINARY_DIR}/src/logging_record.cpp
    ${CMAKE_BINARY_DIR}/src/logging_frame_statistics.cpp
    ${CMAKE_BINARY_DIR}/src/logging_libav.cpp
    ${CMAKE_BINARY_DIR}/src/logging_vaapi.cpp

    LINK_LIBRARIES
    Qt6::Gui
    Qt6::Test
    KF6::CoreAddons
    KPipeWireDmaBuf
    PkgConfig::AVCodec
    PkgConfig::AVUtil
    PkgConfig::AVFormat
    PkgConfig::AVFilter
    PkgConfig::GBM
    PkgConfig::SWScale
    PkgConfig::LIBVA
    PkgConfig::LIBVA-drm
    epoxy::epoxy
    Libdrm::Libdrm
)

target_include_directories(TestEncoder PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/src)
