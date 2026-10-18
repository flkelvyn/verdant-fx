# Verdant FX — build (MSVC x64 recomendado). Na Linux só o alvo test_config é construído.
cmake_minimum_required(VERSION 3.20)
project(VerdantFX VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(vfx_config STATIC src/config.cpp)
target_include_directories(vfx_config PUBLIC src)

enable_testing()
add_executable(test_config tests/test_config.cpp)
target_link_libraries(test_config PRIVATE vfx_config)
add_test(NAME config COMMAND test_config)

if(WIN32)
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "O ETS2 é 64 bits: compile em x64.")
  endif()
  add_library(verdantfx_dxgi SHARED src/dxgi_proxy.cpp src/dxgi.def)
  # NÃO linkar dxgi.lib (importaria o próprio proxy). dxguid fornece só os GUIDs estáticos.
  target_link_libraries(verdantfx_dxgi PRIVATE vfx_config d3d11 dxguid)
  set_target_properties(verdantfx_dxgi PROPERTIES PREFIX "" OUTPUT_NAME "dxgi")
  if(MSVC)
    target_compile_options(verdantfx_dxgi PRIVATE /W4 /permissive-)
  endif()
  # pacote: dxgi.dll + VerdantFX/{config.ini,shaders/}
  install(TARGETS verdantfx_dxgi RUNTIME DESTINATION .)
  install(FILES config/config.ini DESTINATION VerdantFX)
  install(FILES shaders/vegetation_sharpen.hlsl DESTINATION VerdantFX/shaders)
endif()
