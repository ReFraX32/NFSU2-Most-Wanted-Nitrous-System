cmake_minimum_required(VERSION 3.10)
project(NFSU2MostWantedNitrousSystem)

set(CMAKE_CXX_STANDARD 17)

if(MSVC)
    set(CMAKE_GENERATOR_PLATFORM Win32)
endif()

add_library(NFSU2MostWantedNitrousSystem SHARED src/DllMain.cpp)

target_compile_definitions(NFSU2MostWantedNitrousSystem PRIVATE UNICODE)