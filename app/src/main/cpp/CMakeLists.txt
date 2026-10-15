cmake_minimum_required(VERSION 3.22.1)
project(joycon_merge C)

# Eseguibile "travestito" da libreria: Android impacchetta solo file lib*.so
add_executable(joycon_merge joycon_merge.c)
set_target_properties(joycon_merge PROPERTIES
    OUTPUT_NAME "libjoycon_merge"
    SUFFIX ".so"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
)
