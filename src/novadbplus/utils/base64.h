// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#ifndef SRC_novadbPLUS_UTILS_BASE64_H_
#define SRC_novadbPLUS_UTILS_BASE64_H_

#include <string>

class Base64 {
 public:
  static std::string Encode(const unsigned char* str, int bytes);
  static std::string Decode(const char* str, int bytes);
};
#endif  // SRC_novadbPLUS_UTILS_BASE64_H_
