# macOS build of Toolscreen. Included from the top-level CMakeLists.txt after
# project(); builds Toolscreen.dylib from the shared sources plus the macOS
# platform layer in this directory.

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version" FORCE)
endif()

set(TOOLSCREEN_MACOS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos")
set(TOOLSCREEN_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(TOOLSCREEN_GENERATED_VERSION_HEADER "${TOOLSCREEN_GENERATED_DIR}/version.h")
file(MAKE_DIRECTORY "${TOOLSCREEN_GENERATED_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/version.h.in"
    "${TOOLSCREEN_GENERATED_VERSION_HEADER}"
    @ONLY
)

include(FetchContent)

FetchContent_Declare(
    glew
    URL https://github.com/nigels-com/glew/releases/download/glew-2.2.0/glew-2.2.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui
    GIT_TAG v1.92.6
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib
    GIT_TAG v0.40.0
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.4.350
    GIT_SHALLOW TRUE
)

foreach(dep glew imgui nlohmann_json cpp_httplib vulkan_headers)
    FetchContent_GetProperties(${dep})
    if(NOT ${dep}_POPULATED)
        FetchContent_Populate(${dep})
    endif()
endforeach()

find_library(TS_COCOA Cocoa REQUIRED)
find_library(TS_OPENGL OpenGL REQUIRED)
find_library(TS_IOSURFACE IOSurface REQUIRED)
find_library(TS_CARBON Carbon REQUIRED)
find_library(TS_COREGRAPHICS CoreGraphics REQUIRED)
find_library(TS_COREFOUNDATION CoreFoundation REQUIRED)
find_library(TS_COREVIDEO CoreVideo REQUIRED)
find_library(TS_COREMEDIA CoreMedia REQUIRED)
find_library(TS_COREMEDIAIO CoreMediaIO REQUIRED)
find_library(TS_SCREENCAPTUREKIT ScreenCaptureKit REQUIRED)
find_library(TS_UTTYPES UniformTypeIdentifiers REQUIRED)
find_library(TS_WEBKIT WebKit REQUIRED)

set(TS_MAC_COMPILE_OPTIONS
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-unknown-pragmas
    -Wno-missing-braces
    -Wno-sign-compare
    -Wno-switch
    -Wno-microsoft-cast
    -Wno-nonportable-include-path
    -Wno-pragma-pack
    -Wno-unused-private-field
)

# --- GLEW ---------------------------------------------------------------------------
add_library(toolscreen_glew STATIC ${glew_SOURCE_DIR}/src/glew.c)
target_include_directories(toolscreen_glew PUBLIC ${glew_SOURCE_DIR}/include)
target_compile_definitions(toolscreen_glew PUBLIC GLEW_STATIC GLEW_NO_GLU)
target_compile_options(toolscreen_glew PRIVATE -fvisibility=hidden -w)
target_link_libraries(toolscreen_glew PUBLIC ${TS_OPENGL})

# --- Dear ImGui (same backends as Windows; the Win32 backend runs on the
#     Win32 compatibility layer) ---------------------------------------------------------
set(TOOLSCREEN_IMGUI_DIR "${imgui_SOURCE_DIR}")
add_library(toolscreen_imgui STATIC
    ${TOOLSCREEN_IMGUI_DIR}/imgui.cpp
    ${TOOLSCREEN_IMGUI_DIR}/imgui_draw.cpp
    ${TOOLSCREEN_IMGUI_DIR}/imgui_tables.cpp
    ${TOOLSCREEN_IMGUI_DIR}/imgui_widgets.cpp
    ${TOOLSCREEN_IMGUI_DIR}/imgui_demo.cpp
    ${TOOLSCREEN_IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
    ${TOOLSCREEN_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${TOOLSCREEN_IMGUI_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(toolscreen_imgui
    PUBLIC
        ${TOOLSCREEN_IMGUI_DIR}
        ${TOOLSCREEN_IMGUI_DIR}/backends
        ${TOOLSCREEN_IMGUI_DIR}/misc/cpp
    PRIVATE
        ${TOOLSCREEN_MACOS_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(toolscreen_imgui PUBLIC
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
    UNICODE
    _UNICODE
)
target_compile_options(toolscreen_imgui PRIVATE ${TS_MAC_COMPILE_OPTIONS})
target_link_libraries(toolscreen_imgui PUBLIC ${TS_OPENGL})

# --- Embedded resources (the RCDATA entries of src/platform/resource.rc) ---------------
set(TS_RESOURCE_RC "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/resource.rc")
set(TS_RESOURCE_H "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/resource.h")
set(TS_EMBEDDED_RESOURCES_CPP "${TOOLSCREEN_GENERATED_DIR}/embedded_resources.cpp")
file(STRINGS "${TS_RESOURCE_RC}" ts_rc_lines REGEX "RCDATA")
set(TS_RESOURCE_FILES)
foreach(line IN LISTS ts_rc_lines)
    if(line MATCHES "RCDATA[ \t]+\"([^\"]+)\"")
        get_filename_component(abs "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/${CMAKE_MATCH_1}" ABSOLUTE)
        list(APPEND TS_RESOURCE_FILES "${abs}")
    endif()
endforeach()
add_custom_command(
    OUTPUT "${TS_EMBEDDED_RESOURCES_CPP}"
    COMMAND ${CMAKE_COMMAND}
        -DRC_FILE=${TS_RESOURCE_RC}
        -DHEADER_FILE=${TS_RESOURCE_H}
        -DOUTPUT_FILE=${TS_EMBEDDED_RESOURCES_CPP}
        -P ${TOOLSCREEN_MACOS_DIR}/cmake/embed_resources.cmake
    DEPENDS ${TS_RESOURCE_RC} ${TS_RESOURCE_H} ${TS_RESOURCE_FILES} ${TOOLSCREEN_MACOS_DIR}/cmake/embed_resources.cmake
    COMMENT "Embedding Toolscreen resources"
    VERBATIM
)

# --- Toolscreen.dylib --------------------------------------------------------------------
set(TS_SHARED_DIRS bootstrap common config features gui gui/tabs hooks render runtime third_party)
set(TS_SOURCES)
foreach(dir IN LISTS TS_SHARED_DIRS)
    file(GLOB dir_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/${dir}/*.cpp")
    list(APPEND TS_SOURCES ${dir_sources})
endforeach()

# Windows-only modules; their macOS counterparts live in this directory.
list(FILTER TS_SOURCES EXCLUDE REGEX "/src/features/browser_overlay\\.cpp$")
list(FILTER TS_SOURCES EXCLUDE REGEX "/src/hooks/hook_chain\\.cpp$")

file(GLOB_RECURSE TS_MAC_SOURCES CONFIGURE_DEPENDS
    "${TOOLSCREEN_MACOS_DIR}/*.cpp"
    "${TOOLSCREEN_MACOS_DIR}/*.mm"
)
list(FILTER TS_MAC_SOURCES EXCLUDE REGEX "/bootstrap/macos_entry\\.cpp$")

# Sources compile as an object library that does not depend on the GLEW/ImGui
# library targets, so one build reports every compile error.
add_library(toolscreen_objects OBJECT
    ${TS_SOURCES}
    ${TS_MAC_SOURCES}
    ${TOOLSCREEN_MACOS_DIR}/third_party/fishhook/fishhook.c
    ${TS_EMBEDDED_RESOURCES_CPP}
    # Last source, so its initializer runs after every other one (see the file).
    ${TOOLSCREEN_MACOS_DIR}/bootstrap/macos_entry.cpp
)

# The macOS include directory comes first so <Windows.h> and <GL/glew.h>
# resolve to the compatibility headers.
target_include_directories(toolscreen_objects PRIVATE
    ${TOOLSCREEN_MACOS_DIR}/include
    ${TOOLSCREEN_GENERATED_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${nlohmann_json_SOURCE_DIR}/single_include
    ${cpp_httplib_SOURCE_DIR}
    ${vulkan_headers_SOURCE_DIR}/include
    ${TOOLSCREEN_IMGUI_DIR}
    ${TOOLSCREEN_IMGUI_DIR}/backends
    ${TOOLSCREEN_IMGUI_DIR}/misc/cpp
    ${glew_SOURCE_DIR}/include
)

set(TS_COMPILE_DEFINITIONS
    TOOLSCREEN_EXPORTS
    TOOLSCREEN_MACOS
    CMAKE_BUILD
    GLEW_STATIC
    GLEW_NO_GLU
    GL_SILENCE_DEPRECATION
    NOMINMAX
    UNICODE
    _UNICODE
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
)
target_compile_definitions(toolscreen_objects PRIVATE ${TS_COMPILE_DEFINITIONS})
# DLL_PROCESS_DETACH at process exit leaves Toolscreen's threads running
# because Windows has already ended them. On macOS they still run while exit()
# destroys globals, so a joinable global std::thread aborts the process.
# Toolscreen's globals are therefore never destroyed.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-fc++-static-destructors=thread-local" TS_HAS_THREAD_LOCAL_ONLY_DESTRUCTORS)
if(TS_HAS_THREAD_LOCAL_ONLY_DESTRUCTORS)
    set(TS_NO_STATIC_DESTRUCTORS "-fc++-static-destructors=thread-local")
else()
    set(TS_NO_STATIC_DESTRUCTORS "-fno-c++-static-destructors")
endif()

target_compile_options(toolscreen_objects PRIVATE
    ${TS_MAC_COMPILE_OPTIONS}
    "$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:${TS_NO_STATIC_DESTRUCTORS}>"
    -ferror-limit=0
    # Fills C++20 library gaps in Apple's libc++ (see the header).
    "$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:SHELL:-include \"${TOOLSCREEN_MACOS_DIR}/include/ts_macos_prelude.h\">"
)
# The shared sources rely on MSVC conversions (e.g. function pointer to void*).
set_source_files_properties(${TS_SOURCES} PROPERTIES COMPILE_OPTIONS "-fms-extensions;-Wno-microsoft-cast;-Wno-microsoft-template;-Wno-microsoft-goto;-Wno-microsoft-exception-spec;-Wno-ignored-attributes;-Wno-keyword-macro")
set(TS_MAC_OBJC_SOURCES ${TS_MAC_SOURCES})
list(FILTER TS_MAC_OBJC_SOURCES INCLUDE REGEX "\\.mm$")
set_source_files_properties(${TS_MAC_OBJC_SOURCES} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
set_source_files_properties(
    ${TOOLSCREEN_MACOS_DIR}/third_party/fishhook/fishhook.c
    PROPERTIES COMPILE_OPTIONS "-w"
)

add_library(Toolscreen SHARED $<TARGET_OBJECTS:toolscreen_objects>)

set_target_properties(Toolscreen PROPERTIES
    LINKER_LANGUAGE CXX
    PREFIX ""
    SUFFIX ".dylib"
    OUTPUT_NAME "Toolscreen"
    INSTALL_NAME_DIR "@rpath"
    BUILD_WITH_INSTALL_NAME_DIR ON
)

target_link_libraries(Toolscreen PRIVATE
    toolscreen_glew
    toolscreen_imgui
    ${TS_COCOA}
    ${TS_OPENGL}
    ${TS_IOSURFACE}
    ${TS_CARBON}
    ${TS_COREGRAPHICS}
    ${TS_COREFOUNDATION}
    ${TS_COREVIDEO}
    ${TS_COREMEDIA}
    ${TS_COREMEDIAIO}
    ${TS_SCREENCAPTUREKIT}
    ${TS_UTTYPES}
    ${TS_WEBKIT}
)

# --- liblogger --------------------------------------------------------------------------
# The published, verification-approved liblogger.dylib from the upstream
# "liblogger-legal" release is shipped unmodified.
set(TS_LIBLOGGER_URL "https://github.com/jojoe77777/Toolscreen/releases/download/liblogger-legal/liblogger.dylib")
set(TS_LIBLOGGER_DYLIB "${CMAKE_CURRENT_BINARY_DIR}/liblogger/liblogger.dylib")
if(NOT EXISTS "${TS_LIBLOGGER_DYLIB}")
    file(DOWNLOAD "${TS_LIBLOGGER_URL}" "${TS_LIBLOGGER_DYLIB}" STATUS ts_liblogger_status TLS_VERIFY ON)
    list(GET ts_liblogger_status 0 ts_liblogger_code)
    if(NOT ts_liblogger_code EQUAL 0)
        file(REMOVE "${TS_LIBLOGGER_DYLIB}")
        message(WARNING "Could not download liblogger.dylib: ${ts_liblogger_status}")
    endif()
endif()

# --- package -----------------------------------------------------------------------------
# Toolscreen-<version>-macos.jar: the installer with both dylibs embedded.
find_package(Java 17 COMPONENTS Development REQUIRED)
set(TS_PACKAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/package")
set(TS_INSTALLER_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/installer")
set(TS_INSTALLER_JAR "${TS_PACKAGE_DIR}/Toolscreen-${TOOLSCREEN_VERSION}-macos.jar")
set(TS_INSTALLER_VERSION_FILE "${CMAKE_CURRENT_BINARY_DIR}/installer_version.txt")
file(WRITE "${TS_INSTALLER_VERSION_FILE}" "${TOOLSCREEN_VERSION}")
add_custom_target(macos_package
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${TS_INSTALLER_BUILD_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${TS_INSTALLER_BUILD_DIR}/classes/payload" "${TS_PACKAGE_DIR}"
    COMMAND ${Java_JAVAC_EXECUTABLE} --release 17 -encoding UTF-8 -d "${TS_INSTALLER_BUILD_DIR}/classes"
        "${TOOLSCREEN_MACOS_DIR}/installer/Installer.java"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:Toolscreen>" "${TS_INSTALLER_BUILD_DIR}/classes/payload/Toolscreen.dylib"
    COMMAND ${CMAKE_COMMAND} -E copy "${TS_LIBLOGGER_DYLIB}" "${TS_INSTALLER_BUILD_DIR}/classes/payload/liblogger.dylib"
    COMMAND ${CMAKE_COMMAND} -E copy "${TS_INSTALLER_VERSION_FILE}" "${TS_INSTALLER_BUILD_DIR}/classes/payload/version.txt"
    COMMAND ${Java_JAR_EXECUTABLE} --create --file "${TS_INSTALLER_JAR}" --main-class com.toolscreen.macos.Installer
        -C "${TS_INSTALLER_BUILD_DIR}/classes" .
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Toolscreen>" "${TS_PACKAGE_DIR}/Toolscreen.dylib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TS_LIBLOGGER_DYLIB}" "${TS_PACKAGE_DIR}/liblogger.dylib"
    DEPENDS Toolscreen "${TOOLSCREEN_MACOS_DIR}/installer/Installer.java"
    COMMENT "Building the macOS installer"
    VERBATIM
)
