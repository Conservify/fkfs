cmake_minimum_required(VERSION 3.5)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(.)
include_directories(..)

add_executable(read read.cpp hal.cpp ../fkfs.cpp)
add_executable(tester test.cpp hal.cpp ../fkfs.cpp)
