// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#include "novadbplus/storage/pessimistic.h"

#include "novadbplus/utils/invariant.h"

namespace novadbplus {

bool PessimisticShard::isLocked(const std::string& k) const {
  std::lock_guard<std::mutex> lk(_mutex);
  return _set.find(k) != _set.end();
}

void PessimisticShard::lock(const std::string& k) {
  std::lock_guard<std::mutex> lk(_mutex);
  INVARIANT_D(_set.find(k) == _set.end());
  _set.insert(k);
}

void PessimisticShard::unlock(const std::string& k) {
  std::lock_guard<std::mutex> lk(_mutex);
  INVARIANT_D(_set.find(k) != _set.end());
  _set.erase(k);
}

PessimisticMgr::PessimisticMgr(uint32_t num) {
  for (uint32_t i = 0; i < num; ++i) {
    _data.emplace_back(std::make_unique<PessimisticShard>());
  }
}

PessimisticShard* PessimisticMgr::getShard(uint32_t n) {
  return _data[n].get();
}

}  // namespace novadbplus
