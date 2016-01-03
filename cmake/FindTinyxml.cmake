# - Try to find tinyxml
#
# Once done this will define
#  Tinyxml_FOUND - System has tinyxml
#  Tinyxml_INCLUDE_DIRS - The tinyxml include directories
#  Tinyxml_LIBRARIES - The libraries needed to use tinyxml

find_package(PkgConfig)
pkg_check_modules(PC_TINYXML REQUIRED tinyxml)

find_path(TINYXML_INCLUDE_DIR tinyxml.h
  HINTS
    ${PC_TINYXML_INCLUDEDIR}
    ${PC_TINYXML_INCLUDE_DIRS}
  PATHS
    ${PC_TINYXML_INCLUDE_DIRS}
  )

find_library(TINYXML_LIBRARY tinyxml
  HINTS
    ${PC_TINYXML_LIBDIR}
    ${PC_TINYXML_LIBRARY_DIRS}
  PATHS
    ${PC_TINYXML_LIBRARY_DIRS}
  )

mark_as_advanced(TINYXML_INCLUDE_DIR TINYXML_LIBRARY)

if(PC_TINYXML_FOUND)
  set(Tinyxml_FOUND ON)
  set(Tinyxml_INCLUDE_DIRS ${TINYXML_INCLUDE_DIR})
  set(Tinyxml_LIBRARIES ${TINYXML_LIBRARY})
endif(PC_TINYXML_FOUND)
