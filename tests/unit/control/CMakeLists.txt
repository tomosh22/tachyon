tachyon_add_test(control_tests
  SOURCES protocol_test.cpp transport_test.cpp server_client_test.cpp server_stall_test.cpp
          pipe_security_test.cpp
  DEPS tachyon::control)
