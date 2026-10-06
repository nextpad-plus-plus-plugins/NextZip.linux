cmake_minimum_required(VERSION 3.20)
# Keep PROJECT_VERSION in sync with the macOS port's plugin version.
project(NextZip VERSION 1.0.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(NPP_LINUX_DIR "${CMAKE_SOURCE_DIR}/../../nextpad-plus-plus-gtk4")
set(SCINTILLA_INCLUDE "${NPP_LINUX_DIR}/scintilla/include")
set(SZ "${CMAKE_SOURCE_DIR}/deps/7zip")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk4 glib-2.0 gio-2.0)

# The 7z.so engine is built by deps/build-7z-engine.sh (fetch + verify + make
# via upstream's own gcc makefiles). Run it automatically when missing.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/deps/7z.so")
  message(STATUS "deps/7z.so missing — running deps/build-7z-engine.sh")
  execute_process(COMMAND "${CMAKE_SOURCE_DIR}/deps/build-7z-engine.sh"
                  RESULT_VARIABLE SZ_RC)
  if(NOT SZ_RC EQUAL 0)
    message(FATAL_ERROR "7-Zip engine build failed (deps/build-7z-engine.sh)")
  endif()
endif()

# ── 7-Zip client-side SDK support files (mirror Client7z/makefile.gcc) ────────
set(SDK_SOURCES
  ${SZ}/C/Alloc.c
  ${SZ}/CPP/Common/IntToString.cpp ${SZ}/CPP/Common/MyString.cpp
  ${SZ}/CPP/Common/MyVector.cpp    ${SZ}/CPP/Common/NewHandler.cpp
  ${SZ}/CPP/Common/StringConvert.cpp ${SZ}/CPP/Common/StringToInt.cpp
  ${SZ}/CPP/Common/UTFConvert.cpp  ${SZ}/CPP/Common/Wildcard.cpp
  ${SZ}/CPP/Common/MyWindows.cpp
  ${SZ}/CPP/Windows/DLL.cpp        ${SZ}/CPP/Windows/FileDir.cpp
  ${SZ}/CPP/Windows/FileFind.cpp   ${SZ}/CPP/Windows/FileIO.cpp
  ${SZ}/CPP/Windows/FileName.cpp   ${SZ}/CPP/Windows/PropVariant.cpp
  ${SZ}/CPP/Windows/PropVariantConv.cpp ${SZ}/CPP/Windows/TimeUtils.cpp
  ${SZ}/CPP/7zip/Common/FileStreams.cpp
)

# ─────────────────────────────────────────────────────────────────────────────
#  NextZip.so (the Nextpad++ plugin)
# ─────────────────────────────────────────────────────────────────────────────
add_library(NextZip SHARED
  ${SDK_SOURCES}
  src/SevenZipEngine.cpp
  src/NextZipController.cpp
  src/NextZipDialogs.cpp
  src/NextZipPlugin.cpp
  src/LinuxViewBridge.cpp)
set_target_properties(NextZip PROPERTIES PREFIX "" OUTPUT_NAME "NextZip")

target_include_directories(NextZip PRIVATE
  ${SZ}/CPP ${SZ}
  ${NPP_LINUX_DIR}/src        # plugin.h
  ${SCINTILLA_INCLUDE}        # Scintilla.h
  ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(NextZip PRIVATE NDEBUG _REENTRANT _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE)
target_compile_options(NextZip PRIVATE -Wno-deprecated-declarations -fPIC)
target_link_libraries(NextZip PRIVATE PkgConfig::GTK ${CMAKE_DL_LIBS})
# scintilla_view_send_message resolves at dlopen from the host's libscintilla.so.
target_link_options(NextZip PRIVATE -Wl,--allow-shlib-undefined)

# ── engine test harnesses (plain C++, no GTK; link glib for GChecksum) ────────
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
foreach(T engine extract create add nested opts tgz)
  add_executable(test_${T} tests/test_${T}.cpp ${SDK_SOURCES} src/SevenZipEngine.cpp)
  target_include_directories(test_${T} PRIVATE ${SZ}/CPP ${SZ} ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(test_${T} PRIVATE NDEBUG _REENTRANT _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE)
  target_link_libraries(test_${T} PRIVATE PkgConfig::GLIB ${CMAKE_DL_LIBS})
endforeach()

# ── install: plugin + engine + the 7-Zip licences ─────────────────────────────
set(NEXTZIP_INSTALL_DIR "$ENV{HOME}/.local/share/nextpad++/plugins/NextZip"
    CACHE PATH "Where to install the plugin folder")
install(TARGETS NextZip DESTINATION "${NEXTZIP_INSTALL_DIR}")
install(FILES "${CMAKE_SOURCE_DIR}/deps/7z.so" DESTINATION "${NEXTZIP_INSTALL_DIR}")
install(FILES "${SZ}/DOC/License.txt" "${SZ}/DOC/unRarLicense.txt"
        DESTINATION "${NEXTZIP_INSTALL_DIR}" OPTIONAL)

# distributable zip
set(NEXTZIP_STAGE "${CMAKE_BINARY_DIR}/pkg/NextZip")
file(MAKE_DIRECTORY "${NEXTZIP_STAGE}")
add_custom_target(package_zip
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:NextZip> "${NEXTZIP_STAGE}/NextZip.so"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/deps/7z.so" "${NEXTZIP_STAGE}/"
  COMMAND ${CMAKE_COMMAND} -E copy "${SZ}/DOC/License.txt" "${SZ}/DOC/unRarLicense.txt" "${NEXTZIP_STAGE}/"
  COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_BINARY_DIR}/pkg"
          ${CMAKE_COMMAND} -E tar cf "${CMAKE_BINARY_DIR}/NextZip.linux.zip" --format=zip NextZip
  DEPENDS NextZip
  COMMENT "Packaging NextZip.linux.zip")
