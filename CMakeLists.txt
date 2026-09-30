cmake_minimum_required(VERSION 3.20)
project(obs-pdf-presenter VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_GENERATOR MATCHES "Visual Studio")
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
  endif()
endif()

# 需要 OBS 提供的 libobs 包（find_package 会搜索 libobs_DIR / CMAKE_PREFIX_PATH）
find_package(libobs REQUIRED)

add_library(obs-pdf-presenter MODULE src/obs-pdf-presenter.cpp)
target_link_libraries(obs-pdf-presenter PRIVATE OBS::libobs)

target_compile_definitions(obs-pdf-presenter PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)

set_target_properties(obs-pdf-presenter PROPERTIES
  PREFIX ""
  OUTPUT_NAME "obs-pdf-presenter"
)

if(WIN32)
  set(PLUGIN_BIN_DIR "bin/64bit")
else()
  set(PLUGIN_BIN_DIR "bin")
endif()

install(TARGETS obs-pdf-presenter RUNTIME DESTINATION "${PLUGIN_BIN_DIR}")
install(DIRECTORY data/locale DESTINATION "data")
