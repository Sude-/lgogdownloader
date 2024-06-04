find_program(GZIP gzip DOC "Location of the gzip program")
mark_as_advanced(GZIP)

include(GNUInstallDirs)

if(GZIP)
  set(MAN_PAGE "${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.1")
  set(MAN_FILE "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.1.gz")
  add_custom_command(
    OUTPUT ${MAN_FILE}
    COMMAND ${GZIP} -c -9 ${MAN_PAGE} > ${MAN_FILE}
    MAIN_DEPENDENCY ${MAN_PAGE}
	COMMENT "Building man page"
	VERBATIM
	)
  add_custom_target(manpage ALL DEPENDS ${MAN_FILE} ${PROJECT_NAME})
  install(FILES ${MAN_FILE} DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
else(GZIP)
  message("WARNING: One of the following is missing: gzip; man page will not be generated")
endif(GZIP)
