CMAKE_MINIMUM_REQUIRED(VERSION 3.0)
PROJECT(peelocpp_prompt CXX)

INCLUDE(GNUInstallDirs)

ADD_LIBRARY(
  peelocpp_prompt
  src/prompt.cpp
)

TARGET_COMPILE_OPTIONS(
  peelocpp_prompt
  PRIVATE
    -Wall -Werror
)

TARGET_COMPILE_FEATURES(
  peelocpp_prompt
  PRIVATE
    cxx_std_17
)

TARGET_INCLUDE_DIRECTORIES(
  peelocpp_prompt
  PUBLIC
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

INSTALL(
  TARGETS
    peelocpp_prompt
  EXPORT
    peelocpp_prompt-export
  LIBRARY DESTINATION
    ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION
    ${CMAKE_INSTALL_LIBDIR}
)
INSTALL(
  DIRECTORY
    ${CMAKE_CURRENT_SOURCE_DIR}/include/peelo
    DESTINATION
      include
    FILES_MATCHING PATTERN
      "*.hpp"
)
