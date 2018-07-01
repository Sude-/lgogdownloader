cmake_minimum_required(VERSION 3.0.0 FATAL_ERROR)
project (lgogdownloader LANGUAGES C CXX VERSION 3.3)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")
set(LINK_LIBCRYPTO 0)

find_program(READELF readelf DOC "Location of the readelf program")
find_program(GREP grep DOC "Location of the grep program")
find_package(Boost
  REQUIRED
  system
  filesystem
  regex
  program_options
  date_time
  iostreams
  )
find_package(CURL 7.32.0 REQUIRED)
if(CURL_FOUND)
  execute_process(
    COMMAND ${READELF} -d ${CURL_LIBRARIES}
    COMMAND ${GREP} -q "libssl\\|libcrypto"
    RESULT_VARIABLE READELF_RESULT_VAR
  )
  if(READELF_RESULT_VAR EQUAL 0)
    add_definitions(-DSSL_THREAD_SETUP_OPENSSL=1)
    find_package(Libcrypto REQUIRED)
    set(LINK_LIBCRYPTO 1)
  endif(READELF_RESULT_VAR EQUAL 0)
endif(CURL_FOUND)

find_package(OAuth REQUIRED)
find_package(Jsoncpp REQUIRED)
find_package(Htmlcxx REQUIRED)
find_package(Tinyxml2 REQUIRED)
find_package(Rhash REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

file(GLOB SRC_FILES
  main.cpp
  src/api.cpp
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
  PRIVATE ${Htmlcxx_INCLUDE_DIRS}
  PRIVATE ${Tinyxml2_INCLUDE_DIRS}
  PRIVATE ${Rhash_INCLUDE_DIRS}
  PRIVATE ${ZLIB_INCLUDE_DIRS}
  )

target_link_libraries(${PROJECT_NAME}
  PRIVATE ${Boost_LIBRARIES}
  PRIVATE ${CURL_LIBRARIES}
  PRIVATE ${OAuth_LIBRARIES}
  PRIVATE ${Jsoncpp_LIBRARIES}
  PRIVATE ${Htmlcxx_LIBRARIES}
  PRIVATE ${Tinyxml2_LIBRARIES}
  PRIVATE ${Rhash_LIBRARIES}
  PRIVATE ${CMAKE_THREAD_LIBS_INIT}
  PRIVATE ${ZLIB_LIBRARIES}
  )

if(LINK_LIBCRYPTO EQUAL 1)
  target_link_libraries(${PROJECT_NAME}
    PRIVATE ${Libcrypto_LIBRARIES}
  )
endif(LINK_LIBCRYPTO EQUAL 1)

if(MSVC)
  # Force to always compile with W4
  if(CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
    string(REGEX REPLACE "/W[0-4]" "/W4" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
  endif()
elseif(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # Update if necessary
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Wextra -Wno-long-long -fexceptions")
endif()

set(INSTALL_BIN_DIR bin CACHE PATH "Installation directory for executables")
set(INSTALL_SHARE_DIR share CACHE PATH "Installation directory for resource files")

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}${CMAKE_EXECUTABLE_SUFFIX} DESTINATION ${INSTALL_BIN_DIR})
add_subdirectory(man)
