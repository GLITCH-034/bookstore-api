cmake_minimum_required(VERSION 3.16)
project(BookStoreAPI)

set(CMAKE_CXX_STANDARD 17)

find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)

# nlohmann/json (header-only — put json.hpp in include/)
include_directories(include)

add_executable(bookstore_api main.cpp)

target_link_libraries(bookstore_api
    CURL::libcurl
    SQLite::SQLite3
    pthread
)

# Crow is header-only — just needs to be in include/
# Download: https://github.com/CrowCpp/Crow/releases
