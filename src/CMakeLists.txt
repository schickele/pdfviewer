cmake_minimum_required(VERSION 3.21)
project(pdfviewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network WebEngineWidgets PrintSupport)

add_executable(pdfviewer main.cpp)
target_link_libraries(pdfviewer PRIVATE Qt6::Widgets Qt6::Network Qt6::WebEngineWidgets Qt6::PrintSupport)

# Bundle PDF.js next to the binary (fetch it first with scripts/fetch-pdfjs.sh)
if(EXISTS "${CMAKE_SOURCE_DIR}/pdfjs/web/viewer.html")
    add_custom_target(copy_pdfjs ALL
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:pdfviewer>/pdfjs"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/pdfjs" "$<TARGET_FILE_DIR:pdfviewer>/pdfjs"
        VERBATIM)   # quote paths properly (spaces, parentheses, ...)
    add_dependencies(pdfviewer copy_pdfjs)
    install(DIRECTORY pdfjs DESTINATION share/pdfviewer)
else()
    message(WARNING "pdfjs/ not found - run scripts/fetch-pdfjs.sh and re-run cmake")
endif()

install(TARGETS pdfviewer RUNTIME DESTINATION bin)
install(FILES pdfviewer.desktop DESTINATION share/applications)
