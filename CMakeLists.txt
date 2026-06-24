cmake_minimum_required(VERSION 4.2)
project(SendLighting)

set(CMAKE_CXX_STANDARD 20)

include(FetchContent)

FetchContent_Declare(ftxui
        GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
        GIT_TAG main
)

FetchContent_MakeAvailable(ftxui)

add_executable(SendLighting main.cpp)

target_link_libraries(SendLighting
        PRIVATE ftxui::screen
        PRIVATE ftxui::dom
        PRIVATE ftxui::component
)