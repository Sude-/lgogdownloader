find_program(HELP2MAN help2man DOC "Location of the help2man program")
find_program(GZIP gzip DOC "Location of the gzip program")
mark_as_advanced(HELP2MAN)
mark_as_advanced(GZIP)

include(GNUInstallDirs)

if(HELP2MAN AND GZIP)	
  set(H2M_FILE ${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.supplemental.groff)
  set(MAN_PAGE ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.1)
  set(MAN_FILE ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.1.gz)
  add_custom_command(
    OUTPUT ${MAN_FILE}
    COMMAND ${HELP2MAN} -N -i ${H2M_FILE} -o ${MAN_PAGE} ${PROJECT_BINARY_DIR}/${PROJECT_NAME}${CMAKE_EXECUTABLE_SUFFIX}
    COMMAND ${GZIP} -f -9 ${MAN_PAGE}
    MAIN_DEPENDENCY ${H2M_FILE}
	COMMENT "Building man page"
	VERBATIM
	)
  add_custom_target(manpage ALL DEPENDS ${MAN_FILE} ${PROJECT_NAME})
  install(FILES ${MAN_FILE} DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
else(HELP2MAN AND GZIP)
  message("WARNING: One of the following is missing: help2man, gzip; man page will not be generated")
endif(HELP2MAN AND GZIP)
