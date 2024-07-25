// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#ifndef SRC_novadbPLUS_LOCK_LOCK_H__
#define SRC_novadbPLUS_LOCK_LOCK_H__

#include <memory>
#include <string>
#include <utility>

#include "rocksdb/port/lang.h"

#include "novadbplus/lock/mgl/mgl.h"
#include "novadbplus/lock/mgl/mgl_mgr.h"
#include "novadbplus/network/session_ctx.h"
#include "novadbplus/server/session.h"

namespace novadbplus {

class Session;

class ILock {
 public:
  ILock(ILock* parent, mgl::MGLock* lk, Session* sess, bool isRecursive);
  virtual ~ILock() TSAN_SUPPRESSION;
  mgl::LockMode getMode() const;
  mgl::LockRes getLockResult() const;
  virtual uint32_t getStoreId() const;
  virtual uint32_t getChunkId() const;
  virtual std::string getKey() const;

 protected:
  static mgl::LockMode getParentMode(mgl::LockMode mode);
  mgl::LockRes _lockResult;
  std::unique_ptr<ILock> _parent;
  std::unique_ptr<mgl::MGLock> _mgl;
  // not owned
  Session* _sess;
  bool _isRecursive;
};

// TODO(takenliu) : delete StoresLock
class StoresLock : public ILock {
 public:
  explicit StoresLock(mgl::LockMode mode,
                      Session* sess,
                      mgl::MGLockMgr* mgr,
                      uint64_t lockTimeoutMs = 3600000);
  virtual ~StoresLock() = default;

 private:
  static const char _target[];
};

class StoreLock : public ILock {
 public:
  static Expected<std::unique_ptr<StoreLock>> AquireStoreLock(
    uint32_t storeId,
    mgl::LockMode mode,
    Session* sess,
    mgl::MGLockMgr* mgr,
    uint64_t lockTimeoutMs = 3600000);
  StoreLock(uint32_t storeId,
            mgl::LockMode mode,
            Session* sess,
            mgl::MGLockMgr* mgr,
            uint64_t lockTimeoutMs = 3600000,
            bool isRecursive = false);
  uint32_t getStoreId() const final;
  virtual ~StoreLock() = default;

 private:
  uint32_t _storeId;
};

class ChunkLock : public ILock {
 public:
  static Expected<std::unique_ptr<ChunkLock>> AquireChunkLock(
    uint32_t storeId,
    uint32_t chunkId,
    mgl::LockMode mode,
    Session* sess,
    mgl::MGLockMgr* mgr,
    uint64_t lockTimeoutMs = 3600000);
  ChunkLock(uint32_t storeId,
            uint32_t chunkId,
            mgl::LockMode mode,
            Session* sess,
            mgl::MGLockMgr* mgr,
            uint64_t lockTimeoutMs = 3600000,
            bool isRecursive = false);
  uint32_t getStoreId() const final;
  uint32_t getChunkId() const final;
  virtual ~ChunkLock() = default;

 private:
  uint32_t _chunkId;
};

class KeyLock : public ILock {
 public:
  static Expected<std::unique_ptr<KeyLock>> AquireKeyLock(
    uint32_t storeId,
    uint32_t chunkId,
    const std::string& key,
    mgl::LockMode mode,
    Session* sess,
    mgl::MGLockMgr* mgr,
    uint64_t lockTimeoutMs = 3600000);
  KeyLock(uint32_t storeId,
          uint32_t chunkId,
          const std::string& key,
          mgl::LockMode mode,
          Session* sess,
          mgl::MGLockMgr* mgr,
          uint64_t lockTimeoutMs = 3600000);
  uint32_t getStoreId() const final;
  uint32_t getChunkId() const final;
  std::string getKey() const final;
  // remove lock from session before that lock has really been unlocked in its
  // parent's destructor.
  virtual ~KeyLock();

 private:
  const std::string _key;
};

}  // namespace novadbplus
#endif  //  SRC_novadbPLUS_LOCK_LOCK_H__
