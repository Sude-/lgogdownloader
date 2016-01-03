# - Try to find oauth
#
# Once done this will define
#  OAuth_FOUND - System has oauth
#  OAuth_INCLUDE_DIRS - The oauth include directories
#  OAuth_LIBRARIES - The libraries needed to use oauth

find_package(PkgConfig)
pkg_check_modules(PC_OAUTH REQUIRED oauth)

find_path(OAUTH_INCLUDE_DIR oauth.h
  HINTS ${PC_OAUTH_INCLUDEDIR}
  ${PC_OAUTH_INCLUDE_DIRS}
  PATH_SUFFIXES oauth
  )

find_library(OAUTH_LIBRARY NAMES oauth
  HINTS ${PC_OAUTH_LIBDIR}
  ${PC_OAUTH_LIBRARY_DIRS}
  )

mark_as_advanced(OAUTH_INCLUDE_DIR OAUTH_LIBRARY)

if(PC_OAUTH_FOUND)
  set(OAuth_FOUND ON)
  set(OAuth_INCLUDE_DIRS ${OAUTH_INCLUDE_DIR})
  set(OAuth_LIBRARIES ${OAUTH_LIBRARY})
endif(PC_OAUTH_FOUND)
