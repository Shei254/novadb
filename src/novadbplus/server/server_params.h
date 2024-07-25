// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#ifndef SRC_novadbPLUS_SERVER_SERVER_PARAMS_H_
#define SRC_novadbPLUS_SERVER_SERVER_PARAMS_H_

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "glog/logging.h"
#include "rocksdb/port/lang.h"

#include "novadbplus/server/session.h"
#include "novadbplus/utils/redis_port.h"
#include "novadbplus/utils/status.h"
#include "novadbplus/utils/string.h"

namespace novadbplus {

using funptr = std::function<void()>;
using checkfunptr =
  std::function<bool(const std::string&, bool startup, std::string* errinfo)>;
using preProcess = std::function<std::string(const std::string&)>;

std::string removeQuotes(const std::string& v);
std::string removeQuotesAndToLower(const std::string& v);
void NoUseWarning(const std::string& name);

class BaseVar {
 public:
  BaseVar(const std::string& s,
          void* v,
          checkfunptr ptr,
          preProcess preFun,
          bool allowDS)
    : name(s),
      value(v),
      Onupdate(nullptr),
      checkFun(ptr),
      preProcessFun(preFun),
      allowDynamicSet(allowDS) {
    if (v == NULL) {
      assert(false);
      return;
    }
  }
  virtual ~BaseVar() {}
  Status setVar(const std::string& value, bool startup = true) {
    if (!allowDynamicSet && !startup) {
      return {ErrorCodes::ERR_PARSEOPT, name + " can't change dynamically"};
    }
    return set(value, startup);
  }
  virtual bool need_show() const {
    return true;
  }
  virtual std::string show() const = 0;
  virtual std::string default_show() const = 0;
  void setUpdate(funptr f) {
    Onupdate = f;
  }

  std::string getName() const {
    return name;
  }

  bool isallowDynamicSet() const {
    return allowDynamicSet;
  }

 protected:
  virtual Status set(const std::string& value, bool startup) = 0;
  virtual bool check(const std::string& value,
                     bool startup,
                     std::string* errinfo = NULL) {
    if (checkFun != NULL) {
      return checkFun(value, startup, errinfo);
    }
    return true;
  }

  std::string name = "";
  void* value = NULL;
  funptr Onupdate = NULL;
  checkfunptr checkFun = NULL;  // check the value whether it is valid
  preProcess preProcessFun =
    NULL;  // pre process for the value, such as remove the quotes
  bool allowDynamicSet = false;
};

class StringVar : public BaseVar {
 public:
  StringVar(const std::string& name,
            void* v,
            checkfunptr ptr,
            preProcess preFun,
            bool allowDynamicSet)
    : BaseVar(name, v, ptr, preFun, allowDynamicSet),
      _defaultValue(*reinterpret_cast<std::string*>(value)) {
    if (!preProcessFun) {
      preProcessFun = removeQuotes;
    }
  }
  virtual std::string show() const {
    return "\"" + *reinterpret_cast<std::string*>(value) + "\"";
  }

  virtual std::string default_show() const {
    return "\"" + _defaultValue + "\"";
  }

 private:
  Status set(const std::string& val, bool startup) {
    auto v = preProcessFun ? preProcessFun(val) : val;
    std::string errinfo;
    if (!check(v, startup, &errinfo)) {
      return {ErrorCodes::ERR_PARSEOPT, errinfo};
    }

    *reinterpret_cast<std::string*>(value) = v;

    if (Onupdate != NULL)
      Onupdate();
    return {ErrorCodes::ERR_OK, ""};
  }
  std::string _defaultValue;
};

// support:int, uint32_t
class IntVar : public BaseVar {
 public:
  IntVar(const std::string& name,
         void* v,
         checkfunptr ptr,
         preProcess preFun,
         int64_t minVal,
         int64_t maxVal,
         bool allowDynamicSet)
    : BaseVar(name, v, ptr, preFun, allowDynamicSet),
      _defaultValue(*reinterpret_cast<int*>(value)),
      _minVal(minVal),
      _maxVal(maxVal) {}
  virtual std::string show() const {
    return std::to_string(*reinterpret_cast<int*>(value));
  }
  virtual std::string default_show() const {
    return std::to_string(_defaultValue);
  }

 private:
  TSAN_SUPPRESSION Status set(const std::string& val, bool startup) {
    auto v = preProcessFun ? preProcessFun(val) : val;
    std::string errinfo;
    if (!check(v, startup, &errinfo)) {
      return {ErrorCodes::ERR_PARSEOPT, errinfo};
    }

    auto eInt = novadbplus::stoll(v);
    if (!eInt.ok()) {
      return eInt.status();
    }
    int64_t valTemp = eInt.value();
    if (valTemp < _minVal || valTemp > _maxVal) {
      return {ErrorCodes::ERR_PARSEOPT, name + " is out of range"};
    }
    *reinterpret_cast<int*>(value) = valTemp;

    if (Onupdate != NULL)
      Onupdate();

    return {ErrorCodes::ERR_OK, ""};
  }
  int _defaultValue;
  int64_t _minVal;
  int64_t _maxVal;
};

// support:int64_t, uint64_t
class Int64Var : public BaseVar {
 public:
  Int64Var(const std::string& name,
           void* v,
           checkfunptr ptr,
           preProcess preFun,
           int64_t minVal,
           int64_t maxVal,
           bool allowDynamicSet)
    : BaseVar(name, v, ptr, preFun, allowDynamicSet),
      _defaultValue(*reinterpret_cast<int64_t*>(value)),
      _minVal(minVal),
      _maxVal(maxVal) {}
  virtual std::string show() const {
    return std::to_string(*reinterpret_cast<int64_t*>(value));
  }
  virtual std::string default_show() const {
    return std::to_string(_defaultValue);
  }

 private:
  TSAN_SUPPRESSION Status set(const std::string& val, bool startup) {
    auto v = preProcessFun ? preProcessFun(val) : val;
    std::string errinfo;
    if (!check(v, startup, &errinfo)) {
      return {ErrorCodes::ERR_PARSEOPT, errinfo};
    }

    auto eInt = novadbplus::stoll(v);
    if (!eInt.ok()) {
      return eInt.status();
    }
    int64_t valTemp = eInt.value();
    if (valTemp < _minVal || valTemp > _maxVal) {
      return {ErrorCodes::ERR_PARSEOPT, name + " is out of range"};
    }
    *reinterpret_cast<int64_t*>(value) = valTemp;

    if (Onupdate != NULL)
      Onupdate();

    return {ErrorCodes::ERR_OK, ""};
  }
  int64_t _defaultValue;
  int64_t _minVal;
  int64_t _maxVal;
};

class FloatVar : public BaseVar {
 public:
  FloatVar(const std::string& name,
           void* v,
           checkfunptr ptr,
           preProcess preFun,
           bool allowDynamicSet)
    : BaseVar(name, v, ptr, preFun, allowDynamicSet),
      _defaultValue(*reinterpret_cast<float*>(value)) {}
  virtual std::string show() const {
    return std::to_string(*reinterpret_cast<float*>(value));
  }
  virtual std::string default_show() const {
    return std::to_string(_defaultValue);
  }

 private:
  TSAN_SUPPRESSION Status set(const std::string& val, bool startup) {
    auto v = preProcessFun ? preProcessFun(val) : val;
    std::string errinfo;
    if (!check(v, startup, &errinfo)) {
      return {ErrorCodes::ERR_PARSEOPT, errinfo};
    }

    auto eFloat = novadbplus::stold(v);
    if (!eFloat.ok()) {
      return eFloat.status();
    }
    *reinterpret_cast<float*>(value) = static_cast<float>(eFloat.value());

    if (Onupdate != NULL)
      Onupdate();
    return {ErrorCodes::ERR_OK, ""};
  }
  float _defaultValue;
};

class DoubleVar : public BaseVar {
 public:
  DoubleVar(const std::string& name,
            void* v,
            checkfunptr ptr,
            preProcess preFun,
            bool allowDynamicSet)
    : BaseVar(name, v, ptr, preFun, allowDynamicSet),
      _defaultValue(*reinterpret_cast<double*>(value)) {}
  virtual std::string show() const {
    return std::to_string(*reinterpret_cast<double*>(value));
  }
  virtual std::string default_show() const {
    return std::to_string(_defaultValue);
  }

 private:
  Status set(const std::string& val, bool startup) {
    auto v = preProcessFun ? preProcessFun(val) : val;
    std::string errinfo;
    if (!check(v, startup, &errinfo)) {
      return {ErrorCodes::ERR_PARSEOPT, errinfo};
    }

    auto eDouble = novadbplus::stold(v);
    if (!eDouble.ok()) {
      return eDouble.status();
    }
    *reinterpret_cast<double*>(value) = static_cast<double>(eDouble.value());

    if (Onupdate != NULL)
      Onupdate();
    return {ErrorCodes::ERR_OK, ""};
  }
  double _defaultValue;
};

class BoolVar : public BaseVar {
 public:
  BoolVar(const std::string& name,
          void* v,
          checkfunptr ptr,
          preProcess preFun,
          bool allowDynamicSet)
    : BaseVar(name, v, ptr, preFun, allowDynamicSet),
      _defaultValue(*reinterpret_cast<bool*>(value)) {}
  virtual std::string show() const {
    return *reinterpret_cast<bool*>(value) ? "yes" : "no";
  }
  virtual std::string default_show() const {
    return _defaultValue ? "yes" : "no";
  }

 private:
  TSAN_SUPPRESSION Status set(const std::string& val, bool startup) {
    auto v = preProcessFun ? preProcessFun(val) : val;
    std::string errinfo;
    if (!check(v, startup, &errinfo)) {
      return {ErrorCodes::ERR_PARSEOPT, errinfo};
    }

    *reinterpret_cast<bool*>(value) = isOptionOn(v);

    if (Onupdate != NULL)
      Onupdate();
    return {ErrorCodes::ERR_OK, ""};
  }
  bool _defaultValue;
};

class NoUseVar : public BaseVar {
 public:
  NoUseVar(const std::string& name,
           void* v,
           checkfunptr ptr,
           preProcess preFun,
           bool allowDynamicSet)
    : BaseVar(name, v, ptr, preFun, allowDynamicSet), _setFlag(false) {}
  virtual std::string show() const {
    return " not supported anymore";
  }
  virtual std::string default_show() const {
    return "no";
  }
  virtual bool need_show() const {
    return _setFlag;
  }

 private:
  TSAN_SUPPRESSION Status set(const std::string& val, bool startup) {
    _setFlag = true;
    NoUseWarning(name);
    return {ErrorCodes::ERR_OK, ""};
  }
  bool _setFlag;
};

class rewriteConfigState {
 public:
  rewriteConfigState() : _hasTail(false) {}
  ~rewriteConfigState() {}
  Status rewriteConfigReadOldFile(const std::string& confFile);
  void rewriteConfigOption(const std::string& option,
                           const std::string& value,
                           const std::string& defvalue);
  void rewriteConfigRewriteLine(const std::string& option,
                                const std::string& line,
                                bool force);
  std::string rewriteConfigFormatMemory(uint64_t bytes);
  void rewriteConfigRemoveOrphaned();
  std::string rewriteConfigGetContentFromState();
  Status rewriteConfigOverwriteFile(const std::string& confFile,
                                    const std::string& content);

 private:
  std::unordered_map<std::string, std::list<uint64_t>> _optionToLine;
  std::unordered_map<std::string, std::list<uint64_t>> _rewritten;
  std::vector<std::string> _lines;
  bool _hasTail;
  const std::string _fixInfo = "# Generated by CONFIG REWRITE";
};

typedef std::unordered_map<std::string, std::string> ParamsMap;
class ServerParams {
 public:
  ServerParams();
  ~ServerParams();

  Status parseFile(const std::string& filename);
  bool registerOnupdate(const std::string& name, funptr ptr);
  std::string showAll() const;
  bool showVar(const std::string& key, std::string* info) const;
  bool showVar(const std::string& key, std::vector<std::string>* info) const;
  Status setRocksOption(const std::string& name, const std::string& value);
  Status setVar(const std::string& name,
                const std::string& value,
                bool startup = true);
  Status rewriteConfig() const;
  uint32_t paramsNum() const {
    return _mapServerParams.size();
  }
  std::string getConfFile() const {
    return _confFile;
  }
  const ParamsMap& getRocksdbOptions() const {
    return _rocksdbOptions;
  }
  const ParamsMap* getRocksdbCFOptions(const std::string& cf) const {
    auto it = _rocksdbCFOptions.find(cf);
    if (it == _rocksdbCFOptions.end()) {
      return nullptr;
    }
    return &it->second;
  }
  BaseVar* serverParamsVar(const std::string& key) {
    return _mapServerParams[novadbplus::toLower(key)];
  }

 private:
  Status checkParams();

 private:
  std::map<std::string, BaseVar*> _mapServerParams;
  ParamsMap _rocksdbOptions;
  std::unordered_map<std::string, ParamsMap> _rocksdbCFOptions;
  std::string _confFile = "";
  std::set<std::string> _setConfFile;

 public:
  std::string bindIp = "127.0.0.1";
  std::string bindIp2 = "";
  uint32_t port = 8903;
  std::string logLevel = "";
  std::string logDir = "./";
  uint32_t logSizeMb = 128;
  bool daemon = true;

  std::string storageEngine = "rocks";
  std::string dbPath = "./db";
  std::string dumpPath = "./dump";
  std::string requirepass = "";
  std::string masterauth = "";
  std::string pidFile = "./novadbplus.pid";
  bool versionIncrease = true;
  bool generalLog = false;
  // false: For command "set a b", it don't check the type of
  // "a" and update it directly. It can make set() faster.
  // Default false. Redis layer can guarantee that it's safe
  bool checkKeyTypeForSet = false;

  uint32_t chunkSize = 0x4000;  // same as rediscluster
  // forward compatible only
  uint32_t fakeChunkSize = 0x4000;
  uint32_t kvStoreCount = 10;

  uint32_t scanCntIndexMgr = 1000;
  uint32_t scanJobCntIndexMgr = 1;
  uint32_t delCntIndexMgr = 10000;
  uint32_t delJobCntIndexMgr = 1;
  uint32_t pauseTimeIndexMgr = 1;
  uint64_t elementLimitForSingleDelete = 2048;
  uint64_t elementLimitForSingleDeleteZset = 1024;

  uint32_t protoMaxBulkLen = CONFIG_DEFAULT_PROTO_MAX_BULK_LEN;
  uint32_t dbNum = CONFIG_DEFAULT_DBNUM;

  bool noexpire = false;
  bool noexpireBlob = false;
  uint64_t maxBinlogKeepNum = 1;
  uint32_t minBinlogKeepSec = 3600;
  uint64_t slaveBinlogKeepNum = 1;
  uint64_t dumpFileKeepNum = 0;
  uint64_t dumpFileKeepHour = 0;
  bool dumpFileFlush = true;

  uint32_t maxClients = CONFIG_DEFAULT_MAX_CLIENTS;
  std::string slowlogPath = "./slowlog";
  uint64_t slowlogLogSlowerThan = CONFIG_DEFAULT_SLOWLOG_LOG_SLOWER_THAN;
  uint64_t slowlogMaxLen = CONFIG_DEFAULT_SLOWLOG_LOG_MAX_LEN;
  uint64_t novadbLatencyLimit = 0;   // us
  uint64_t rocksdbLatencyLimit = 0;  // us
  bool slowlogFileEnabled = true;
  bool binlogUsingDefaultCF = false;

  // If false, novadb don't save binlog when write data. Without Binlog, novadb
  // write will faster.
  // NOT SUPPORTED Replication and Cluster Replicate.
  // Read the configuration through 'config get binlog-enabled'
  bool binlogEnabled = true;

  // If false, novadb do not dump binlog to 'dumpPath'. binlog-save-logs
  // Read the configuration through 'config get binlog-save-logs'
  bool binlogSaveLogs = true;

  uint32_t netIoThreadNum = 0;
  uint32_t executorThreadNum = 0;
  uint32_t executorWorkPoolSize = 0;
  bool simpleWorkPoolName = false;

  uint32_t binlogRateLimitMB = 64;
  uint32_t netBatchSize = 1024 * 1024;
  uint32_t netBatchTimeoutSec = 10;
  uint32_t timeoutSecBinlogWaitRsp = 3;
  uint32_t incrPushThreadnum = 10;
  uint32_t fullPushThreadnum = 5;
  uint32_t fullReceiveThreadnum = 5;
  uint32_t logRecycleThreadnum = 5;
  uint32_t truncateBinlogIntervalMs = 1000;
  uint32_t truncateBinlogNum = 10000;
  uint32_t binlogFileSizeMB = 64;
  uint32_t binlogFileSecs = 20 * 60;

  uint32_t keysDefaultLimit = 100;
  uint32_t lockWaitTimeOut = 3600;
  uint32_t lockDbXWaitTimeout = 1;
  bool ignoreKeyLock = false;  // only for test

  // parameter for scan command
  uint32_t scanDefaultLimit = 10;
  uint32_t scanDefaultMaxIterateTimes = 10000;

  // parameter for rocksdb
  uint32_t rocksBlockcacheMB = 4096;
  int32_t rocksBlockcacheNumShardBits = 6;
  uint32_t rocksRowcacheMB = 0;
  bool rocksBlobcacheInBlockcache = false;
  uint32_t rocksBlobcacheMB = 0;
  int32_t rocksBlobcacheNumShardBits = 6;
  int64_t rocksRateLimiterRateBytesPerSec = 0;
  int64_t rocksRateLimiterRefillPeriodUs = 100 * 1000;
  int64_t rocksRateLimiterFairness = 10;
  bool rocksRateLimiterAutoTuned = true;
  bool rocksStrictCapacityLimit = false;
  std::string rocksWALDir = "";
  std::string rocksCompressType = "snappy";
  int32_t rocksMaxOpenFiles = -1;
  int32_t rocksMaxBackgroundJobs = 2;
  uint32_t rocksCompactOnDeletionWindow = 0;
  uint32_t rocksCompactOnDeletionTrigger = 0;
  double rocksCompactOnDeletionRatio = 0;
  uint32_t rocksTransactionMode = 2;
  int64_t rocksDeleteBytesPerSecond = 0;

  // WriteOptions
  bool rocksDisableWAL = false;
  bool rocksFlushLogAtTrxCommit = false;
  bool level0Compress = false;
  bool level1Compress = false;

  // TxnOptions
  // #if ROCKSDB_MAJOR > 5 || (ROCKSDB_MAJOR == 5 && ROCKSDB_MINOR > 17)
  bool skipConcurrencyControl = false;
  // #endif

  bool bgcompactEnabled = false;
  uint64_t bgcompactInterval = 60;  // s
  int bgcompactBegin = 0;
  int bgcompactEnd = 7;
  uint32_t bgcompactForceDeletePercentage = 10;

  uint32_t binlogSendBatch = 256;
  uint32_t binlogSendBytes = 16 * 1024 * 1024;

  uint32_t migrateSenderThreadnum = 5;
  uint32_t migrateReceiveThreadnum = 5;

  bool clusterEnabled = false;
  bool domainEnabled = false;
  bool slaveReconfEnabled = true;
  bool slaveMigarateEnabled = false;
  bool clusterAllowReplicaMigration = false;
  bool aofEnabled = false;
  bool psyncEnabled = false;
  bool fullPsyncNoticeEnable = false;
  bool replicateFixEnable = true;
  uint32_t forceRecovery = 0;

  uint32_t aofPsyncNum = 500;
  uint32_t snapShotRetryCnt = 1000;

  uint32_t migrateTaskSlotsLimit = 10;
  uint32_t migrateDistance = 10000;
  uint32_t migrateBinlogIter = 10;
  uint32_t migrateRateLimitMB = 32;
  uint32_t migrateSnapshotKeyNum = 100000;
  uint32_t supplyFullPsyncKeyBatchNum = 100;

  // The Batch Size when sending snapshot during migration.
  // Dynamically changeable through 'config set cluster-migration-batch-size'
  uint32_t migrateSnapshotBatchSizeKB = 16;

  // The network timeout during migration, it used in following scenarios:
  // 1) The source node send data timeout
  // 2) The destination node side reply data timeout
  // Dynamically changeable through 'config set cluster-migration-timeout'
  uint32_t migrateNetworkTimeout = 5;  // second

  uint32_t clusterNodeTimeout = 15000;
  bool clusterRequireFullCoverage = true;
  bool clusterSlaveNoFailover = false;
  uint32_t clusterMigrationBarrier = 1;
  uint32_t clusterSlaveValidityFactor = 10;
  bool clusterSingleNode = false;
  bool clusterCheckDiskBeforePong = false;
  bool clusterCheckDiskWrite = false;
  bool clusterCheckDiskRead = false;

  uint64_t tbitmapFragmentSize = 1024;

  int64_t luaTimeLimit = 5000;                   // ms
  int64_t luaStateMaxIdleTime = 60 * 60 * 1000;  // ms
  bool jeprofAutoDump = true;
  bool enableJemallocBgThread = true;
  bool deleteFilesInRangeForMigrateGc = true;
  bool compactRangeAfterDeleteRange = false;
  bool logError = false;
  bool directIo = false;
  bool allowCrossSlot = false;
  uint32_t generateHeartbeatBinlogInterval = 0;  // s
  int64_t waitTimeIfExistsMigrateTask = 600;     // s
  uint64_t clientOutputBufferLimitNormalHardMB = 0;
  uint64_t clientOutputBufferLimitNormalSoftMB = 0;
  uint64_t clientOutputBufferLimitNormalSoftSecond = 10;
  bool moveDirWhenRestoreCkpt = false;
};

extern std::shared_ptr<novadbplus::ServerParams> gParams;
}  // namespace novadbplus

#endif  // SRC_novadbPLUS_SERVER_SERVER_PARAMS_H_
