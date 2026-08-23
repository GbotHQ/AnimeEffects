# Regression test for the BoneKey::deserialize use-after-free (dangling
# &mBindingCaches.back() captured into a deferred IDSolver lambda that runs
# after the QList reallocates). Self-contained; see bonekey_regression.cpp.
add_executable(bonekey_regression bonekey_regression.cpp)
target_include_directories(bonekey_regression PRIVATE ..)
target_compile_definitions(bonekey_regression PRIVATE
    AE_MAJOR_VERSION=1
    AE_MINOR_VERSION=7
    AE_MICRO_VERSION=1
    USE_GL_CORE_PROFILE
)
target_link_libraries(bonekey_regression PRIVATE
    Qt6::Core Qt6::Core5Compat Qt6::Widgets
    Qt6::Gui Qt6::OpenGL Qt6::OpenGLWidgets
    Qt6::Svg Qt6::Xml Qt6::Concurrent Qt6::Multimedia
    cmnd core ctrl gl img thr util pugixml
)

# Round-trip project loader/saver harness (open -> serialize -> reload -> close).
# Takes .anie paths on the command line; used to exercise real project files.
add_executable(roundtrip roundtrip_main.cpp)
target_include_directories(roundtrip PRIVATE ..)
target_compile_definitions(roundtrip PRIVATE
    AE_MAJOR_VERSION=1
    AE_MINOR_VERSION=7
    AE_MICRO_VERSION=1
    USE_GL_CORE_PROFILE
)
target_link_libraries(roundtrip PRIVATE
    Qt6::Core Qt6::Core5Compat Qt6::Widgets
    Qt6::Gui Qt6::OpenGL Qt6::OpenGLWidgets
    Qt6::Svg Qt6::Xml Qt6::Concurrent Qt6::Multimedia
    cmnd core ctrl gl img thr util pugixml
)

# Right-click key selection: an unselected key becomes the selection for
# the context menu; also pins the surface->model coordinate mapping the
# widget must apply to context-menu positions. See
# rightclick_transient_selection.cpp. Compiles the real theme::TimeLine so
# ctrl's renderer links without pulling in the whole gui library.
add_executable(rightclick_transient_selection rightclick_transient_selection.cpp
    ../src/gui/theme/TimeLine.cpp)
target_include_directories(rightclick_transient_selection PRIVATE ..)
target_compile_definitions(rightclick_transient_selection PRIVATE
    AE_MAJOR_VERSION=1
    AE_MINOR_VERSION=7
    AE_MICRO_VERSION=1
    USE_GL_CORE_PROFILE
)
target_link_libraries(rightclick_transient_selection PRIVATE
    Qt6::Core Qt6::Core5Compat Qt6::Widgets
    Qt6::Gui Qt6::OpenGL Qt6::OpenGLWidgets
    Qt6::Svg Qt6::Xml Qt6::Concurrent Qt6::Multimedia
    cmnd core ctrl gl img thr util pugixml
)

# Regression test for the custom-bezier easing solver swap (WebKit UnitBezier
# port) and its control-point solver cache. See easing_regression.cpp.
find_package(Threads REQUIRED)
add_executable(easing_regression easing_regression.cpp)
target_include_directories(easing_regression PRIVATE ..)
target_compile_definitions(easing_regression PRIVATE
    AE_MAJOR_VERSION=1
    AE_MINOR_VERSION=7
    AE_MICRO_VERSION=1
    USE_GL_CORE_PROFILE
)
target_link_libraries(easing_regression PRIVATE
    Qt6::Core Qt6::Core5Compat Qt6::Widgets
    Qt6::Gui Qt6::OpenGL Qt6::OpenGLWidgets
    Qt6::Svg Qt6::Xml Qt6::Concurrent Qt6::Multimedia
    util
    Threads::Threads
)
