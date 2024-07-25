// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#ifndef SRC_novadbPLUS_UTILS_FILE_H_
#define SRC_novadbPLUS_UTILS_FILE_H_

#include <memory>
#include <string>

#include "rocksdb/db.h"

#include "novadbplus/utils/status.h"

namespace novadbplus {

struct AlignedBuff {
  ~AlignedBuff() {
    free(buf);
  }
  char* buf;
  size_t bufSize;
  size_t logicalBlockSize;
};

std::shared_ptr<AlignedBuff> newAlignedBuff(const std::string& path,
                                            int32_t sizeMultiple = 16);

std::unique_ptr<rocksdb::WritableFile> openWritableFile(
  const std::string& fullFileName, bool use_direct_writes, bool reOpen);

}  // namespace novadbplus
#endif  // SRC_novadbPLUS_UTILS_FILE_H_
