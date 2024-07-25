// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#include "novadbplus/commands/release.h"

#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#include "rocksdb/version.h"

#include "novadbplus/commands/version.h"
#include "novadbplus/utils/redis_port.h"

uint64_t redisBuildId(void) {
  std::stringstream buildidstr;
  buildidstr << novadbPLUS_VERSION_PRE << __ROCKSDB_MAJOR__ << "."
             << __ROCKSDB_MINOR__ << "." << __ROCKSDB_PATCH__
             << novadbPLUS_BUILD_ID << novadbPLUS_GIT_DIRTY
             << novadbPLUS_GIT_SHA1;

  std::string buildid = buildidstr.str();
  return novadbplus::redis_port::crc64(
    0, (unsigned char*)buildid.c_str(), buildid.length());
}

std::string getnovadbPlusVersion() {
  std::stringstream novadbver;

  novadbver << novadbPLUS_VERSION_PRE << __ROCKSDB_MAJOR__ << "."
            << __ROCKSDB_MINOR__ << "." << __ROCKSDB_PATCH__;
  return novadbver.str();
}
