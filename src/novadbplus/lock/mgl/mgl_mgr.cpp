// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#include "novadbplus/lock/mgl/mgl_mgr.h"

#include <utility>

#include "novadbplus/commands/command.h"
#include "novadbplus/lock/mgl/lock_defines.h"
#include "novadbplus/lock/mgl/mgl.h"
#include "novadbplus/utils/invariant.h"

namespace novadbplus {

namespace mgl {

/**
 * Map of conflicts. 'conflictTable[newMode] & existingMode != 0' means that a
 * new request with the given 'newMode' conflicts with an existing request with
 * mode 'existingMode'.
 */
#define ENUM2INT(m) \
  static_cast<typename std::underlying_type<LockMode>::type>(m)
static const int conflictTable[] = {
  // MODE_NONE
  0,
  // MODE_IS
  (1 << ENUM2INT(LockMode::LOCK_X)),
  // MODE_IX
  (1 << ENUM2INT(LockMode::LOCK_S)) | (1 << ENUM2INT(LockMode::LOCK_X)),
  // MODE_S
  (1 << ENUM2INT(LockMode::LOCK_IX)) | (1 << ENUM2INT(LockMode::LOCK_X)),
  // MODE_X
  (1 << ENUM2INT(LockMode::LOCK_IS)) | (1 << ENUM2INT(LockMode::LOCK_IX)) |
    (1 << ENUM2INT(LockMode::LOCK_S)) | (1 << ENUM2INT(LockMode::LOCK_X)),
};

const char* lockModeRepr(LockMode mode) {
  switch (mode) {
    case LockMode::LOCK_X:
      return "X";
    case LockMode::LOCK_IX:
      return "IX";
    case LockMode::LOCK_S:
      return "S";
    case LockMode::LOCK_IS:
      return "IS";
    default:
      return "?";
  }
}

bool isConflict(uint16_t modes, LockMode mode) {
  uint16_t modeInt = enum2Int(mode);
  return (conflictTable[modeInt] & modes) != 0;
}

LockSchedCtx::LockSchedCtx()
  : _runningModes(0),
    _pendingModes(0),
    _runningRefCnt(enum2Int(LockMode::LOCK_MODE_NUM), 0),
    _pendingRefCnt(enum2Int(LockMode::LOCK_MODE_NUM), 0) {}

// NOTE(deyukong): if compitable locks come endlessly,
// and we always schedule compitable locks first.
// Then the _pendingList will have no chance to schedule.
void LockSchedCtx::lock(MGLock* core) {
  auto mode = core->getMode();
  if (isConflict(_runningModes, mode) || _pendingList.size() >= 1) {
    auto it = _pendingList.insert(_pendingList.end(), core);
    incrPendingRef(mode);
    core->setLockResult(LockRes::LOCKRES_WAIT, it);
  } else {
    auto it = _runningList.insert(_runningList.end(), core);
    incrRunningRef(mode);
    core->setLockResult(LockRes::LOCKRES_OK, it);
  }
}

void LockSchedCtx::schedPendingLocks() {
  std::list<MGLock*>::iterator it = _pendingList.begin();
  while (it != _pendingList.end()) {
    MGLock* tmpLock = *it;
    if (isConflict(_runningModes, tmpLock->getMode())) {
      it++;
      // NOTE(vinchen): Here, it should be break instead of continue.
      // Because of first come first lock/unlock, it can't release the
      // lock after the conflict pending lock. Otherwise, it would lead
      // to this lock starve.
      break;
    }
    incrRunningRef(tmpLock->getMode());
    decPendingRef(tmpLock->getMode());
    auto runningIt = _runningList.insert(_runningList.end(), tmpLock);
    it = _pendingList.erase(it);
    tmpLock->setLockResult(LockRes::LOCKRES_OK, runningIt);
    tmpLock->notify();
  }
}

bool LockSchedCtx::unlock(MGLock* core) {
  auto mode = core->getMode();
  if (core->getStatus() == LockRes::LOCKRES_OK) {
    _runningList.erase(core->getLockIter());
    decRunningRef(mode);
    core->releaseLockResult();
    if (_runningModes != 0) {
      return false;
    }
    INVARIANT_D(_runningList.size() == 0);
    schedPendingLocks();
  } else if (core->getStatus() == LockRes::LOCKRES_WAIT) {
    _pendingList.erase(core->getLockIter());
    decPendingRef(mode);
    core->releaseLockResult();
    INVARIANT_D((_pendingModes == 0 && _pendingList.size() == 0) ||
                (_pendingModes != 0 && _pendingList.size() != 0));
    schedPendingLocks();
  } else {
    INVARIANT_D(0);
  }
  return _pendingList.empty() && _runningList.empty();
}

void LockSchedCtx::incrPendingRef(LockMode mode) {
  auto modeInt = enum2Int(mode);
  ++_pendingRefCnt[modeInt];
  if (_pendingRefCnt[modeInt] == 1) {
    INVARIANT_D((_pendingModes & (1 << modeInt)) == 0);
    _pendingModes |= static_cast<uint16_t>((1 << modeInt));
  }
}

void LockSchedCtx::decPendingRef(LockMode mode) {
  auto modeInt = enum2Int(mode);
  INVARIANT_D(_pendingRefCnt[modeInt] != 0);
  --_pendingRefCnt[modeInt];
  if (_pendingRefCnt[modeInt] == 0) {
    INVARIANT_D((_pendingModes & (1 << modeInt)) != 0);
    _pendingModes &= static_cast<uint16_t>(~(1 << modeInt));
  }
}

void LockSchedCtx::incrRunningRef(LockMode mode) {
  auto modeInt = enum2Int(mode);
  ++_runningRefCnt[modeInt];
  if (_runningRefCnt[modeInt] == 1) {
    INVARIANT_D((_runningModes & (1 << modeInt)) == 0);
    _runningModes |= static_cast<uint16_t>((1 << modeInt));
  }
}

void LockSchedCtx::decRunningRef(LockMode mode) {
  auto modeInt = enum2Int(mode);
  INVARIANT_D(_runningRefCnt[modeInt] != 0);
  --_runningRefCnt[modeInt];
  if (_runningRefCnt[modeInt] == 0) {
    INVARIANT_D((_runningModes & (1 << modeInt)) != 0);
    _runningModes &= static_cast<uint16_t>(~(1 << modeInt));
  }
}

std::string LockSchedCtx::toString() {
  std::stringstream ss;

  for (auto i : _runningList) {
    ss << "running: {" << i->toString() << "}\r\n";
  }

  for (auto i : _pendingList) {
    ss << "pending: {" << i->toString() << "}\r\n";
  }

  return ss.str();
}

std::vector<std::string> LockSchedCtx::getShardLocks() {
  std::vector<std::string> tempLocks;
  for (auto i : _runningList) {
    tempLocks.push_back("running: {" + i->toString() + "}");
  }

  for (auto i : _pendingList) {
    tempLocks.push_back("pending: {" + i->toString() + "}");
  }
  return tempLocks;
}

void MGLockMgr::lock(MGLock* core) {
  uint64_t hash = core->getHash();
  LockShard& shard = _shards[hash % SHARD_NUM];
  std::lock_guard<std::mutex> lk(shard.mutex);
  auto iter = shard.map.find(core->getTarget());
  if (iter == shard.map.end()) {
    LockSchedCtx tmp;
    auto insertResult = shard.map.emplace(core->getTarget(), std::move(tmp));
    iter = insertResult.first;
  }
  iter->second.lock(core);
  return;
}

void MGLockMgr::unlock(MGLock* core) {
  uint64_t hash = core->getHash();
  LockShard& shard = _shards[hash % SHARD_NUM];
  std::lock_guard<std::mutex> lk(shard.mutex);

  INVARIANT_D(core->getStatus() == LockRes::LOCKRES_WAIT ||
              core->getStatus() == LockRes::LOCKRES_OK);

  auto iter = shard.map.find(core->getTarget());
  INVARIANT(iter != shard.map.end());
  bool empty = iter->second.unlock(core);
  if (empty) {
    shard.map.erase(iter);
  }
  return;
}

std::string MGLockMgr::toString() {
  std::stringstream ss;
  auto locklist = getLockList();
  for (auto& vs : locklist) {
    ss << vs;
  }
  return ss.str();
}

std::vector<std::string> MGLockMgr::getLockList() {
  std::vector<std::string> list;
  for (uint32_t i = 0; i < SHARD_NUM; i++) {
    LockShard& shard = _shards[i];
    std::lock_guard<std::mutex> lk(shard.mutex);
    for (auto& iter : shard.map) {
      auto locklist = iter.second.getShardLocks();
      for (auto& v : locklist) {
        list.push_back(v);
      }
    }
  }
  return list;
}

}  // namespace mgl
}  // namespace novadbplus
