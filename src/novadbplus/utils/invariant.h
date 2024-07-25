// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#ifndef SRC_novadbPLUS_UTILS_INVARIANT_H_
#define SRC_novadbPLUS_UTILS_INVARIANT_H_

#include "glog/logging.h"

#ifdef _WIN32
#include "unistd.h"
#endif  // _WIN32


#ifndef LIKELY
#define LIKELY(x) (__builtin_expect((x), 1))
#endif
#ifndef UNLIKELY
#define UNLIKELY(x) (__builtin_expect((x), 0))
#endif


#define INVARIANT(e)                                                    \
  do {                                                                  \
    if (!__builtin_expect(e, 1)) {                                      \
      LOG(FATAL) << "INVARIANT failed:" << #e << ' ' << __FILE__ << ' ' \
                 << __LINE__;                                           \
    }                                                                   \
  } while (0)

#define INVARIANT_COMPARE(e1, op, e2)                                       \
  do {                                                                      \
    if (!((e1)op(e2))) {                                                    \
      LOG(FATAL) << "INVARIANT failed:" << #e1 << #op << #e2 << ' ' << (e1) \
                 << #op << (e2) << ' ' << __FILE__ << ' ' << __LINE__;      \
    }                                                                       \
  } while (0)

#define INVARIANT_LOG(e)                                                \
  do {                                                                  \
    if (!__builtin_expect(e, 1)) {                                      \
      LOG(ERROR) << "INVARIANT failed:" << #e << ' ' << __FILE__ << ' ' \
                 << __LINE__;                                           \
    }                                                                   \
  } while (0)

#define INVARIANT_COMPARE_LOG(e1, op, e2)                                   \
  do {                                                                      \
    if (!((e1)op(e2))) {                                                    \
      LOG(ERROR) << "INVARIANT failed:" << #e1 << #op << #e2 << ' ' << (e1) \
                 << #op << (e2) << ' ' << __FILE__ << ' ' << __LINE__;      \
    }                                                                       \
  } while (0)

#ifdef novadb_DEBUG
#define INVARIANT_D(e) INVARIANT(e)
#define INVARIANT_COMPARE_D(e1, op, e2) INVARIANT_COMPARE(e1, op, e2)
#else
#define INVARIANT_D(e) INVARIANT_LOG(e)
#define INVARIANT_COMPARE_D(e1, op, e2) INVARIANT_COMPARE_LOG(e1, op, e2)
#endif

#endif  // SRC_novadbPLUS_UTILS_INVARIANT_H_
