cmake_minimum_required (VERSION 3.8)
enable_testing()

project("Piper2")
set(CMAKE_CXX_STANDARD 17)
add_compile_definitions("UNICODE")
add_compile_definitions("_UNICODE")
add_compile_options("/utf-8")
#DEBUG
#add_link_options("/PROFILE")

function(set_output_path REPATH)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Lib/${REPATH} PARENT_SCOPE)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Lib/${REPATH} PARENT_SCOPE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Bin/${REPATH} PARENT_SCOPE)
endfunction()

add_subdirectory("Src")

install(FILES LICENSE DESTINATION ./Licenses)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/Licenses DESTINATION ./)
install(DIRECTORY ${CMAKE_BINARY_DIR}/Lib DESTINATION ./)
install(DIRECTORY ${CMAKE_BINARY_DIR}/Bin DESTINATION ./)
