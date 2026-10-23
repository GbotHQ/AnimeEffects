cmake_minimum_required(VERSION 3.16)
project(AnimeEffects VERSION 1.7.1 LANGUAGES C CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
# AddressSanitizer (with LeakSanitizer) opt-in: pretty bad perf hit.
# The known process-lifetime caches (fontconfig/Pango/GTK3 via the Qt
# gtk3 platform theme) are suppressed in-code via __lsan_default_suppressions()
# (see src/gui/LeakSuppressions.cpp), so no env vars or run scripts are
# needed — configure with -DAE_ENABLE_ASAN=ON and run the binary directly.
option(AE_ENABLE_ASAN "Build with AddressSanitizer and LeakSanitizer" OFF)
if(AE_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

include(GNUInstallDirs)
# You need to add the Qt directory to your path, otherwise you'll have a very bad time
# On MacOS Tahoe please remove AGL from the framework list when linking
# QuickControls2 is here for the migrated Qt Quick dialogs: their controls are
# QtQuick.Controls types with the design's own background/contentItem delegates.
# The Basic style is selected in Main.cpp (the default on Linux is Fusion, and
# Basic is the documented base for a customised look).
find_package(Qt6 REQUIRED COMPONENTS Core5Compat Core Gui OpenGL OpenGLWidgets Svg Xml Concurrent Widgets Multimedia Quick Qml QuickWidgets QuickControls2 LinguistTools Test)
# Qt's i18n CMake support: declares the translated languages lupdate/lrelease
# operate on, and enables the qt_add_translations/update_translations targets.
# The translation sources live in assets/locale/translation_<locale>.ts.
qt_standard_project_setup(I18N_TRANSLATED_LANGUAGES es ja ko ru zh)

# ---------------------------------------------------------------------------
# Warning policy for first-party code.
#
# This project compiled with no warning flags at all, which is why a test
# section could be defined and never called (`-Wunused-function` is not on by
# default; it needs -Wall) and why the compiler had nothing to say about it.
# The policy is a function each first-party directory calls explicitly rather
# than a global add_compile_options, so that:
#   - the flag set has exactly one definition, and
#   - vendored subprojects (src/deps/pugixml) do not inherit our flags and get
#     linted as if the warnings were ours.
#
# Deliberately NOT -Werror: this codebase is built by several compilers, and
# promoting warnings to errors turns a compiler difference into a broken build.
# The point is to see the warnings, not to be blocked by them.
#
# MSVC is not enabled here. It IS a supported path (the Windows CI uses
# win64_msvc2022_64, whose equivalent level is /W4), but that path cannot be
# verified from this machine, and switching on a warning level blind would put
# unverified noise into CI. Left as a documented gap rather than a guess.
# ---------------------------------------------------------------------------
function(ae_enable_warnings)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-Wall -Wextra)
    endif()
endfunction()

# Get sources
# ---------------------------------------------------------------------------
# Dear ImGui: the renderer of the viewport overlay (the animation graph panel).
#
# WHY A TAG AND NOT A BRANCH: v1.92.7 is the release the sibling project on this
# machine already carries (commit dac0719), so the two agree; GIT_SHALLOW keeps
# the fetch to one commit (22 MB in the build tree). The sources stay OUT of this
# repository - a pinned release is fetched into build/_deps.
#
# OFFLINE / HERMETIC BUILDS: FetchContent honours
# -DFETCHCONTENT_SOURCE_DIR_IMGUI=<an existing imgui checkout> and then fetches
# nothing at all (measured: configures in the same time as with the option OFF,
# and creates no _deps directory).
#
# ImGui ships no CMakeLists.txt upstream, so FetchContent_MakeAvailable only
# populates it: no add_subdirectory, and there is no find_package for it to need.
#
# ONE LIBRARY FOR THE APP AND THE TESTS: ae_imgui compiles the four core sources
# (imgui_demo.cpp, the demo window, is left out). The GL3 backend is deliberately
# NOT here - it is compiled inside the gui target through ImGuiSpikeGL.cpp,
# because that translation unit's loader shim must precede it (see
# src/gui/CMakeLists.txt).
# ---------------------------------------------------------------------------
option(AE_ENABLE_IMGUI "Build the Dear ImGui viewport overlay (the animation graph panel)" ON)

if(AE_ENABLE_IMGUI)
    include(FetchContent)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.92.7
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(imgui)

    add_library(ae_imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    )
    target_include_directories(ae_imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
endif()

# ---------------------------------------------------------------------------
# Qt Canvas Painter (Qt 6.12+): the RHI-backed 2D canvas the timeline and the
# animation-graph panel migrate to. OFF unless the Qt in use actually provides it:
# the module ships in 6.12, and QRhiWidget's xcb composite path crashes on 6.11.2
# (AGENTS.md, "Canvas Painter on this machine"). Fedora 44's Qt is 6.11.2, so the
# default build here does NOT get it; a 6.12 toolchain (aqt/Qt installer) turns it on.
# ---------------------------------------------------------------------------
option(AE_ENABLE_CANVAS_PAINTER "Build the Canvas Painter renderer (needs Qt >= 6.12)" ON)
set(AE_CANVAS_PAINTER 0)
if(AE_ENABLE_CANVAS_PAINTER)
    find_package(Qt6 QUIET COMPONENTS CanvasPainter)
    if(Qt6CanvasPainter_FOUND AND Qt6Core_VERSION VERSION_GREATER_EQUAL 6.12)
        set(AE_CANVAS_PAINTER 1)
        message(STATUS "Canvas Painter: ON (Qt ${Qt6Core_VERSION})")
    else()
        message(STATUS "Canvas Painter: OFF (needs Qt >= 6.12; this is ${Qt6Core_VERSION})")
    endif()
endif()

add_subdirectory(src/util)
add_subdirectory(src/thr)
add_subdirectory(src/cmnd)
add_subdirectory(src/gl)
add_subdirectory(src/img)
add_subdirectory(src/core)
add_subdirectory(src/media)
add_subdirectory(src/ctrl)
add_subdirectory(src/gui)
add_subdirectory(src/deps/pugixml)
add_subdirectory(tests)

if(APPLE)
    set_target_properties( ${PROJECT_NAME}
            PROPERTIES
            MACOSX_BUNDLE_NAME                             ${PROJECT_NAME}
            MACOSX_BUNDLE_VERSION                          ${PROJECT_VERSION}
            MACOSX_BUNDLE_GUI_IDENTIFIER                   org.AnimeEffectsDevs.${PROJECT_NAME}
            MACOSX_BUNDLE_ICON_FILE                        AnimeEffects.icns
            MACOSX_BUNDLE                                  TRUE
            XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY             AnimeEffectsDevs
            XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED          NO
    )
endif ()