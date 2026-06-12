cmake_minimum_required(VERSION 3.16)
project(hydro_cavitation_audit VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/O2 /arch:AVX2 /fp:fast /GL /GS- /utf-8)
    add_link_options(/LTCG)
else()
    add_compile_options(-O3 -march=native -ffast-math -flto)
    add_link_options(-flto)
endif()

add_executable(cavitation_audit
    src/main.cpp
    src/ae_dag_stream.cpp
    src/wpd_kernel.cpp
    src/energy_matrix.cpp
    src/palmgren_miner.cpp
    src/life_prediction_report.cpp
)

target_include_directories(cavitation_audit PRIVATE src)
