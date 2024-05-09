# - Try to find tidy
#
# Once done this will define
#  Tidy_FOUND - System has tidy
#  Tidy_INCLUDE_DIRS - The tidy include directories
#  Tidy_LIBRARIES - The libraries needed to use tidy

find_package(PkgConfig)
pkg_check_modules(PC_TIDY tidy)

find_path(TIDY_INCLUDE_DIR tidy.h
  HINTS
    ${PC_TIDY_INCLUDEDIR}
    ${PC_TIDY_INCLUDE_DIRS}
  PATH_SUFFIXES
    tidy
  PATHS
    ${PC_TIDY_INCLUDE_DIRS}
  )

find_library(TIDY_LIBRARY tidy
  HINTS
    ${PC_TIDY_LIBDIR}
    ${PC_TIDY_LIBRARY_DIRS}
  PATHS
    ${PC_TIDY_LIBRARY_DIRS}
  )

mark_as_advanced(TIDY_INCLUDE_DIR TIDY_LIBRARY)

if(TIDY_INCLUDE_DIR)
  set(Tidy_FOUND ON)
  set(Tidy_INCLUDE_DIRS ${TIDY_INCLUDE_DIR})
  set(Tidy_LIBRARIES ${TIDY_LIBRARY})
endif(TIDY_INCLUDE_DIR)
