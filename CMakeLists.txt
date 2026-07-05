cmake_minimum_required(VERSION 4.2)
project(SendLighting)

set(CMAKE_CXX_STANDARD 20)

include(FetchContent)

# --- FTXUI ---
FetchContent_Declare(ftxui
        URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/heads/main.tar.gz
)
FetchContent_MakeAvailable(ftxui)

# --- ASIO ---
FetchContent_Declare(
    asio
    URL https://github.com/chriskohlhoff/asio/archive/refs/heads/master.tar.gz
    SOURCE_SUBDIR  none
)
FetchContent_MakeAvailable(asio)

add_library(asio_lib INTERFACE
        main.cpp)
target_include_directories(asio_lib INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio_lib INTERFACE ASIO_STANDALONE)

find_package(Threads REQUIRED)
target_link_libraries(asio_lib INTERFACE Threads::Threads)

# --- LIBSODIUM ---
FetchContent_Declare(
  libsodium
  GIT_REPOSITORY https://github.com/robinlinden/libsodium-cmake.git
  GIT_TAG        master
)
FetchContent_MakeAvailable(libsodium)

# --- Executable ---
add_executable(SendLighting main.cpp crypto_utils.cpp)

target_link_libraries(SendLighting
        PRIVATE ftxui::screen
        PRIVATE ftxui::dom
        PRIVATE ftxui::component
        PRIVATE asio_lib
        PRIVATE sodium
)