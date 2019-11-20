# - Try to find Jsoncpp
#
# Once done, this will define
#  Jsoncpp_FOUND - system has Jsoncpp
#  Jsoncpp_INCLUDE_DIRS - the Jsoncpp include directories
#  Jsoncpp_LIBRARIES - link these to use Jsoncpp

find_package(PkgConfig)
pkg_check_modules(PC_JSONCPP REQUIRED jsoncpp)

find_path(JSONCPP_INCLUDE_DIR
  NAMES
    json/allocator.h
  HINTS
    ${PC_JSONCPP_INCLUDEDIR}
    ${PC_JSONCPP_INCLUDEDIRS}
  PATH_SUFFIXES
    jsoncpp
  PATHS
    ${PC_JSONCPP_INCLUDE_DIRS}
  )

find_library(JSONCPP_LIBRARY jsoncpp
  PATHS
    ${PC_JSONCPP_LIBRARY_DIRS}
  )

mark_as_advanced(JSONCPP_INCLUDE_DIR JSONCPP_LIBRARY)

if(PC_JSONCPP_FOUND)
  set(Jsoncpp_FOUND ON)
  set(Jsoncpp_INCLUDE_DIRS ${JSONCPP_INCLUDE_DIR})
  set(Jsoncpp_LIBRARIES ${JSONCPP_LIBRARY})
endif(PC_JSONCPP_FOUND)
