// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#ifndef SRC_novadbPLUS_UTILS_STRING_H_
#define SRC_novadbPLUS_UTILS_STRING_H_

#include <algorithm>
#include <bitset>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "novadbplus/storage/varint.h"
#include "novadbplus/utils/status.h"

namespace novadbplus {

#define CLUSTER_SLOTS 16384
using byte = unsigned char;

std::string toLower(const std::string&);
std::string toUpper(const std::string&);

Expected<int32_t> stol(const std::string&);
Expected<uint64_t> stoul(const std::string&);
Expected<int64_t> stoll(const std::string&);
Expected<uint64_t> stoull(const std::string&);
Expected<long double> stold(const std::string&);
Expected<double> stod(const std::string& s);
std::string dtos(const double d);
std::string ldtos(const long double d, bool humanfriendly);
std::string itos(int32_t d);
std::string uitos(uint32_t d);
std::string ultos(uint64_t d);

std::string hexlify(const std::string&);
Expected<std::string> unhexlify(const std::string&);
bool isOptionOn(const std::string& s);
void sdstrim(std::string& s, const char* cset);

std::string& replaceAll(std::string& str,
                        const std::string& old_value,
                        const std::string& new_value);

const std::string& getCurThreadId();

using LenStrDecodeResult = std::pair<std::string, size_t>;
size_t lenStrEncode(std::stringstream& ss, const std::string& val);
std::string lenStrEncode(const std::string& val);
size_t lenStrEncode(char* dest, size_t destsize, const std::string& val);
size_t lenStrEncodeSize(const std::string& val);
Expected<LenStrDecodeResult> lenStrDecode(const std::string& str);
Expected<LenStrDecodeResult> lenStrDecode(const char* ptr, size_t max_size);

std::vector<std::string> stringSplit(const std::string& s,
                                     const std::string& delim);

std::string trim_left(const std::string& str);
std::string trim_right(const std::string& str);
std::string trim(const std::string& str);

Expected<std::pair<uint32_t, uint32_t>> getSlotRange(const std::string& s);

#define strDelete(str, c) \
  (str).erase(std::remove((str).begin(), (str).end(), (c)), (str).end())

std::string getUUid(const int len);
unsigned char random_char();

template <typename T>
void CopyUint(std::vector<uint8_t>* buf, T element) {
  for (size_t i = 0; i < sizeof(element); ++i) {
    buf->emplace_back((element >> ((sizeof(element) - i - 1) * 8)) & 0xff);
  }
}

template <size_t size>
std::vector<uint16_t> bitsetEncodeVec(const std::bitset<size>& bitmap) {
  size_t idx = 0;
  std::vector<uint16_t> slotBuff;
  while (idx < bitmap.size()) {
    if (bitmap.test(idx)) {
      uint16_t pageLen = 0;
      slotBuff.push_back(static_cast<uint16_t>(idx));
      while (idx < bitmap.size() && bitmap.test(idx)) {
        pageLen++;
        idx++;
      }
      slotBuff.push_back(pageLen);
    } else {
      idx++;
    }
  }
  return slotBuff;
}

template <size_t size>
Expected<std::bitset<size>> bitsetDecodeVec(const std::vector<uint16_t> vec) {
  std::bitset<size> bitmap;
  if (vec.size() % 2 != 0) {
    return {ErrorCodes::ERR_DECODE, "bitsetIntDecode bitset error length"};
  }

  int32_t last_pos = -1;
  size_t offset = 0;
  while (offset < vec.size()) {
    int32_t pos = vec[offset];
    if (pos <= last_pos) {
      return {ErrorCodes::ERR_DECODE, "bitset error input"};
    }

    auto pageLength = vec[offset + 1];
    offset += 2;
    auto len = static_cast<size_t>(pos + pageLength);
    if (len > size) {
      return {ErrorCodes::ERR_DECODE, "bitset error length"};
    }
    for (size_t j = pos; j < len; j++) {
      bitmap.set(j);
      last_pos = j;
    }
  }
  return bitmap;
}

template <size_t size>
uint32_t bitsetEncodeSize(const std::bitset<size>& bitmap) {
  auto vec = bitsetEncodeVec(bitmap);
  return sizeof(uint32_t) + vec.size() * sizeof(uint16_t);
}

template <size_t size>
std::string bitsetEncode(const std::bitset<size>& bitmap) {
  auto vec = bitsetEncodeVec(bitmap);
  std::vector<uint8_t> key;

  uint32_t encsize = sizeof(uint32_t) + sizeof(uint16_t) * vec.size();
  key.reserve(encsize);
  CopyUint(&key, encsize);
  for (auto& v : vec) {
    CopyUint(&key, v);
  }

  return std::string(reinterpret_cast<const char*>(key.data()), key.size());
}

template <size_t size>
Expected<std::bitset<size>> bitsetDecode(const char* str, size_t max_size) {
  std::bitset<size> bitmap;
  size_t offset = 0;

  if (max_size < sizeof(uint32_t)) {
    return {ErrorCodes::ERR_DECODE, "bitsetDecode too small"};
  }

  auto decodeSize = int32Decode(str);
  offset += sizeof(uint32_t);
  if (max_size < decodeSize) {
    return {ErrorCodes::ERR_DECODE, "bitsetDecode size too small"};
  }

  std::vector<uint16_t> vec;
  while (offset < decodeSize) {
    auto pos = int16Decode(str + offset);
    offset += sizeof(pos);
    vec.push_back(pos);
  }

  auto eBitmap = bitsetDecodeVec<size>(vec);
  if (!eBitmap.ok()) {
    return eBitmap.status();
  }

  return eBitmap.value();
}

template <size_t size>
Expected<std::bitset<size>> bitsetDecode(const std::string& str) {
  return bitsetDecode<size>(str.c_str(), str.size());
}

template <size_t size>
std::string bitsetStrEncode(const std::bitset<size>& bitmap) {
  size_t idx = 0;
  std::string slotStr = " ";
  while (idx < bitmap.size()) {
    if (bitmap.test(idx)) {
      size_t pos = idx;
      size_t pageLen = 0;
      std::stringstream tempStream;
      if (idx >= bitmap.size() - 1 || !bitmap.test(idx + 1)) {
        idx++;
        tempStream << pos;
      } else {
        idx++;
        while (idx < bitmap.size() && bitmap.test(idx)) {
          pageLen++;
          idx++;
        }
        tempStream << pos << "-" << pos + pageLen;
      }
      slotStr += tempStream.str() + " ";
    } else {
      idx++;
    }
  }
  return slotStr;
}

template <size_t size>
Expected<std::bitset<size>> bitsetStrDecode(const std::string bitmapStr) {
  std::bitset<size> bitmap;
  std::vector<std::string> vec = stringSplit(bitmapStr, " ");
  vec.erase(vec.begin());
  for (auto& vs : vec) {
    // TODO(wayenchen): the following cases should be considered:
    // a-b; -aaa; aaaa; aaaaa-; aaa-aaa-aa;
    if (vs.find("-") != std::string::npos) {
      std::vector<std::string> s = stringSplit(vs, "-");
      Expected<uint64_t> sPtr = ::novadbplus::stoul(s[0]);

      Expected<uint64_t> ePtr = ::novadbplus::stoul(s[1]);
      if (sPtr.ok() && ePtr.ok()) {
        size_t begin = static_cast<size_t>(sPtr.value());
        size_t end = static_cast<size_t>(ePtr.value());

        if (end >= size) {
          return {ErrorCodes::ERR_DECODE,
                  "bitsetStrDecode bitset error length"};
        }
        for (size_t j = begin; j <= end; j++) {
          bitmap.set(j);
        }
      } else {
        return {ErrorCodes::ERR_DECODE, "error start end "};
      }
    } else {
      Expected<uint64_t> sPtr = ::novadbplus::stoul(vs);
      if (sPtr.ok()) {
        size_t pos = static_cast<size_t>(sPtr.value());
        bitmap.set(pos);
      } else {
        return {ErrorCodes::ERR_DECODE, "error start end "};
      }
    }
  }
  return bitmap;
}

Expected<int64_t> getIntSize(const std::string& str);

template <typename T>
size_t easyCopy(std::vector<byte>* buf, size_t* pos, T element) {
  // TODO(jingjunli): reduce resize time
  if (*pos + sizeof(T) > buf->size()) {
    buf->resize(*pos + sizeof(T));
  }
  auto* ptr = reinterpret_cast<byte*>(&element);
  std::copy(ptr, (ptr + sizeof(T)), buf->begin() + *pos);
  *pos += sizeof(T);
  return sizeof(T);
}

template <typename T>
size_t easyCopy(std::vector<byte>* buf,
                size_t* pos,
                const T* array,
                size_t len) {
  if (*pos + len > buf->size()) {
    buf->resize(*pos + len * sizeof(T));
  }
  auto* ptr = const_cast<byte*>(reinterpret_cast<const byte*>(array));
  std::copy(ptr, (ptr + len * sizeof(T)), buf->begin() + *pos);
  *pos += len * sizeof(T);
  return len * sizeof(T);
}

template <typename T>
size_t easyCopy(T* dest, const std::string& buf, size_t* pos) {
  if (buf.size() < *pos) {
    return 0;
  }
  byte* ptr = reinterpret_cast<byte*>(dest);
  size_t end = *pos + sizeof(T);
  std::copy(&buf[*pos], &buf[end], ptr);
  *pos += sizeof(T);
  return sizeof(T);
}

}  // namespace novadbplus

#ifdef _MSC_VER
#define strcasecmp stricmp
#define strncasecmp strnicmp
#endif

#ifndef _WIN32
#if __has_include(<string_view>)
#include <string_view>
using std::string_view;
#define mystring_view string_view

#elif __has_include(<experimental/string_view>)
#include <experimental/string_view>  // NOLINT
using std::experimental::string_view;  // NOLINT
#define mystring_view string_view

#else
#error "no available string_view headfile"
#endif

#else
#define mystring_view std::string
#endif

#endif  // SRC_novadbPLUS_UTILS_STRING_H_
