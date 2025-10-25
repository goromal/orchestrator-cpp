cmake_minimum_required (VERSION 3.16)

set(PROJ_NAME orchestrator)
project(${PROJ_NAME})

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(BUILD_TESTS "Build Tests" ON)
option(SECONDARY_SANITIZERS "Run Secondary Sanitizers" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20")

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} \
                             -pthread")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  if (SECONDARY_SANITIZERS)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
  else()
    add_compile_options(-fsanitize=address -fsanitize=undefined)
    add_link_options(-fsanitize=address -fsanitize=undefined)
  endif()
  if(MSVC)
    add_compile_options(/W4 /WX)
  else()
    add_compile_options(-Wall -Wextra -pedantic -Werror)
  endif()
endif()

find_package(mscpp REQUIRED)
find_package(Boost REQUIRED COMPONENTS program_options log log_setup thread system date_time filesystem)

set(LIB_DEPS mscpp protobuf)
set(LIB_INCS $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)

add_library(${PROJ_NAME}
  src/internal/common.cpp
  src/JobQueue.cpp
)
target_include_directories(${PROJ_NAME} PUBLIC ${LIB_INCS})
target_link_libraries(${PROJ_NAME} PUBLIC ${LIB_DEPS})

add_executable(${PROJ_NAME}d
  src/main.cpp
)
target_include_directories(${PROJ_NAME}d
  PRIVATE
  ${Boost_INCLUDE_DIR}
)
target_link_libraries(${PROJ_NAME}d
  ${PROJ_NAME}
  ${Boost_LIBRARIES}
)

if (BUILD_TESTS)
    add_executable(test-jobqueue
        src/internal/common.cpp
        src/JobQueue.cpp
        src/mocks/JobExecutorMock.cpp
        src/mocks/JobDatabaseMock.cpp
        tests/JobQueueTest.cpp
    )
    target_include_directories(test-jobqueue PUBLIC ${LIB_INCS})
    target_link_libraries(test-jobqueue boost_unit_test_framework ${Boost_LIBRARIES} ${LIB_DEPS})
    add_test(NAME test-jobqueue COMMAND test-jobqueue)

    add_custom_command(
        TARGET test-jobqueue
        COMMENT "Run JobQueue unit tests"
        POST_BUILD
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMAND test-jobqueue
    )
endif()

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/${PROJ_NAME}ConfigVersion.cmake"
    VERSION 0.0
    COMPATIBILITY AnyNewerVersion
)

install(TARGETS ${PROJ_NAME}
    EXPORT ${PROJ_NAME}Targets
    LIBRARY DESTINATION lib COMPONENT Runtime
    ARCHIVE DESTINATION lib COMPONENT Development
    RUNTIME DESTINATION bin COMPONENT Runtime
    PUBLIC_HEADER DESTINATION include COMPONENT Development
    BUNDLE DESTINATION bin COMPONENT Runtime
)

install(TARGETS ${PROJ_NAME}d
    EXPORT ${PROJ_NAME}Targets
    LIBRARY DESTINATION lib COMPONENT Runtime
    ARCHIVE DESTINATION lib COMPONENT Development
    RUNTIME DESTINATION bin COMPONENT Runtime
    PUBLIC_HEADER DESTINATION include COMPONENT Development
    BUNDLE DESTINATION bin COMPONENT Runtime
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/${PROJ_NAME}Config.cmake.in"
    "${PROJECT_BINARY_DIR}/${PROJ_NAME}Config.cmake"
    INSTALL_DESTINATION lib/cmake/${PROJ_NAME}
)

install(EXPORT ${PROJ_NAME}Targets DESTINATION lib/cmake/${PROJ_NAME})
install(FILES "${PROJECT_BINARY_DIR}/${PROJ_NAME}ConfigVersion.cmake"
              "${PROJECT_BINARY_DIR}/${PROJ_NAME}Config.cmake"
        DESTINATION lib/cmake/${PROJ_NAME})
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/ DESTINATION include)
