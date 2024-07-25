// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#include "novadbplus/lock/mgl/mgl.h"

#include "novadbplus/lock/mgl/mgl_mgr.h"
#include "novadbplus/utils/invariant.h"
#include "novadbplus/utils/string.h"

namespace novadbplus {

namespace mgl {

std::atomic<uint64_t> MGLock::_idGen(0);
std::list<MGLock*> MGLock::_dummyList{};

MGLock::MGLock(MGLockMgr* mgr)
  : _id(_idGen.fetch_add(1, std::memory_order_relaxed)),
    _target(""),
    _targetHash(0),
    _mode(LockMode::LOCK_NONE),
    _res(LockRes::LOCKRES_UNINITED),
    _resIter(_dummyList.end()),
    _lockMgr(mgr),
    _threadId(getCurThreadId()) {
  INVARIANT_D(_lockMgr != nullptr);
}

MGLock::~MGLock() {
  INVARIANT_D(_res == LockRes::LOCKRES_UNINITED);
}

void MGLock::releaseLockResult() {
  std::lock_guard<std::mutex> lk(_mutex);
  _res = LockRes::LOCKRES_UNINITED;
  _resIter = _dummyList.end();
}

void MGLock::setLockResult(LockRes res, std::list<MGLock*>::iterator iter) {
  std::lock_guard<std::mutex> lk(_mutex);
  _res = res;
  _resIter = iter;
}

void MGLock::unlock() {
  LockRes status = getStatus();
  if (status != LockRes::LOCKRES_UNINITED) {
    INVARIANT_D(status == LockRes::LOCKRES_OK ||
                status == LockRes::LOCKRES_WAIT);
    _lockMgr->unlock(this);
    status = getStatus();
    INVARIANT_D(status == LockRes::LOCKRES_UNINITED);
  }
}

LockRes MGLock::lock(const std::string& target,
                     LockMode mode,
                     uint64_t timeoutMs) {
  _target = target;
  _mode = mode;
  INVARIANT_D(getStatus() == LockRes::LOCKRES_UNINITED);
  _resIter = _dummyList.end();
  if (_target != "") {
    _targetHash = static_cast<uint64_t>(std::hash<std::string>{}(_target));
  } else {
    _targetHash = 0;
  }
  _lockMgr->lock(this);
  if (getStatus() == LockRes::LOCKRES_OK) {
    return LockRes::LOCKRES_OK;
  }
  if (waitLock(timeoutMs)) {
    return LockRes::LOCKRES_OK;
  } else {
    return LockRes::LOCKRES_TIMEOUT;
  }
}

std::list<MGLock*>::iterator MGLock::getLockIter() const {
  return _resIter;
}

void MGLock::notify() {
  _cv.notify_one();
}

bool MGLock::waitLock(uint64_t timeoutMs) {
  std::unique_lock<std::mutex> lk(_mutex);
  return _cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this]() {
    return _res == LockRes::LOCKRES_OK;
  });
}

LockRes MGLock::getStatus() const {
  std::lock_guard<std::mutex> lk(_mutex);
  return _res;
}

std::string MGLock::toString() const {
  std::lock_guard<std::mutex> lk(_mutex);
  char buf[256];
  snprintf(buf,
           sizeof(buf),
           "id:%" PRIu64 " target:%s targetHash:%" PRIu64
           " LockMode:%s LockRes:%d threadId:%s",
           _id,
           _target.c_str(),
           _targetHash,
           lockModeRepr(_mode),
           static_cast<int>(_res),
           _threadId.c_str());
  return std::string(buf);
}

}  // namespace mgl
}  // namespace novadbplus
