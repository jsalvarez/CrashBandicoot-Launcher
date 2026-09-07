cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK or pass -DCMAKE_TOOLCHAIN_FILE=<vitasdk>/share/vita.toolchain.cmake")
  endif()
endif()

project(crash_vita_bootstrap C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Crash Recomp Vita Bootstrap")
set(VITA_TITLEID "CRSH00001")
set(VITA_VERSION "01.00")
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME} src/main.c)
target_compile_options(${PROJECT_NAME} PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(${PROJECT_NAME} SceDisplay_stub SceCtrl_stub SceLibKernel_stub SceProcessmgr_stub)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
)
