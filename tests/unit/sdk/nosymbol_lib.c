/* Shared library that deliberately lacks Tachyon_GetAPI (loader negative test). */
#if defined(_WIN32)
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
int tachyon_test_unrelated_symbol(void) {
  return 42;
}
