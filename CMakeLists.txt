cmake_minimum_required(VERSION 3.28)
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

# --- CLIP (Cross-Platform Clipboard) ---
FetchContent_Declare(
        clip
        GIT_REPOSITORY https://github.com/dacap/clip.git
        GIT_TAG        main
)
FetchContent_MakeAvailable(clip)

# --- XXHASH ---

set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
        xxhash
        GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
        GIT_TAG        v0.8.3
)
FetchContent_MakeAvailable(xxhash)

# --- Executable ---
add_executable(FileLighting main.cpp crypto_utils.cpp)

target_link_libraries(FileLighting
        PRIVATE ftxui::screen
        PRIVATE ftxui::dom
        PRIVATE ftxui::component
        PRIVATE asio_lib
        PRIVATE sodium
        PRIVATE clip
        PRIVATE xxhash
)

if(WIN32)
    # 1. Link Windows networking libraries (Required for ASIO)
    target_link_libraries(FileLighting PRIVATE ws2_32 wsock32)

    # 2. Statically link C/C++ runtimes if using GCC/Clang
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(FileLighting PRIVATE
                "-static"
                "-static-libgcc"
                "-static-libstdc++"
        )
    endif()
endif()