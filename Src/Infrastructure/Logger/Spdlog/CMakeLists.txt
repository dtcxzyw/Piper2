cmake_minimum_required (VERSION 3.8)

find_package(spdlog CONFIG REQUIRED)

set_output_path(Infrastructure/Logger/Spdlog)
add_library(Spdlog SHARED "Spdlog.cpp")
target_link_libraries(Spdlog PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore spdlog::spdlog spdlog::spdlog_header_only)
