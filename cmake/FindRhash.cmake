# - Try to find rhash
#
# Once done this will define
#  Rhash_FOUND - System has rhash
#  Rhash_INCLUDE_DIRS - The rhash include directories
#  Rhash_LIBRARIES - The libraries needed to use rhash

find_path(RHASH_INCLUDE_DIR rhash.h)
find_library(RHASH_LIBRARY rhash)

mark_as_advanced(RHASH_INCLUDE_DIR RHASH_LIBRARY)

if(RHASH_LIBRARY AND RHASH_INCLUDE_DIR)
  set(Rhash_FOUND ON)
  set(Rhash_LIBRARIES ${RHASH_LIBRARY})
  set(Rhash_INCLUDE_DIRS ${RHASH_INCLUDE_DIR})
else()
  set(Rhash_FOUND OFF)
  if(Rhash_FIND_REQUIRED)
    message(FATAL_ERROR "Could not find rhash")
  endif(Rhash_FIND_REQUIRED)
endif(RHASH_LIBRARY AND RHASH_INCLUDE_DIR)
