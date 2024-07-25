// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#ifndef SRC_novadbPLUS_UTILS_TIME_RECORD_H_
#define SRC_novadbPLUS_UTILS_TIME_RECORD_H_

namespace novadbplus {

#define novadb_LOCK_LATENCY_RECORD(PROC, SESS, NAME, TYPE)                    \
  if (gParams && gParams->novadbLatencyLimit == 0) {                          \
    (PROC);                                                                   \
  } else {                                                                    \
    auto timsStart = usSinceEpoch();                                          \
    (PROC);                                                                   \
    auto usSpend = usSinceEpoch() - timsStart;                                \
    if (SESS) {                                                               \
      (SESS)->getCtx()->addLockRecord(usSpend, (NAME), (TYPE));               \
    } else if (usSpend >= (gParams ? gParams->novadbLatencyLimit : 100000)) { \
      LOG(WARNING) << "latency too long acquire lock, start ts(us):"          \
                   << timsStart << " latency(us):" << usSpend                 \
                   << " lock type:" << LLTToString[TYPE]                      \
                   << " lock id:" << (NAME)                                   \
                   << " threadid:" << getCurThreadId();                       \
    }                                                                         \
  }

#define novadb_ROCKSDB_LATENCY_RECORD(PROC, RWSIZE, TYPE)                      \
  if (gParams && gParams->novadbLatencyLimit == 0) {                           \
    return (PROC);                                                             \
  }                                                                            \
  auto timsStart = usSinceEpoch();                                             \
  auto status = (PROC);                                                        \
  auto usSpend = usSinceEpoch() - timsStart;                                   \
  if (_session) {                                                              \
    _session->getCtx()->addRocksdbRecord(                                      \
      usSpend, status.ok(), (RWSIZE), (TYPE));                                 \
  } else if (usSpend >= (gParams ? gParams->novadbLatencyLimit : 100000)) {    \
    LOG(WARNING) << "latency too long rocksdb r/w, start ts(us):" << timsStart \
                 << " latency(us):" << usSpend                                 \
                 << " op type:" << RLTToString[TYPE]                           \
                 << " op size:" << (RWSIZE)                                    \
                 << " threadid:" << getCurThreadId();                          \
  }                                                                            \
  return status;

}  // namespace novadbplus

#endif  // SRC_novadbPLUS_UTILS_TIME_RECORD_H_
