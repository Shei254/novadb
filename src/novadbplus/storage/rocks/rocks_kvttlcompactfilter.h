// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#ifndef SRC_novadbPLUS_STORAGE_ROCKS_ROCKS_KVTTLCOMPACTFILTER_H_
#define SRC_novadbPLUS_STORAGE_ROCKS_ROCKS_KVTTLCOMPACTFILTER_H_

#include <memory>

#include "rocksdb/compaction_filter.h"
#include "rocksdb/env.h"

#include "novadbplus/storage/kvstore.h"
#include "novadbplus/storage/rocks/rocks_kvstore.h"

namespace novadbplus {

using rocksdb::CompactionFilter;
using rocksdb::CompactionFilterFactory;

struct KVTTLCompactionContext {
  bool is_manual_compaction;
};

class KVTtlCompactionFilterFactory : public CompactionFilterFactory {
 public:
  explicit KVTtlCompactionFilterFactory(KVStore* store,
                                        const std::shared_ptr<ServerParams> cfg)
    : _store(store), _cfg(cfg) {}

  const char* Name() const override {
    return "KVTTLCompactionFilterFactory";
  }

  std::unique_ptr<CompactionFilter> CreateCompactionFilter(
    const CompactionFilter::Context& /*context*/) override;

 private:
  KVStore* _store;
  const std::shared_ptr<ServerParams> _cfg;
};

}  // namespace novadbplus

#endif  // SRC_novadbPLUS_STORAGE_ROCKS_ROCKS_KVTTLCOMPACTFILTER_H_
