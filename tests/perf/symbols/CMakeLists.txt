tachyon_add_perf_test(symbols_perf_tests SOURCES symbol_resolve_perf_test.cpp
  DEPS tachyon::symbols tachyon::model tachyon::trace)

# DIA perf budget (WP-22): Windows only, uses the real msdia140 and the testbed + system DLL PDBs.
if(WIN32 AND TACHYON_ENABLE_DIA)
  tachyon_add_perf_test(dia_perf_tests SOURCES dia_perf_test.cpp
    DEPS tachyon::symbols tachyon::etlconvert tachyon::model tachyon::trace)
  if(TARGET dia_perf_tests)
    target_compile_definitions(dia_perf_tests PRIVATE TACHYON_TESTBED_EXE="$<TARGET_FILE:tachyon-testbed>")
    add_dependencies(dia_perf_tests tachyon-testbed)
    tachyon_copy_dia_dll(dia_perf_tests)
  endif()
endif()

# DWARF provider perf budget (WP-72): Linux + libdw, resolves RVAs of the testbed and libc.
if(TACHYON_ENABLE_LIBDW)
  tachyon_add_perf_test(dwarf_perf_tests SOURCES dwarf_perf_test.cpp
    DEPS tachyon::symbols tachyon::model tachyon::trace)
  if(TARGET dwarf_perf_tests)
    target_compile_definitions(dwarf_perf_tests PRIVATE TACHYON_TESTBED_EXE="$<TARGET_FILE:tachyon-testbed>")
    add_dependencies(dwarf_perf_tests tachyon-testbed)
  endif()
endif()
