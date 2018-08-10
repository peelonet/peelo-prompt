CMAKE_MINIMUM_REQUIRED(VERSION 3.0)
PROJECT(peelocpp_prompt_example CXX)

ADD_EXECUTABLE(
  example
  example.cpp
)

TARGET_COMPILE_OPTIONS(
  example
  PRIVATE
    -Wall -Werror
)

TARGET_COMPILE_FEATURES(
  example
  PRIVATE
    cxx_std_17
)

TARGET_INCLUDE_DIRECTORIES(
  example
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
)

TARGET_LINK_LIBRARIES(
  example
  peelocpp_prompt
)
