tachyon_add_test(control_tests
  SOURCES protocol_test.cpp transport_test.cpp server_client_test.cpp server_stall_test.cpp
          pipe_security_test.cpp tcp_test.cpp live_encoder_test.cpp
  DEPS tachyon::control tachyon::analysis tachyon::symbols tachyon::model)
