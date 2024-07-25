// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#ifndef SRC_novadbPLUS_SERVER_INDEX_MANAGER_H_
#define SRC_novadbPLUS_SERVER_INDEX_MANAGER_H_

#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "novadbplus/network/worker_pool.h"
#include "novadbplus/server/server_entry.h"

namespace novadbplus {

using JobStatus = std::unordered_map<std::size_t, std::atomic<bool>>;
using JobCnt = std::unordered_map<std::size_t, std::atomic<uint32_t>>;

class IndexManager {
 public:
  IndexManager(std::shared_ptr<ServerEntry> svr,
               const std::shared_ptr<ServerParams>& cfg);
  Status startup();
  void stop();
  Status run();
  Status scanExpiredKeysJob(uint32_t storeId);
  int tryDelExpiredKeysJob(uint32_t storeId);
  bool isRunning();
  Status stopStore(uint32_t storeId);
  void indexScannerResize(size_t size);
  void keyDeleterResize(size_t size);
  size_t indexScannerSize();
  size_t keyDeleterSize();
  size_t scanExpiredCount() const {
    return _totalEnqueue;
  }
  size_t delExpiredCount() const {
    return _totalDequeue;
  }
  std::string getInfoString();

 private:
  std::unique_ptr<WorkerPool> _indexScanner;
  std::unique_ptr<WorkerPool> _keyDeleter;
  std::unordered_map<std::size_t, std::list<TTLIndex>> _expiredKeys;
  std::unordered_map<std::size_t, std::string> _scanPoints;
  std::vector<uint64_t> _scanPonitsTtl;
  JobStatus _scanJobStatus;
  JobStatus _delJobStatus;
  // when destroystore, _disableStatus[storeId] = true
  JobStatus _disableStatus;
  JobCnt _scanJobCnt;
  JobCnt _delJobCnt;

  std::atomic<bool> _isRunning;
  std::shared_ptr<ServerEntry> _svr;
  std::shared_ptr<ServerParams> _cfg;
  std::thread _runner;
  std::mutex _mutex;

  std::shared_ptr<PoolMatrix> _scannerMatrix;
  std::shared_ptr<PoolMatrix> _deleterMatrix;

  std::atomic<uint64_t> _totalDequeue;
  std::atomic<uint64_t> _totalEnqueue;
};

}  // namespace novadbplus

#endif  // SRC_novadbPLUS_SERVER_INDEX_MANAGER_H_
