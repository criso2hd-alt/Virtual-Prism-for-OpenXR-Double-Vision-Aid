cmake_minimum_required(VERSION 3.20)
project(VirtualPrismOpenXR LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so users do not need the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_ALL_EXTENSIONS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)
FetchContent_Declare(OpenXR
  GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK-Source.git
  GIT_TAG release-1.1.63
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(OpenXR)

set(LAYER_NAME XR_APILAYER_NOVENDOR_virtual_prism)

# ---- API layer (injected into every OpenXR app by the loader) ----
add_library(${LAYER_NAME} SHARED src/layer/layer.cpp)
target_include_directories(${LAYER_NAME} PRIVATE
  src/common)
target_link_libraries(${LAYER_NAME} PRIVATE OpenXR::headers)
target_compile_options(${LAYER_NAME} PRIVATE /utf-8)
target_compile_definitions(${LAYER_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

# ---- Settings + VR calibration app ----
add_executable(VirtualPrism WIN32
  src/app/main.cpp src/app/vr_calibration.cpp src/app/scenes.cpp src/app/app.rc)
target_include_directories(VirtualPrism PRIVATE
  src/common)
target_compile_options(VirtualPrism PRIVATE /utf-8)
target_compile_definitions(VirtualPrism PRIVATE NOMINMAX UNICODE _UNICODE)
target_link_libraries(VirtualPrism PRIVATE openxr_loader
  d3d11 dxgi gdiplus comctl32 user32 gdi32 advapi32 shell32 ole32)

# The manifest sits next to the layer DLL / exe.
add_custom_command(TARGET VirtualPrism POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/src/layer/${LAYER_NAME}.json $<TARGET_FILE_DIR:VirtualPrism>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:${LAYER_NAME}> $<TARGET_FILE_DIR:VirtualPrism>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:openxr_loader> $<TARGET_FILE_DIR:VirtualPrism>)
add_dependencies(VirtualPrism ${LAYER_NAME})
