cmake_minimum_required (VERSION 3.8)

set_output_path(Infrastructure/Config/)
add_library(NlohmannJson SHARED "NlohmannJson.cpp")
target_link_libraries(NlohmannJson PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)
