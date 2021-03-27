project(pegtl-test)
cmake_minimum_required(VERSION 3.16)

# find_package(pegtl CONFIG REQUIRED)
add_subdirectory(PEGTL)

add_executable(pegtl-test main.cpp)

target_link_libraries(pegtl-test
    PRIVATE taocpp::pegtl
)
