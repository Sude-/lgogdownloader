# - Try to find libcrypto
#
# Once done this will define
#  Libcrypto_FOUND - System has libcrypto
#  Libcrypto_INCLUDE_DIRS - The libcrypto include directories
#  Libcrypto_LIBRARIES - The libraries needed to use libcrypto

find_package(PkgConfig)
pkg_check_modules(PC_LIBCRYPTO REQUIRED libcrypto)

find_path(LIBCRYPTO_INCLUDE_DIR openssl/crypto.h
  HINTS ${PC_LIBCRYPTO_INCLUDEDIR}
  ${PC_LIBCRYPTO_INCLUDE_DIRS}
  )

find_library(LIBCRYPTO_LIBRARY NAMES crypto
  HINTS ${PC_LIBCRYPTO_LIBDIR}
  ${PC_LIBCRYPTO_LIBRARY_DIRS}
  )

mark_as_advanced(LIBCRYPTO_INCLUDE_DIR LIBCRYPTO_LIBRARY)

if(PC_LIBCRYPTO_FOUND)
  set(Libcrypto_FOUND ON)
  set(Libcrypto_INCLUDE_DIRS ${LIBCRYPTO_INCLUDE_DIR})
  set(Libcrypto_LIBRARIES ${LIBCRYPTO_LIBRARY})
endif(PC_LIBCRYPTO_FOUND)
