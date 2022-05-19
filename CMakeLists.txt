cmake_minimum_required(VERSION 3.16)
project(KPipewire)
set(PROJECT_VERSION "5.24.80")
set(PROJECT_VERSION_MAJOR 5)

find_package(ECM 5.20 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${ECM_KDE_MODULE_DIR})
include(KDEInstallDirs)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDECMakeSettings)
include(KDEClangFormat)
include(KDEGitCommitHooks)
include(ECMQtDeclareLoggingCategory)

find_package(Qt5 REQUIRED COMPONENTS DBus Quick WaylandClient)
find_package(KF5 REQUIRED COMPONENTS Wayland I18n CoreAddons)
find_package(Wayland REQUIRED COMPONENTS Client)

find_package(PlasmaWaylandProtocols REQUIRED)
find_package(QtWaylandScanner REQUIRED)

pkg_check_modules(PipeWire IMPORTED_TARGET libpipewire-0.3)

# ffmpeg, for recording
pkg_check_modules(AVCodec IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUtil IMPORTED_TARGET libavutil)
pkg_check_modules(AVFormat IMPORTED_TARGET libavformat)
pkg_check_modules(GBM IMPORTED_TARGET gbm)
pkg_check_modules(SWScale IMPORTED_TARGET libswscale)
find_package(epoxy 1.3 REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# add clang-format target for all our real source files
file(GLOB_RECURSE ALL_CLANG_FORMAT_SOURCE_FILES *.cpp *.h)
kde_clang_format(${ALL_CLANG_FORMAT_SOURCE_FILES})
kde_configure_git_pre_commit_hook(CHECKS CLANG_FORMAT)

# Prevent EGL headers from including platform headers, in particular Xlib.h.
add_definitions(-DMESA_EGL_NO_X11_HEADERS)
add_definitions(-DEGL_NO_X11)
add_definitions(-DEGL_NO_PLATFORM_SPECIFIC_TYPES)

add_subdirectory(src)
if (BUILD_TESTING)
   add_subdirectory(tests)
endif()



