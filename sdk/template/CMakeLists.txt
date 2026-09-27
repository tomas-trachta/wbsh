cmake_minimum_required(VERSION 3.15)
project(myutil C)

# Where wbshsdk.h lives: the folder holding this SDK's include\ directory.
# Defaults to the parent of this folder, which is right when the template
# is built where the SDK zip put it; set it when the project has moved.
set(WBSH_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "The wbsh SDK folder")

if(NOT EXISTS "${WBSH_SDK_DIR}/include/wbshsdk.h")
    message(FATAL_ERROR "wbshsdk.h not found under ${WBSH_SDK_DIR}/include; pass -DWBSH_SDK_DIR=<sdk folder>")
endif()

add_library(myutil SHARED myutil.c)
target_include_directories(myutil PRIVATE "${WBSH_SDK_DIR}/include")
target_compile_definitions(myutil PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_target_properties(myutil PROPERTIES PREFIX "" OUTPUT_NAME "myutil")

if(MSVC)
    target_compile_options(myutil PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(myutil PRIVATE -Wall -Wextra -Werror)
endif()
