# Fetches ntdll.pdb from the Microsoft symbol server (skips when the network is unreachable).
tachyon_add_test(symbols_windows_tests
  SOURCES msdl_fetch_test.cpp
  DEPS tachyon::symbols tachyon::etlconvert
  LABELS windows)
