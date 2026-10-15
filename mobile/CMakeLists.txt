cmake_minimum_required(VERSION 3.24)
project(MyFiles VERSION 0.1.0 LANGUAGES C)
if(IOS)
  enable_language(OBJC)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)

# On iOS and Android SDL is built from source; on desktop the installed packages (e.g. Homebrew) are
# used unless MYFILES_VENDOR_SDL=ON.
if(IOS OR ANDROID)
  set(_vendor_default ON)
else()
  set(_vendor_default OFF)
endif()
option(MYFILES_VENDOR_SDL "Build SDL3, SDL3_ttf and SDL3_image from source" ${_vendor_default})
option(MYFILES_TESTS "Build tests" ON)
set(MYFILES_BUNDLE_ID "com.example.myfiles" CACHE STRING "iOS bundle identifier")
set(MYFILES_TEAM_ID "" CACHE STRING "Apple developer team ID for iOS code signing")

# ---- third-party ------------------------------------------------------------------------------------

# SQLite is bundled everywhere so FTS5 is guaranteed (Android has no public system SQLite).
# SOURCE_SUBDIR points at a directory with no CMakeLists.txt, so MakeAvailable only downloads; the
# sources are compiled by the targets below.
FetchContent_Declare(sqlite
  URL https://www.sqlite.org/2026/sqlite-amalgamation-3530100.zip
  URL_HASH SHA256=36ad6e7f38540a3b21a2ac36340833f0a9e426bc1c752751c3ba669466827eae
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR _none)
FetchContent_Declare(cjson
  URL https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.18.tar.gz
  URL_HASH SHA256=3aa806844a03442c00769b83e99970be70fbef03735ff898f4811dd03b9f5ee5
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR _none)
FetchContent_Declare(qrcodegen
  URL https://github.com/nayuki/QR-Code-generator/archive/refs/tags/v1.8.0.tar.gz
  URL_HASH SHA256=2ec0a4d33d6f521c942eeaf473d42d5fe139abcfa57d2beffe10c5cf7d34ae60
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR _none)
FetchContent_MakeAvailable(sqlite cjson qrcodegen)

add_library(mf_sqlite STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(mf_sqlite PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(mf_sqlite PRIVATE
  SQLITE_ENABLE_FTS5 SQLITE_DQS=0 SQLITE_THREADSAFE=1 SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_DEPRECATED)

add_library(mf_thirdparty STATIC ${cjson_SOURCE_DIR}/cJSON.c ${qrcodegen_SOURCE_DIR}/c/qrcodegen.c)
target_include_directories(mf_thirdparty PUBLIC ${cjson_SOURCE_DIR} ${qrcodegen_SOURCE_DIR}/c)

if(MYFILES_VENDOR_SDL)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
  set(SDLTTF_HARFBUZZ OFF CACHE BOOL "" FORCE)
  set(SDLTTF_PLUTOSVG OFF CACHE BOOL "" FORCE)
  set(SDLIMAGE_VENDORED ON CACHE BOOL "" FORCE)
  set(SDLIMAGE_BACKEND_STB ON CACHE BOOL "" FORCE)
  set(SDLIMAGE_DEPS_SHARED OFF CACHE BOOL "" FORCE)
  foreach(fmt AVIF BMP GIF JXL LBM PCX PNM QOI SVG TGA TIF WEBP XCF XPM XV ANI)
    set(SDLIMAGE_${fmt} OFF CACHE BOOL "" FORCE)
  endforeach()
  set(SDLIMAGE_JPG ON CACHE BOOL "" FORCE)
  set(SDLIMAGE_PNG ON CACHE BOOL "" FORCE)
  set(SDLIMAGE_JPG_SAVE ON CACHE BOOL "" FORCE)
  set(SDLIMAGE_PNG_SAVE ON CACHE BOOL "" FORCE)
  set(SDLIMAGE_PNG_LIBPNG OFF CACHE BOOL "" FORCE) # stb + built-in writer; no libpng/zlib to vendor
  FetchContent_Declare(SDL3 URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.8.tar.gz DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_Declare(SDL3_ttf GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git GIT_TAG release-3.2.2 GIT_SHALLOW TRUE GIT_SUBMODULES_RECURSE TRUE)
  FetchContent_Declare(SDL3_image GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git GIT_TAG release-3.4.8 GIT_SHALLOW TRUE GIT_SUBMODULES "")
  FetchContent_MakeAvailable(SDL3 SDL3_ttf SDL3_image)
else()
  find_package(SDL3 REQUIRED CONFIG)
  find_package(SDL3_ttf REQUIRED CONFIG)
  find_package(SDL3_image REQUIRED CONFIG)
endif()

# ---- core (no UI, no platform code) -----------------------------------------------------------------

add_library(myfiles_core STATIC src/fileno.c src/db.c src/sync.c src/pdf.c)
target_include_directories(myfiles_core PUBLIC src)
target_link_libraries(myfiles_core PUBLIC SDL3::SDL3 mf_sqlite mf_thirdparty)

# ---- app --------------------------------------------------------------------------------------------

set(APP_SOURCES src/main.c src/ui.c src/screens.c src/label.c src/platform/platform_common.c)
if(IOS)
  list(APPEND APP_SOURCES src/platform/platform_ios.m)
elseif(ANDROID)
  list(APPEND APP_SOURCES src/platform/platform_android.c)
else()
  list(APPEND APP_SOURCES src/platform/platform_desktop.c src/platform/http_curl.c)
endif()

set(FONT_FILES ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSans-Regular.ttf ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSans-Bold.ttf)
set(IMAGE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/assets/images/logo.png)
if(IOS)
  set(ICON_FILES ${CMAKE_CURRENT_SOURCE_DIR}/ios/Assets.xcassets)
elseif(APPLE)
  set(ICON_FILES ${CMAKE_CURRENT_SOURCE_DIR}/brand/MyFiles.icns)
endif()

if(ANDROID)
  # SDLActivity loads libmain.so
  add_library(main SHARED ${APP_SOURCES})
  set(APP_TARGET main)
else()
  add_executable(MyFiles MACOSX_BUNDLE ${APP_SOURCES} ${FONT_FILES} ${IMAGE_FILES} ${ICON_FILES})
  set(APP_TARGET MyFiles)
endif()

target_link_libraries(${APP_TARGET} PRIVATE myfiles_core SDL3_ttf::SDL3_ttf SDL3_image::SDL3_image)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(myfiles_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_compile_options(${APP_TARGET} PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(IOS)
  set_source_files_properties(${FONT_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION "fonts")
  set_source_files_properties(${IMAGE_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION "images")
  set_target_properties(MyFiles PROPERTIES RESOURCE "${ICON_FILES}")
  set_target_properties(MyFiles PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER "${MYFILES_BUNDLE_ID}"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${MYFILES_BUNDLE_ID}"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${MYFILES_TEAM_ID}"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "16.0"
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon")
  target_compile_options(${APP_TARGET} PRIVATE $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
  target_link_libraries(${APP_TARGET} PRIVATE
    "-framework UIKit" "-framework Vision" "-framework VisionKit" "-framework AVFoundation" "-framework Foundation")
elseif(APPLE)
  set_source_files_properties(${FONT_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/fonts")
  set_source_files_properties(${IMAGE_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/images")
  set_source_files_properties(${ICON_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
  set_target_properties(MyFiles PROPERTIES MACOSX_BUNDLE_ICON_FILE MyFiles.icns)
  find_package(CURL REQUIRED)
  target_link_libraries(${APP_TARGET} PRIVATE CURL::libcurl)
elseif(NOT ANDROID)
  find_package(CURL REQUIRED)
  target_link_libraries(${APP_TARGET} PRIVATE CURL::libcurl)
  file(COPY ${FONT_FILES} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/fonts)
  file(COPY ${IMAGE_FILES} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/images)
endif()

if(ANDROID)
  target_link_libraries(${APP_TARGET} PRIVATE android log)
endif()

# ---- tests ------------------------------------------------------------------------------------------

if(MYFILES_TESTS AND NOT IOS AND NOT ANDROID)
  enable_testing()
  add_executable(test_core tests/test_core.c)
  target_link_libraries(test_core PRIVATE myfiles_core)
  add_test(NAME core COMMAND test_core)

  # End-to-end: the sync engine against a running backend (skips itself if MYFILES_TEST_SERVER is unset).
  find_package(CURL REQUIRED)
  add_executable(test_sync tests/test_sync.c src/platform/http_curl.c)
  target_link_libraries(test_sync PRIVATE myfiles_core CURL::libcurl)
  add_test(NAME sync COMMAND test_sync)
endif()
