cmake_minimum_required(VERSION 3.20)
project(meshwave C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Find Python for the embed script
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Embed index.html as a C string header
add_custom_command(
  OUTPUT  ${CMAKE_BINARY_DIR}/web_bundle.h
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/embed_html.py
          ${CMAKE_SOURCE_DIR}/web/index.html
          ${CMAKE_BINARY_DIR}/web_bundle.h
  DEPENDS ${CMAKE_SOURCE_DIR}/web/index.html
          ${CMAKE_SOURCE_DIR}/scripts/embed_html.py
  COMMENT "Embedding index.html into web_bundle.h"
)

add_executable(meshwave
  src/main.cpp
  src/server.c
  src/client.c
  src/discovery.c
  src/transfer.c
  src/http.cpp
  src/util.c
  ${CMAKE_BINARY_DIR}/web_bundle.h
)

target_include_directories(meshwave PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_BINARY_DIR}
)

target_compile_options(meshwave PRIVATE -Wall -Wextra -O2)

# Link threading and socket libraries
if(WIN32)
  # MSYS2/MinGW: ws2_32 for Winsock, pthread is provided by MinGW
  target_link_libraries(meshwave PRIVATE ws2_32 pthread)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(meshwave PRIVATE Threads::Threads)
endif()
