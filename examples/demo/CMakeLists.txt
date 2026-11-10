# The demo application: a consumer of libmyrcp. Inside myrcp's build (-DMYRCP_DEMO=ON)
# it links the library being built; on its own it finds an installed myrcp:
#
#   cmake -S examples/demo -B build-demo && cmake --build build-demo
cmake_minimum_required(VERSION 3.24)
project(myrcp-demo LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT TARGET myrcp::myrcp)
  find_package(myrcp REQUIRED)
endif()

add_executable(myrcp-demo main.cpp)
target_link_libraries(myrcp-demo PRIVATE myrcp::myrcp)
target_compile_definitions(myrcp-demo PRIVATE MYRCP_DEMO_PLUGINS="${CMAKE_CURRENT_SOURCE_DIR}/plugins")

if(TARGET myrcp::myrcp-gtk)
  add_executable(myrcp-demo-gtk main.cpp)
  target_link_libraries(myrcp-demo-gtk PRIVATE myrcp::myrcp-gtk)
  target_compile_definitions(myrcp-demo-gtk PRIVATE MYRCP_DEMO_PLUGINS="${CMAKE_CURRENT_SOURCE_DIR}/plugins"
                                                    MYRCP_DEMO_GTK=1)
endif()
