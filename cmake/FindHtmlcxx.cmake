# - Try to find htmlcxx
#
# Once done this will define
#  Htmlcxx_FOUND - System has htmlcxx
#  Htmlcxx_INCLUDE_DIRS - The htmlcxx include directories
#  Htmlcxx_LIBRARIES - The libraries needed to use htmlcxx

find_package(PkgConfig)
pkg_check_modules(PC_HTMLCXX REQUIRED htmlcxx)

find_path(HTMLCXX_INCLUDE_DIR
  NAMES
    css/parser.h
    html/tree.h
  HINTS
    ${PC_HTMLCXX_INCLUDEDIR}
    ${PC_HTMLCXX_INCLUDE_DIRS}
  PATH_SUFFIXES
    htmlcxx
  PATHS
    ${PC_HTMLCXX_INCLUDE_DIRS}
  )

find_library(HTMLCXX_LIBRARY_HTMLCXX htmlcxx
  HINTS
    ${PC_HTMLCXX_LIBDIR}
    ${PC_HTMLCXX_LIBRARY_DIRS}
  PATHS
    ${PC_HTMLCXX_LIBRARY_DIRS}
  )

find_library(HTMLCXX_LIBRARY_CSS_PARSER css_parser
  HINTS
    ${PC_HTMLCXX_LIBDIR}
    ${PC_HTMLCXX_LIBRARY_DIRS}
  PATHS
    ${PC_HTMLCXX_LIBRARY_DIRS}
  )

find_library(HTMLCXX_LIBRARY_CSS_PARSER_PP css_parser_pp
  HINTS
    ${PC_HTMLCXX_LIBDIR}
    ${PC_HTMLCXX_LIBRARY_DIRS}
  PATHS
    ${PC_HTMLCXX_LIBRARY_DIRS}
  )

mark_as_advanced(HTMLCXX_INCLUDE_DIR HTMLCXX_LIBRARY_HTMLCXX HTMLCXX_LIBRARY_CSS_PARSER HTMLCXX_LIBRARY_CSS_PARSER_PP)

if(PC_HTMLCXX_FOUND)
  set(Htmlcxx_FOUND ON)
  set(Htmlcxx_INCLUDE_DIRS ${HTMLCXX_INCLUDE_DIR})
  set(Htmlcxx_LIBRARIES ${HTMLCXX_LIBRARY_HTMLCXX} ${HTMLCXX_LIBRARY_CSS_PARSER} ${HTMLCXX_LIBRARY_CSS_PARSER_PP})
endif(PC_HTMLCXX_FOUND)
