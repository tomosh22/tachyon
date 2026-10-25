# Runs `tachyon-viewer --selftest <missing file>` and requires a normal, non-zero exit with a diagnostic.
execute_process(COMMAND ${VIEWER} --selftest ${MISSING}
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
# A crash makes RESULT_VARIABLE a string such as "Segmentation fault" instead of an integer.
if(NOT rc MATCHES "^[0-9]+$")
  message(FATAL_ERROR "viewer crashed: ${rc}")
endif()
if(rc EQUAL 0)
  message(FATAL_ERROR "expected a non-zero exit code for a missing file")
endif()
if(NOT out MATCHES "selftest: FAILED")
  message(FATAL_ERROR "missing 'selftest: FAILED' diagnostic; stdout: ${out} stderr: ${err}")
endif()
