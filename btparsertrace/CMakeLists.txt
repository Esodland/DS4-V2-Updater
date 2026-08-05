cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Define VITASDK to the VitaSDK directory")
  endif()
endif()

project(btparsertrace C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -Wextra -O2 -std=gnu99")
add_executable(${PROJECT_NAME} main.c)

target_link_libraries(${PROJECT_NAME}
  gcc
  taihenForKernel_stub
  SceThreadmgrForDriver_stub
  SceIofilemgrForDriver_stub
  SceSysclibForDriver_stub
)

set_target_properties(${PROJECT_NAME}
  PROPERTIES LINK_FLAGS "-nostdlib"
  COMPILE_FLAGS "-D__VITA_KERNEL__"
)

vita_create_self(${PROJECT_NAME}.skprx ${PROJECT_NAME}
  CONFIG ${PROJECT_NAME}.yml
  UNSAFE
)
