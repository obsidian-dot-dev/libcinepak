cmake_minimum_required(VERSION 3.14)
project(cinepak C)

add_library(cinepak STATIC cinepak.c)
target_include_directories(cinepak PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(cinepak PROPERTIES
    C_STANDARD          99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS        OFF)
