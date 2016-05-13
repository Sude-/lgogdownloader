# - Try to find tinyxml2
#
# Once done this will define
#  Tinyxml2_FOUND - System has tinyxml2
#  Tinyxml2_INCLUDE_DIRS - The tinyxml2 include directories
#  Tinyxml2_LIBRARIES - The libraries needed to use tinyxml

find_package(PkgConfig)
pkg_check_modules(PC_TINYXML2 tinyxml2)

find_path(TINYXML2_INCLUDE_DIR tinyxml2.h
  HINTS
    ${PC_TINYXML2_INCLUDEDIR}
    ${PC_TINYXML2_INCLUDE_DIRS}
  PATHS
    ${PC_TINYXML2_INCLUDE_DIRS}
  )

find_library(TINYXML2_LIBRARY tinyxml2
  HINTS
    ${PC_TINYXML2_LIBDIR}
    ${PC_TINYXML2_LIBRARY_DIRS}
  PATHS
    ${PC_TINYXML2_LIBRARY_DIRS}
  )

mark_as_advanced(TINYXML2_INCLUDE_DIR TINYXML2_LIBRARY)

if(TINYXML2_INCLUDE_DIR)
  set(Tinyxml2_FOUND ON)
  set(Tinyxml2_INCLUDE_DIRS ${TINYXML2_INCLUDE_DIR})
  set(Tinyxml2_LIBRARIES ${TINYXML2_LIBRARY})
endif(TINYXML2_INCLUDE_DIR)
