cmake_minimum_required(VERSION 3.21 FATAL_ERROR)

project(spsc_queue VERSION 1.0 LANGUAGES CXX)

add_library(${PROJECT_NAME} INTERFACE)
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})
target_compile_features(${PROJECT_NAME} INTERFACE cxx_std_11)
target_include_directories(${PROJECT_NAME} INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    find_package(benchmark REQUIRED)
    find_path(READERWRITERQUEUE_INCLUDE_DIRS "readerwriterqueue/atomicops.h")
    find_path(SPSCQUEUE_INCLUDE_DIRS "rigtorp/SPSCQueue.h")

    add_executable(${PROJECT_NAME}_bench
        "src/benchmark.cpp"
        "src/platform-win.cpp")
    set_property(TARGET ${PROJECT_NAME}_bench PROPERTY CXX_STANDARD 23)
    target_include_directories(${PROJECT_NAME}_bench PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        ${READERWRITERQUEUE_INCLUDE_DIRS}
        ${SPSCQUEUE_INCLUDE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/drogalis/include")
    target_link_libraries(${PROJECT_NAME}_bench PRIVATE benchmark::benchmark)
    target_compile_definitions(${PROJECT_NAME}_bench PRIVATE MOODYCAMEL_CACHE_LINE_SIZE=128)
endif()

# Install

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion)

export(
    TARGETS ${PROJECT_NAME}
    NAMESPACE ${PROJECT_NAME}::
    FILE "${PROJECT_NAME}Config.cmake")

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    install(
        DIRECTORY "include/"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(
        TARGETS ${PROJECT_NAME}
        EXPORT "${PROJECT_NAME}Config"
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(
        EXPORT "${PROJECT_NAME}Config"
        NAMESPACE ${PROJECT_NAME}::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}")

    install(
        FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}")
endif()
