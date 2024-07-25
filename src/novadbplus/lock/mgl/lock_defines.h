// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#ifndef SRC_novadbPLUS_LOCK_MGL_LOCK_DEFINES_H__
#define SRC_novadbPLUS_LOCK_MGL_LOCK_DEFINES_H__

#include <cinttypes>
#include <type_traits>

namespace novadbplus {

namespace mgl {

// NOTE(wolfkdy): reserve 16bits to extend to more lock modes
enum class LockMode : std::uint16_t {
  LOCK_NONE = 0,
  LOCK_IS = 1,
  LOCK_IX = 2,
  LOCK_S = 3,
  LOCK_X = 4,
  LOCK_MODE_NUM = 5,
};

// NOTE(wolfkdy): currently LOCKRES_DEADLOCK is not implemented
enum class LockRes : std::uint8_t {
  LOCKRES_OK = 0,
  LOCKRES_WAIT = 1,
  LOCKRES_TIMEOUT = 2,
  LOCKRES_DEADLOCK = 3,
  LOCKRES_UNINITED = 4,
  LOCKRES_NUM = 5,
};

const char* lockModeRepr(LockMode mode);

bool isConflict(uint16_t modes, LockMode mode);

template <typename E>
constexpr typename std::underlying_type<E>::type enum2Int(E e) {
  return static_cast<typename std::underlying_type<E>::type>(e);
}

}  // namespace mgl
}  // namespace novadbplus
#endif  // SRC_novadbPLUS_LOCK_MGL_LOCK_DEFINES_H__
