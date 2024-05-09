cmake_minimum_required(VERSION 3.5.0 FATAL_ERROR)
project (lgogdownloader LANGUAGES C CXX VERSION 3.12)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")

# Disable search for boost-cmake
# Fixes building with Boost >= 1.70.0
set(Boost_NO_BOOST_CMAKE ON)

option(USE_QT_GUI "Build with Qt GUI login support" OFF)
if(USE_QT_GUI)
  add_definitions(-DUSE_QT_GUI_LOGIN=1)
  set(CMAKE_AUTOMOC ON)
  set(CMAKE_AUTOUIC ON)
endif(USE_QT_GUI)

find_package(Boost
  REQUIRED
  system
  filesystem
  regex
  program_options
  date_time
  iostreams
  )
find_package(CURL 7.55.0 REQUIRED)
find_package(Jsoncpp REQUIRED)
find_package(Tinyxml2 REQUIRED)
find_package(Rhash REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Tidy REQUIRED)

file(GLOB SRC_FILES
  main.cpp
  src/website.cpp
  src/downloader.cpp
  src/progressbar.cpp
  src/util.cpp
  src/blacklist.cpp
  src/gamefile.cpp
  src/gamedetails.cpp
  src/galaxyapi.cpp
  src/ziputil.cpp
  )

if(USE_QT_GUI)
  set(QT Qt6)
  find_package(Qt6 COMPONENTS Widgets WebEngineWidgets CONFIG)
  if(NOT Qt6_FOUND)
    set(QT Qt5)
    find_package(Qt5 REQUIRED COMPONENTS Widgets WebEngineWidgets CONFIG)
  endif()

  file(GLOB QT_GUI_SRC_FILES
    src/gui_login.cpp
  )
  list(APPEND SRC_FILES ${QT_GUI_SRC_FILES})
endif(USE_QT_GUI)


set(GIT_CHECKOUT FALSE)
if(EXISTS ${PROJECT_SOURCE_DIR}/.git)
  if(NOT EXISTS ${PROJECT_SOURCE_DIR}/.git/shallow)
    find_package(Git)
    if(GIT_FOUND)
      set(GIT_CHECKOUT TRUE)
    else(GIT_FOUND)
      message(WARNING "Git executable not found")
    endif(GIT_FOUND)
  else(NOT EXISTS ${PROJECT_SOURCE_DIR}/.git/shallow)
    message(STATUS "Shallow Git clone detected, not attempting to retrieve version info")
  endif(NOT EXISTS ${PROJECT_SOURCE_DIR}/.git/shallow)
endif(EXISTS ${PROJECT_SOURCE_DIR}/.git)

if(GIT_CHECKOUT)
  execute_process(COMMAND ${GIT_EXECUTABLE} diff --shortstat
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_SHORTSTAT
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_REV_PARSE
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(GIT_SHORTSTAT)
    set(GIT_DIRTY ON)
  endif(GIT_SHORTSTAT)

  if(GIT_DIRTY)
    set(PROJECT_VERSION_MINOR ${PROJECT_VERSION_MINOR}M)
  endif(GIT_DIRTY)

  set(PROJECT_VERSION_PATCH ${GIT_REV_PARSE})
  set(PROJECT_VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH})
endif(GIT_CHECKOUT)

set(VERSION_NUMBER ${PROJECT_VERSION})
set(VERSION_STRING "LGOGDownloader ${VERSION_NUMBER}")
set(DEFAULT_USER_AGENT "LGOGDownloader/${VERSION_NUMBER} (${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR})")

add_definitions(-D_FILE_OFFSET_BITS=64 -DVERSION_NUMBER="${VERSION_NUMBER}" -DVERSION_STRING="${VERSION_STRING}" -DDEFAULT_USER_AGENT="${DEFAULT_USER_AGENT}")

add_executable (${PROJECT_NAME} ${SRC_FILES})


target_include_directories(${PROJECT_NAME}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${Boost_INCLUDE_DIRS}
  PRIVATE ${CURL_INCLUDE_DIRS}
  PRIVATE ${OAuth_INCLUDE_DIRS}
  PRIVATE ${Jsoncpp_INCLUDE_DIRS}
  PRIVATE ${Tinyxml2_INCLUDE_DIRS}
  PRIVATE ${Rhash_INCLUDE_DIRS}
  PRIVATE ${ZLIB_INCLUDE_DIRS}
  PRIVATE ${Tidy_INCLUDE_DIRS}
  )

target_link_libraries(${PROJECT_NAME}
  PRIVATE ${Boost_LIBRARIES}
  PRIVATE ${CURL_LIBRARIES}
  PRIVATE ${OAuth_LIBRARIES}
  PRIVATE ${Jsoncpp_LIBRARIES}
  PRIVATE ${Tinyxml2_LIBRARIES}
  PRIVATE ${Rhash_LIBRARIES}
  PRIVATE ${CMAKE_THREAD_LIBS_INIT}
  PRIVATE ${ZLIB_LIBRARIES}
  PRIVATE ${Tidy_LIBRARIES}
  )

# Check if libatomic is needed in order to use std::atomic, and add
# it to the list of JavaScriptCore libraries.
file(WRITE ${CMAKE_BINARY_DIR}/test_atomic.cpp
     "#include <atomic>\n"
     "int main() { std::atomic<int64_t> i(0); i++; return 0; }\n")
try_compile(ATOMIC_BUILD_SUCCEEDED ${CMAKE_BINARY_DIR} ${CMAKE_BINARY_DIR}/test_atomic.cpp)
if (NOT ATOMIC_BUILD_SUCCEEDED)
  target_link_libraries(${PROJECT_NAME}
    PRIVATE -latomic
  )
endif ()
file(REMOVE ${CMAKE_BINARY_DIR}/test_atomic.cpp)

if(USE_QT_GUI)
  target_link_libraries(${PROJECT_NAME}
    PRIVATE ${QT}::Widgets
    PRIVATE ${QT}::WebEngineWidgets
  )
endif(USE_QT_GUI)

if(Qt6_FOUND)
  set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
else()
  set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 11)
endif(Qt6_FOUND)

if(MSVC)
  # Force to always compile with W4
  if(CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
    string(REGEX REPLACE "/W[0-4]" "/W4" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
  endif()
elseif(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # Update if necessary
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-long-long -fexceptions")
endif()

set(INSTALL_BIN_DIR bin CACHE PATH "Installation directory for executables")
set(INSTALL_SHARE_DIR share CACHE PATH "Installation directory for resource files")

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}${CMAKE_EXECUTABLE_SUFFIX} DESTINATION ${INSTALL_BIN_DIR})
add_subdirectory(man)
