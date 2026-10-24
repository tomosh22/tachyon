// tachyon::Result<T>: the error-handling type used across every library boundary (no exceptions, Part 2.4).
#pragma once

#include <cassert>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace tachyon {

enum class ErrorCode : unsigned {
  Ok = 0,
  InvalidArgument,
  NotFound,
  IoError,
  CorruptData,
  Unsupported,
  NoPrivilege,  // maps to protocol code ENOPRIV
  Busy,         // maps to protocol code EBUSY
  Cancelled,
  Timeout,
  OutOfMemory,
  Internal,  // maps to protocol code EINTERNAL
};

// Thread-safe: pure function.
constexpr std::string_view errorCodeName(ErrorCode code) {
  switch (code) {
    case ErrorCode::Ok: return "Ok";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::IoError: return "IoError";
    case ErrorCode::CorruptData: return "CorruptData";
    case ErrorCode::Unsupported: return "Unsupported";
    case ErrorCode::NoPrivilege: return "NoPrivilege";
    case ErrorCode::Busy: return "Busy";
    case ErrorCode::Cancelled: return "Cancelled";
    case ErrorCode::Timeout: return "Timeout";
    case ErrorCode::OutOfMemory: return "OutOfMemory";
    case ErrorCode::Internal: return "Internal";
  }
  return "Unknown";
}

struct Error {
  ErrorCode code = ErrorCode::Internal;
  std::string message;

  // Thread-safe: pure function.
  std::string toString() const { return std::string(errorCodeName(code)) + ": " + message; }
};

// Thread-safe: pure function. Builds an Error; implicitly converts to any Result<T>.
inline Error makeError(ErrorCode code, std::string message) {
  return Error{code, std::move(message)};
}

template <typename T>
class [[nodiscard]] Result {
  static_assert(!std::is_same_v<std::decay_t<T>, Error>, "Result<Error> is not allowed");

 public:
  using value_type = T;

  Result(T value)
      : m_storage(std::in_place_index<0>, std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(Error error)
      : m_storage(std::in_place_index<1>, std::move(error)) {}  // NOLINT(google-explicit-constructor)

  // All accessors: thread-safe for concurrent const access.
  bool ok() const { return m_storage.index() == 0; }
  explicit operator bool() const { return ok(); }

  T& value() & {
    assert(ok());
    return std::get<0>(m_storage);
  }
  const T& value() const& {
    assert(ok());
    return std::get<0>(m_storage);
  }
  T&& value() && {
    assert(ok());
    return std::get<0>(std::move(m_storage));
  }

  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T&& operator*() && { return std::move(*this).value(); }
  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

  const Error& error() const& {
    assert(!ok());
    return std::get<1>(m_storage);
  }
  Error&& error() && {
    assert(!ok());
    return std::get<1>(std::move(m_storage));
  }

  template <typename U>
  T valueOr(U&& fallback) const& {
    return ok() ? value() : static_cast<T>(std::forward<U>(fallback));
  }

 private:
  std::variant<T, Error> m_storage;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Error error) : m_error(std::move(error)), m_ok(false) {}  // NOLINT(google-explicit-constructor)

  bool ok() const { return m_ok; }
  explicit operator bool() const { return m_ok; }
  void value() const { assert(m_ok); }
  const Error& error() const& {
    assert(!m_ok);
    return m_error;
  }
  Error&& error() && {
    assert(!m_ok);
    return std::move(m_error);
  }

 private:
  Error m_error;
  bool m_ok = true;
};

// Thread-safe: pure function. Success value for Result<void>.
inline Result<void> ok() {
  return Result<void>();
}

}  // namespace tachyon

#define TACHYON_CONCAT_INNER(a, b) a##b
#define TACHYON_CONCAT(a, b) TACHYON_CONCAT_INNER(a, b)

// TACHYON_TRY(expr): evaluates a Result-returning expression and returns its error from the enclosing
// function.
#define TACHYON_TRY(expr)                            \
  do {                                               \
    auto&& tachyon_try_result_ = (expr);             \
    if (!tachyon_try_result_.ok()) {                 \
      return std::move(tachyon_try_result_).error(); \
    }                                                \
  } while (0)

// TACHYON_TRY_ASSIGN(lhs, expr): `lhs = std::move(*expr)` or returns the error. lhs may be a declaration.
#define TACHYON_TRY_ASSIGN(lhs, expr) \
  TACHYON_TRY_ASSIGN_IMPL(TACHYON_CONCAT(tachyon_try_tmp_, __LINE__), lhs, expr)
#define TACHYON_TRY_ASSIGN_IMPL(tmp, lhs, expr) \
  auto tmp = (expr);                            \
  if (!tmp.ok()) {                              \
    return std::move(tmp).error();              \
  }                                             \
  lhs = std::move(tmp).value()
