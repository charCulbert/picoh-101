cmake_minimum_required(VERSION 3.16)
project(picoh101_firmware_tests CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The firmware's parts that don't touch hardware, with the real synth (../synth).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/../../params/generate.py COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/../../params/parameters.json)
set(SYNTH ${CMAKE_CURRENT_LIST_DIR}/../../synth)
add_executable(tests tests.cpp ${SYNTH}/Synth.cpp ${SYNTH}/SynthParameters.cpp)
target_include_directories(tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/.. ${CMAKE_CURRENT_LIST_DIR}/../../protocol ${SYNTH}
                                         ${SYNTH}/elliptic-blep)
target_compile_options(tests PRIVATE -Wall -fsanitize=undefined)
target_link_options(tests PRIVATE -fsanitize=undefined)
add_executable(fast_math_lines fast_math_lines.cpp)
target_include_directories(fast_math_lines PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../protocol)
target_compile_options(fast_math_lines PRIVATE -O2 -ffast-math)
