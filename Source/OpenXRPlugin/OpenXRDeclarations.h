#pragma once

/// \brief Whether an XrResult reports failure.
///
/// OpenXR splits XrResult by sign: negative values are errors, zero (XR_SUCCESS) and positive values are successes.
/// The positive ones carry information rather than failure - XR_FRAME_DISCARDED, XR_SESSION_LOSS_PENDING,
/// XR_TIMEOUT_EXPIRED - so comparing against XR_SUCCESS alone misreads them as fatal.
#define XR_FAILED_RESULT(s) ((s) < 0)

#define XR_LOG_ERROR(code)                                                                                                     \
  do                                                                                                                           \
  {                                                                                                                            \
    auto s = (code);                                                                                                           \
    if (XR_FAILED_RESULT(s))                                                                                                   \
    {                                                                                                                          \
      plLog::Error("OpenXR call '{0}' failed with: {1} in {2}:{3}", PL_PP_STRINGIFY(code), s, PL_SOURCE_FILE, PL_SOURCE_LINE); \
    }                                                                                                                          \
  } while (false)

#define XR_SUCCEED_OR_RETURN_LOG(code)                                                                                         \
  do                                                                                                                           \
  {                                                                                                                            \
    auto s = (code);                                                                                                           \
    if (XR_FAILED_RESULT(s))                                                                                                   \
    {                                                                                                                          \
      plLog::Error("OpenXR call '{0}' failed with: {1} in {2}:{3}", PL_PP_STRINGIFY(code), s, PL_SOURCE_FILE, PL_SOURCE_LINE); \
      return s;                                                                                                                \
    }                                                                                                                          \
  } while (false)

#define XR_SUCCEED_OR_CLEANUP_LOG(code, cleanup)                                                                               \
  do                                                                                                                           \
  {                                                                                                                            \
    auto s = (code);                                                                                                           \
    if (XR_FAILED_RESULT(s))                                                                                                   \
    {                                                                                                                          \
      plLog::Error("OpenXR call '{0}' failed with: {1} in {2}:{3}", PL_PP_STRINGIFY(code), s, PL_SOURCE_FILE, PL_SOURCE_LINE); \
      cleanup();                                                                                                               \
      return s;                                                                                                                \
    }                                                                                                                          \
  } while (false)

static void voidFunction() {}
