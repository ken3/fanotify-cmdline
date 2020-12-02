cmake_minimum_required(VERSION 3.17)
project(fanotify-cmdline C)

set(CMAKE_C_STANDARD 11)

add_executable(fanotify-cmdline
        fanotify-cmdline.c)