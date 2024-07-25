// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#include "novadbplus/utils/time.h"

#ifdef _WIN32
#include <time.h>
#include <unistd.h>
#endif  // !_WIN32

#include <sstream>

namespace novadbplus {

uint64_t nsSinceEpoch() {
  using NS = std::chrono::nanoseconds;
  return std::chrono::duration_cast<NS>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

uint64_t usSinceEpoch() {
  using US = std::chrono::microseconds;
  return std::chrono::duration_cast<US>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

uint64_t msSinceEpoch() {
  using MS = std::chrono::milliseconds;
  return std::chrono::duration_cast<MS>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

uint32_t sinceEpoch() {
  using S = std::chrono::seconds;
  uint64_t count = std::chrono::duration_cast<S>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
  // we all know seconds since epoch fits in uint32_t
  return static_cast<uint32_t>(count);
}

std::string timePointRepr(const SCLOCK::time_point& tp) {
  std::stringstream ss;
  using SYSCLOCK = std::chrono::system_clock;
  auto t = SYSCLOCK::to_time_t(
    SYSCLOCK::now() +
    std::chrono::duration_cast<SYSCLOCK::duration>(tp - SCLOCK::now()));
#ifndef _WIN32
  ss << std::ctime(&t);
#else
  ss << _ctime64(&t);
#endif  // _WIN32
  return ss.str();
}

uint64_t nsSinceEpoch(const SCLOCK::time_point& tp) {
  using SYSCLOCK = std::chrono::system_clock;
  using NS = std::chrono::nanoseconds;
  auto t = SYSCLOCK::now() +
    std::chrono::duration_cast<SYSCLOCK::duration>(tp - SCLOCK::now());
  return std::chrono::duration_cast<NS>(t.time_since_epoch()).count();
}

uint32_t sinceEpoch(const SCLOCK::time_point& tp) {
  using SYSCLOCK = std::chrono::system_clock;
  using S = std::chrono::seconds;
  auto t = SYSCLOCK::now() +
    std::chrono::duration_cast<SYSCLOCK::duration>(tp - SCLOCK::now());
  return static_cast<uint32_t>(
    std::chrono::duration_cast<S>(t.time_since_epoch()).count());
}

// timestamp in second
std::string epochToDatetime(uint64_t epoch) {
  struct tm *dt, rt;
  char buffer[64];
  const time_t t = epoch;
  dt = localtime_r(&t, &rt);
  strftime(buffer, sizeof(buffer), "%y-%m-%d %H:%M:%S", dt);
  return std::string(buffer);
}

// timestamp in second
std::string epochToDatetimeInOneStr(uint64_t epoch) {
  struct tm *dt, rt;
  char buffer[64];
  const time_t t = epoch;
  dt = localtime_r(&t, &rt);
  strftime(buffer, sizeof(buffer), "%y/%m/%d-%H:%M:%S", dt);
  return std::string(buffer);
}

std::string msEpochToDatetime(uint64_t msEpoch) {
  return epochToDatetime(msEpoch / 1000);
}

std::string nsEpochToDatetime(uint64_t nsEpoch) {
  return epochToDatetime(nsEpoch / 1000000000);
}

/* How many milliseconds to the current time */
uint64_t msToNow(uint64_t ms) {
  auto now = msSinceEpoch();
  if (now <= ms) {
    return 0;
  }

  return now - ms;
}

}  // namespace novadbplus
