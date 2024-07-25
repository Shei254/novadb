// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#ifndef SRC_novadbPLUS_UTILS_PARAM_MANAGER_H_
#define SRC_novadbPLUS_UTILS_PARAM_MANAGER_H_

#include <cstdint>
#include <map>
#include <string>

namespace novadbplus {

class ParamManager {
 public:
  void init(int argc, char** argv);
  uint64_t getUint64(const char* param, uint64_t default_value = 0) const;
  std::string getString(const char* param,
                        std::string default_value = "") const;

 private:
  std::map<std::string, std::string> _dict;
};

}  // namespace novadbplus
#endif  // SRC_novadbPLUS_UTILS_PARAM_MANAGER_H_
