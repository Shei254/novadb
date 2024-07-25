// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#include <algorithm>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "rocksdb/utilities/table_properties_collectors.h"

#include "novadbplus/commands/command.h"
#include "novadbplus/server/server_entry.h"
#include "novadbplus/server/server_params.h"
#include "novadbplus/storage/rocks/rocks_kvstore.h"
#include "novadbplus/utils/invariant.h"
#include "novadbplus/utils/portable.h"
#include "novadbplus/utils/redis_port.h"
#include "novadbplus/utils/scopeguard.h"
#include "novadbplus/utils/status.h"
#include "novadbplus/utils/string.h"
#include "novadbplus/utils/sync_point.h"
#include "novadbplus/utils/test_util.h"

namespace novadbplus {

Expected<std::string> recordList2Aof(const std::list<Record>& list);
Expected<std::string> key2Aof(Session* sess, const std::string& key);

void testSetRetry(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);
  NetSession sess1(svr, std::move(socket1), 1, false, nullptr, nullptr);

  uint32_t cnt = 0;
  const auto guard =
    MakeGuard([] { SyncPoint::GetInstance()->ClearAllCallBacks(); });
  SyncPoint::GetInstance()->EnableProcessing();
  SyncPoint::GetInstance()->SetCallBack("setGeneric::SetKV::1", [&](void* arg) {
    ++cnt;
    if (cnt % 2 == 1) {
      sess1.setArgs({"set", "a", "1"});
      auto expect = Command::runSessionCmd(&sess1);
      EXPECT_TRUE(expect.ok());
      EXPECT_EQ(expect.value(), Command::fmtOK());
    }
  });

  sess.setArgs({"set", "a", "1"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(cnt, uint32_t(6));
  EXPECT_EQ(expect.status().code(), ErrorCodes::ERR_COMMIT_RETRY);
}

void testDel(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  // bounder for optimistic del/pessimistic del
  for (auto v : {1000u, 10000u}) {
    sess.setArgs({"set", "a", "b"});
    auto expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    EXPECT_EQ(expect.value(), Command::fmtOK());

    sess.setArgs({"del", "a"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    EXPECT_EQ(expect.value(), Command::fmtOne());

    for (uint32_t i = 0; i < v; i++) {
      sess.setArgs({"lpush", "a", std::to_string(2 * i)});
      auto expect = Command::runSessionCmd(&sess);
      EXPECT_TRUE(expect.ok());
      EXPECT_EQ(expect.value(), Command::fmtLongLong(i + 1));
    }

    sess.setArgs({"get", "a"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(!expect.ok());

    sess.setArgs({"expire", "a", std::to_string(1)});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    EXPECT_EQ(expect.value(), Command::fmtOne());

    sess.setArgs({"del", "a"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    EXPECT_EQ(expect.value(), Command::fmtOne());

    sess.setArgs({"llen", "a"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    EXPECT_EQ(expect.value(), Command::fmtZero());

    sess.setArgs({"get", "a"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    EXPECT_EQ(expect.value(), Command::fmtNull());
  }
  for (auto v : {1000u, 10000u}) {
    for (uint32_t i = 0; i < v; i++) {
      sess.setArgs({"lpush", "a", std::to_string(2 * i)});
      auto expect = Command::runSessionCmd(&sess);
      EXPECT_TRUE(expect.ok());
      EXPECT_EQ(expect.value(), Command::fmtLongLong(i + 1));
    }

    sess.setArgs({"expire", "a", std::to_string(1)});
    auto expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    EXPECT_EQ(expect.value(), Command::fmtOne());

    std::this_thread::sleep_for(std::chrono::seconds(2));
    sess.setArgs({"del", "a"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    EXPECT_EQ(expect.value(), Command::fmtZero());
  }

  for (int i = 0; i < 10000; ++i) {
    sess.setArgs({"zadd", "testzsetdel", std::to_string(i), std::to_string(i)});
    auto expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
  }
  const auto guard =
    MakeGuard([] { SyncPoint::GetInstance()->ClearAllCallBacks(); });
  std::cout << "begin delete zset" << std::endl;
  SyncPoint::GetInstance()->EnableProcessing();
  SyncPoint::GetInstance()->SetCallBack(
    "delKeyPessimistic::TotalCount", [&](void* arg) {
      uint64_t v = *(static_cast<uint64_t*>(arg));
      EXPECT_EQ(v, 20001U);
    });
  sess.setArgs({"del", "testzsetdel"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
}

void testSpopOptimize(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext, ioContext2;
  asio::ip::tcp::socket socket(ioContext), socket2(ioContext2);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  sess.setArgs({"sadd", "kv_2", "val_0", "val_1", "val_2"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"sadd", "kv_2", "val_3", "val_4", "val_5", "val_6"});
  auto expectAdd = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expectAdd.ok());

  sess.setArgs({"scard", "kv_2"});
  auto expectScard = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expectScard.ok());
  EXPECT_EQ(expectScard.value(), ":7\r\n");

  sess.setArgs({"spop", "kv_2", "2"});
  auto expect1 = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(expect1.value(), "*2\r\n$5\r\nval_0\r\n$5\r\nval_1\r\n");

  sess.setArgs({"srem", "kv_2", "val_2"});
  auto expect2 = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect2.ok());
  EXPECT_EQ(expect2.value(), ":1\r\n");

  sess.setArgs({"spop", "kv_2", "2"});
  auto expect3 = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect3.ok());
  EXPECT_EQ(expect3.value(), "*2\r\n$5\r\nval_3\r\n$5\r\nval_4\r\n");

  sess.setArgs({"sadd", "kv_2", "val_0"});
  auto expect4 = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect4.ok());
  EXPECT_EQ(expect4.value(), ":1\r\n");

  sess.setArgs({"sadd", "kv_2", "val_1"});
  auto expect5 = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect5.ok());
  EXPECT_EQ(expect5.value(), ":1\r\n");

  sess.setArgs({"spop", "kv_2", "2"});
  auto expect6 = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect6.ok());
  EXPECT_EQ(expect6.value(), "*2\r\n$5\r\nval_0\r\n$5\r\nval_1\r\n");

  sess.setArgs({"spop", "kv_2", "1"});
  auto expect7 = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect7.ok());
  EXPECT_EQ(expect7.value(), "*1\r\n$5\r\nval_5\r\n");

  sess.setArgs({"spop", "kv_2", "1"});
  auto expect9 = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect9.ok());
  EXPECT_EQ(expect9.value(), "*1\r\n$5\r\nval_6\r\n");

  sess.setArgs({"scard", "kv_2"});
  auto expect8 = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect8.ok());
  EXPECT_EQ(expect8.value(), ":0\r\n");
}

TEST(Command, del) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  testDel(server);
  testSpopOptimize(server);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

TEST(Command, expire) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());

  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  testExpireForImmediately(server);
  testExpireForAlreadyExpired1(server);
  testExpireForAlreadyExpired2(server);
  testExpireCommandWhenNoexpireTrue(server);
  testExpireKeyWhenGet(server);
  testExpireKeyWhenCompaction(server);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

void testExtendProtocol(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  sess.setArgs({"config", "set", "session", "novadb_protocol_extend", "1"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"sadd", "ss", "a", "100", "100", "v1"});
  auto s = sess.processExtendProtocol();
  EXPECT_TRUE(s.ok());
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(sess.getServerEntry()->getTsEp(), 100);

  sess.setArgs({"sadd", "ss", "b", "101", "101", "v1"});
  s = sess.processExtendProtocol();
  EXPECT_TRUE(s.ok());
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(sess.getServerEntry()->getTsEp(), 101);

  sess.setArgs({"sadd", "ss", "c", "102", "a", "v1"});
  s = sess.processExtendProtocol();
  EXPECT_TRUE(!s.ok());
  EXPECT_EQ(sess.getServerEntry()->getTsEp(), 101);

  std::stringstream ss1;
  sess.setArgs({"smembers", "ss", "102", "102", "v1"});
  s = sess.processExtendProtocol();
  EXPECT_TRUE(s.ok());
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  ss1.str("");
  Command::fmtMultiBulkLen(ss1, 2);
  Command::fmtBulk(ss1, "a");
  Command::fmtBulk(ss1, "b");
  EXPECT_EQ(ss1.str(), expect.value());

  // version ep behaviour test -- hash
  {
    sess.setArgs({"hset", "hash", "key", "1000", "100", "100", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    // for normal occasion, smaller version can't overwrite greater op.
    sess.setArgs({"hset", "hash", "key", "999", "101", "99", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(!expect.ok());

    // cmd with no EP can modify key's which version is not -1
    sess.setArgs({"hset", "hash", "key1", "10"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    // cmd with greater version is allowed.
    sess.setArgs({"hset", "hash", "key1", "1080", "102", "102", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    sess.setArgs({"hget", "hash", "key1", "103", "103", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    EXPECT_EQ(Command::fmtBulk("1080"), expect.value());

    sess.setArgs({"hincrby", "hash", "key1", "1", "101", "101", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(!expect.ok());
    sess.setArgs({"hincrby", "hash", "key1", "2", "103", "103", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    sess.setArgs({"hget", "hash", "key1", "104", "104", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    EXPECT_EQ(Command::fmtBulk("1082"), expect.value());

    sess.setArgs({"hset", "hash2", "key2", "ori"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    // overwrite version.
    sess.setArgs({"hset", "hash2", "key2", "EPset", "100", "100", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"hset", "hash2", "key2", "naked"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"hget", "hash2", "key2", "100", "100", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    EXPECT_EQ(Command::fmtBulk("naked"), expect.value());
  }

  {
    sess.setArgs({"zadd", "zset1", "5", "foo", "100", "100", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"zadd", "zset1", "6", "bar", "100", "100", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(!expect.ok());

    sess.setArgs({"zrange", "zset1", "0", "-1", "101", "101", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    ss1.str("");
    Command::fmtMultiBulkLen(ss1, 1);
    Command::fmtBulk(ss1, "foo");
    EXPECT_EQ(ss1.str(), expect.value());

    sess.setArgs({"zadd", "zset1", "7", "baz", "101", "101", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"zrange", "zset1", "0", "-1", "102", "102", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    ss1.str("");
    Command::fmtMultiBulkLen(ss1, 2);
    Command::fmtBulk(ss1, "foo");
    Command::fmtBulk(ss1, "baz");
    EXPECT_EQ(ss1.str(), expect.value());

    sess.setArgs({"zrem", "zset1", "baz", "100", "100", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(!expect.ok());

    sess.setArgs({"zrem", "zset1", "foo", "102", "102", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"zrange", "zset1", "0", "-1", "103", "103", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    ss1.str("");
    Command::fmtMultiBulkLen(ss1, 1);
    Command::fmtBulk(ss1, "baz");
    EXPECT_EQ(ss1.str(), expect.value());
  }

  {
    sess.setArgs({"rpush", "list1", "a", "b", "c", "100", "100", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"rpop", "list1", "99", "99", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(!expect.ok());

    sess.setArgs({"lpop", "list1", "101", "101", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"lrange", "list1", "0", "-1", "102", "102", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    ss1.str("");
    Command::fmtMultiBulkLen(ss1, 2);
    Command::fmtBulk(ss1, "b");
    Command::fmtBulk(ss1, "c");
    EXPECT_EQ(ss1.str(), expect.value());

    sess.setArgs({"rpush", "list1", "z", "100", "100", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(!expect.ok());

    sess.setArgs({"lpush", "list1", "d", "102", "102", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"lrange", "list1", "0", "-1", "103", "103", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    ss1.str("");
    Command::fmtMultiBulkLen(ss1, 3);
    Command::fmtBulk(ss1, "d");
    Command::fmtBulk(ss1, "b");
    Command::fmtBulk(ss1, "c");
    EXPECT_EQ(ss1.str(), expect.value());

    sess.setArgs({"lpush", "list1", "c", "104", "104", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"rpush", "list1", "d", "105", "105", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"linsert", "list1", "after", "c", "f", "106", "106", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"linsert", "list1", "before", "d", "e", "107", "107", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"lrange", "list1", "0", "-1", "108", "108", "v1"});
    s = sess.processExtendProtocol();
    EXPECT_TRUE(s.ok());
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    ss1.str("");
    Command::fmtMultiBulkLen(ss1, 7);
    Command::fmtBulk(ss1, "c");
    Command::fmtBulk(ss1, "f");
    Command::fmtBulk(ss1, "e");
    Command::fmtBulk(ss1, "d");
    Command::fmtBulk(ss1, "b");
    Command::fmtBulk(ss1, "c");
    Command::fmtBulk(ss1, "d");
    EXPECT_EQ(ss1.str(), expect.value());
  }
}

void testLockMulti(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  for (int i = 0; i < 10; i++) {
    std::vector<std::string> vec;
    std::vector<int> index;

    LOG(INFO) << "testLockMulti " << i;

    for (int j = 0; j < 100; j++) {
      // different string
      vec.emplace_back(randomStr(20, true) + std::to_string(j));
      index.emplace_back(j);
    }

    for (int j = 0; j < 100; j++) {
      auto rng = std::default_random_engine{};
      std::shuffle(vec.begin(), vec.end(), rng);

      auto locklist = svr->getSegmentMgr()->getAllKeysLocked(
        &sess, vec, index, mgl::LockMode::LOCK_X);
      EXPECT_TRUE(locklist.ok());

      uint32_t id = 0;
      uint32_t chunkid = 0;
      std::string key = "";
      auto list = std::move(locklist.value());
      for (auto& l : list) {
        if (l->getStoreId() == id) {
          EXPECT_TRUE(l->getChunkId() >= chunkid);
          if (l->getChunkId() == chunkid) {
            EXPECT_TRUE(l->getKey() > key);
          }
        }

        EXPECT_TRUE(l->getStoreId() >= id);

        key = l->getKey();
        id = l->getStoreId();
        chunkid = l->getChunkId();
      }
    }
  }
}

void testCheckKeyType(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  sess.setArgs({"sadd", "ss", "a"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"set", "ss", "b"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"set", "ss1", "b"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
}

void testScan(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  sess.setArgs({"sadd",
                "scanset",
                "a",
                "b",
                "c",
                "d",
                "e",
                "f",
                "g",
                "h",
                "i",
                "j",
                "k",
                "l",
                "m",
                "n",
                "o"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"sscan", "scanset", "0"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  std::stringstream ss;
  Command::fmtMultiBulkLen(ss, 2);
  std::string cursor = getBulkValue(expect.value(), 0);
  EXPECT_TRUE(novadbplus::stoull(cursor).ok());  // cursor must be an integer
  Command::fmtBulk(ss, cursor);
  Command::fmtMultiBulkLen(ss, 10);
  for (int i = 0; i < 10; ++i) {
    std::string tmp;
    tmp.push_back('a' + i);
    Command::fmtBulk(ss, tmp);
  }
  EXPECT_EQ(ss.str(), expect.value());

  sess.setArgs({"sscan", "scanset", cursor});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok()) << expect.status().toString();
  ss.str("");
  Command::fmtMultiBulkLen(ss, 2);
  cursor = "0";
  Command::fmtBulk(ss, cursor);
  Command::fmtMultiBulkLen(ss, 5);
  for (int i = 0; i < 5; ++i) {
    std::string tmp;
    tmp.push_back('a' + 10 + i);
    Command::fmtBulk(ss, tmp);
  }
  EXPECT_EQ(ss.str(), expect.value());

  // case 2: hscan
  sess.setArgs({"hmset",
                "scanhash",
                "a",
                "b",
                "c",
                "d",
                "e",
                "f",
                "g",
                "h",
                "i",
                "j",
                "k",
                "l",
                "m",
                "n"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  ss.str("");

  // case 2.1: hscan from cursor 0
  uint32_t count = 5;
  uint32_t field_count = 7;
  sess.setArgs({"hscan", "scanhash", "0", "count", std::to_string(count)});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  Command::fmtMultiBulkLen(ss, 2);
  cursor = getBulkValue(expect.value(), 0);
  EXPECT_TRUE(novadbplus::stoull(cursor).ok());
  EXPECT_EQ(std::to_string(count + 1), cursor);
  Command::fmtBulk(ss, cursor);
  Command::fmtMultiBulkLen(ss, 2 * count);
  for (size_t i = 0; i < 2 * count; ++i) {
    std::string tmp;
    tmp.push_back('a' + i);
    Command::fmtBulk(ss, tmp);
  }
  EXPECT_EQ(ss.str(), expect.value());

  // case 2.2: hscan from a invalid cursor
  {
    // "1" is invalid cursor, scan from "0"
    sess.setArgs({"hscan", "scanhash", "1", "count", std::to_string(count)});
    auto expect1 = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect1.ok());
    // The result is same with scan with "0"
    EXPECT_EQ(expect.value(), expect1.value());
  }

  // case 2.3: hscan from last valid cursor
  sess.setArgs({"hscan", "scanhash", cursor, "count", std::to_string(count)});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok()) << expect.status().toString();
  ss.str("");
  Command::fmtMultiBulkLen(ss, 2);
  cursor = "0";
  Command::fmtBulk(ss, cursor);
  Command::fmtMultiBulkLen(ss, (field_count - count) * 2);
  for (size_t i = 0; i < (field_count - count) * 2; ++i) {
    std::string tmp;
    tmp.push_back('a' + 2 * count + i);
    Command::fmtBulk(ss, tmp);
  }
  EXPECT_EQ(ss.str(), expect.value());

  // case 2.4: hscan a string cursor
  sess.setArgs({"hscan", "scanhash", "abcde", "count", "5"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_FALSE(expect.ok());
}

void testMulti(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioCtx;
  asio::ip::tcp::socket socket(ioCtx), socket1(ioCtx);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  sess.setArgs({"config", "set", "session", "novadb_protocol_extend", "1"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"hset", "multitest", "initkey", "initval", "1", "1", "v1"});
  auto s = sess.processExtendProtocol();
  EXPECT_TRUE(s.ok());
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  // Command with version equal to key is not allowed to perform.
  sess.setArgs({"hset", "multitest", "dupver", "dupver", "1", "1", "v1"});
  s = sess.processExtendProtocol();
  EXPECT_TRUE(s.ok());
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(!expect.ok());

  sess.setArgs({"multi", "2", "2", "v1"});
  s = sess.processExtendProtocol();
  EXPECT_TRUE(s.ok());
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  // Out multi/exec doesn't behaviour like what redis does.
  // each command between multi and exec will be executed immediately.
  sess.setArgs({"hset", "multitest", "multi1", "multi1", "2", "2", "v1"});
  s = sess.processExtendProtocol();
  EXPECT_TRUE(s.ok());
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"hset", "multitest", "multi2", "multi2", "2", "2", "v1"});
  s = sess.processExtendProtocol();
  EXPECT_TRUE(s.ok());
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"hset", "multitest", "multi3", "multi3", "2", "2", "v1"});
  s = sess.processExtendProtocol();
  EXPECT_TRUE(s.ok());
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  // Exec will just return ok, no array reply.
  sess.setArgs({"exec", "2", "2", "v1"});
  s = sess.processExtendProtocol();
  EXPECT_TRUE(s.ok());
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"multi", "3", "3", "v1"});
  s = sess.processExtendProtocol();
  EXPECT_TRUE(s.ok());
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"hset", "multitest", "multi4", "multi4", "3", "3", "v1"});
  s = sess.processExtendProtocol();
  EXPECT_TRUE(s.ok());
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  // version check: exec with version not same as txn will fail.
  sess.setArgs({"exec", "4", "4", "v1"});
  s = sess.processExtendProtocol();
  EXPECT_TRUE(s.ok());
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(!expect.ok());
}

void testMaxClients(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);
  uint32_t i = 30;
  sess.setArgs({"config", "get", "maxclients"});
  auto expect = Command::runSessionCmd(&sess);
  std::stringstream ss;
  Command::fmtMultiBulkLen(ss, 2);
  Command::fmtBulk(ss, "maxclients");
  Command::fmtBulk(ss, "10000");
  EXPECT_EQ(ss.str(), expect.value());
  ss.clear();
  ss.str("");

  sess.setArgs({"config", "set", "maxclients", std::to_string(i)});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"config", "get", "maxclients"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  Command::fmtMultiBulkLen(ss, 2);
  Command::fmtBulk(ss, "maxclients");
  Command::fmtBulk(ss, std::to_string(i));
  EXPECT_EQ(ss.str(), expect.value());
  ss.clear();
  ss.str("");

  sess.setArgs({"config", "set", "masterauth", "testauth"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  sess.setArgs({"config", "get", "masterauth"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  Command::fmtMultiBulkLen(ss, 2);
  Command::fmtBulk(ss, "masterauth");
  Command::fmtBulk(ss, "testauth");
  EXPECT_EQ(ss.str(), expect.value());
}

void testSlowLog(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  uint32_t i = 0;
  sess.setArgs({"config", "set", "slowlog-log-slower-than", std::to_string(i)});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"sadd", "ss", "a"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"set", "ss", "b"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"set", "ss1", "b"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"config", "get", "slowlog-log-slower-than"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"config", "set", "slowlog-file-enabled", "0"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"set", "ss2", "a"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"config", "set", "slowlog-file-enabled", "1"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"set", "ss2", "b"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
}

void testGlobStylePattern(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  sess.setArgs({"config", "set", "slowlog-log-slower-than", "100000"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"config", "set", "slowlog-max-len", "1024"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"config", "get", "*slow*"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(
    "*10\r\n$7\r\nslowlog\r\n$11\r\n\"./"
    "slowlog\"\r\n$20\r\nslowlog-file-enabled\r\n$3\r\nyes\r\n$"
    "22\r\nslowlog-"
    "flush-interval\r\n$22\r\n not supported anymore\r\n$23\r\n"
    "slowlog-log-slower-than\r\n$"
    "6\r\n100000\r\n$15\r\nslowlog-max-len\r\n$4\r\n1024\r\n",
    expect.value());

  sess.setArgs({"config", "get", "?lowlog"});
  expect = Command::runSessionCmd(&sess);
  std::stringstream ss;
  Command::fmtMultiBulkLen(ss, 2);
  Command::fmtBulk(ss, "slowlog");
  Command::fmtBulk(ss, "\"./slowlog\"");
  EXPECT_EQ(ss.str(), expect.value());

  sess.setArgs({"config", "get", "no_exist_key"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(Command::fmtZeroBulkLen(), expect.value());

  sess.setArgs({"config", "get", "a", "b"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(!expect.ok());
}

void testConfigRewrite(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  sess.setArgs({"config", "set", "maxbinlogkeepnum", "1500000"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"config", "rewrite"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  auto confile = svr->getParams()->getConfFile();
  std::ifstream file(confile);
  if (!file.is_open()) {
    EXPECT_TRUE(0);
  }
  std::string line;
  std::string text;
  std::vector<std::string> tokens;

  bool find = false;
  try {
    line.clear();
    while (std::getline(file, line)) {
      line = trim(line);
      if (line.size() == 0 || line[0] == '#') {
        continue;
      }
      std::stringstream ss(line);
      tokens.clear();
      std::string tmp;
      while (std::getline(ss, tmp, ' ')) {
        tokens.emplace_back(tmp);
      }
      if (tokens.size() == 2) {
        if (tokens[0] == "maxbinlogkeepnum" && tokens[1] == "1500000") {
          find = true;
        }
      }
    }
  } catch (const std::exception& ex) {
    EXPECT_TRUE(0);
    return;
  }
  EXPECT_TRUE(find);
  file.close();

  std::ofstream out;
  out.open(confile);
  out.flush();
  out << text;
  out.close();

  sess.setArgs({"config", "rewrite"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  bool correct = false;
  std::ifstream check(confile);
  if (!check.is_open()) {
    EXPECT_TRUE(0);
  }
  try {
    line.clear();
    while (std::getline(check, line)) {
      line = trim(line);
      if (line.size() == 0 || line[0] == '#') {
        continue;
      }
      std::stringstream ss(line);
      tokens.clear();
      std::string tmp;
      while (std::getline(ss, tmp, ' ')) {
        tokens.emplace_back(tmp);
      }
      if (tokens.size() == 2) {
        if (tokens[0] == "maxbinlogkeepnum" && tokens[1] == "1500000") {
          correct = true;
          break;
        }
      }
    }
  } catch (const std::exception& ex) {
    EXPECT_TRUE(0);
    return;
  }
  EXPECT_TRUE(correct);
  check.close();
}

void testCommand(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  sess.setArgs({"command", "getkeys", "set", "a", "b"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(
    "*1\r\n"
    "$1\r\na\r\n",
    expect.value());

  sess.setArgs({"COMMAND", "GETKEYS", "SET", "a", "b"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(
    "*1\r\n"
    "$1\r\na\r\n",
    expect.value());
}

void testObject(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  sess.setArgs({"set", "a", "b"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"object", "encoding", "a"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"OBJECT", "ENCODING", "a"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
}

class CommandCommonTest : public ::testing::Test,
                          public ::testing::WithParamInterface<bool> {
 public:
  CommandCommonTest() : binlogEnabled_(true) {}
  void SetUp() override {
    binlogEnabled_ = GetParam();
  }
  bool binlogEnabled_;
};

TEST_P(CommandCommonTest, BinlogEnabled) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);
  cfg->binlogEnabled = binlogEnabled_;

  testPf(server);
  testList(server);
  testKV(server);

  // testSetRetry only works in TXN_OPT mode
  // testSetRetry(server);
  testType(server);
  testHash1(server);
  testHash2(server);
  testSet(server);
  // zadd/zrem/zrank/zscore
  testZset(server);
  // zcount
  testZset2(server);
  // zlexcount, zrange, zrangebylex, zrangebyscore
  testZset3(server);
  // zremrangebyrank, zremrangebylex, zremrangebyscore
  testZset4(server);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

INSTANTIATE_TEST_CASE_P(BinlogEnabled, CommandCommonTest, testing::Bool());

TEST(Command, common_scan) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  testScan(server);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

TEST(Command, novadbex) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  // need 420000
  // cfg->chunkSize = 420000;
  auto server = makeServerEntry(cfg);

  testExtendProtocol(server);
  testSync(server);
  testMulti(server);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

TEST(Command, checkKeyTypeForSetKV) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  cfg->checkKeyTypeForSet = true;
  auto server = makeServerEntry(cfg);

  testCheckKeyType(server);
  testMset(server);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

TEST(Command, lockMulti) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  testHash2(server);
  testLockMulti(server);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

TEST(Command, maxClients) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  testMaxClients(server);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

#ifndef _WIN32
TEST(Command, slowlog) {
  const auto guard = MakeGuard([] { destroyEnv(); });
  char line[100];
  FILE* fp;
  std::string clear =
    "echo "
    " > ./slowlogtest";
  const char* clearCommand = clear.data();
  if ((fp = popen(clearCommand, "r")) == NULL) {
    std::cout << "error" << std::endl;
    return;
  }

  {
    EXPECT_TRUE(setupEnv());
    auto cfg = makeServerParam();
    cfg->slowlogPath = "slowlogtest";
    auto server = makeServerEntry(cfg);

    testSlowLog(server);

#ifndef _WIN32
    server->stop();
    EXPECT_EQ(server.use_count(), 1);
#endif
  }

  std::string cmd = "grep -Ev '^$|[#;]' ./slowlogtest";
  const char* sysCommand = cmd.data();
  if ((fp = popen(sysCommand, "r")) == NULL) {
    std::cout << "error" << std::endl;
    return;
  }

  char* ptr;
  ptr = fgets(line, sizeof(line) - 1, fp);
  EXPECT_STRCASEEQ(line, "[] config set slowlog-log-slower-than 0 \n");
  ptr = fgets(line, sizeof(line) - 1, fp);
  EXPECT_STRCASEEQ(line, "[] sadd ss a \n");
  ptr = fgets(line, sizeof(line) - 1, fp);
  EXPECT_STRCASEEQ(line, "[] set ss b \n");
  ptr = fgets(line, sizeof(line) - 1, fp);
  EXPECT_STRCASEEQ(line, "[] set ss1 b \n");
  ptr = fgets(line, sizeof(line) - 1, fp);
  EXPECT_STRCASEEQ(line, "[] config get slowlog-log-slower-than \n");
  ptr = fgets(line, sizeof(line) - 1, fp);
  EXPECT_STRCASEEQ(line, "[] config set slowlog-file-enabled 1 \n");
  ptr = fgets(line, sizeof(line) - 1, fp);
  EXPECT_STRCASEEQ(line, "[] set ss2 b \n");
  pclose(fp);
}
#endif  // !

TEST(Command, testGlobStylePattern) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  testGlobStylePattern(server);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

TEST(Command, testConfigRewrite) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());

  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  testConfigRewrite(server);

  remove(cfg->getConfFile().c_str());

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

TEST(Command, testCommand) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());

  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  testCommand(server);

  remove(cfg->getConfFile().c_str());

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

TEST(Command, testObject) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());

  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  testObject(server);

  remove(cfg->getConfFile().c_str());

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

void testRenameCommand(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  sess.setArgs({"set"});
  auto eprecheck = Command::precheck(&sess);
  EXPECT_EQ(Command::fmtErr("unknown command 'set'"),
            eprecheck.status().toString());

  sess.setArgs({"set_rename", "a", "1"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(Command::fmtOK(), expect.value());

  sess.setArgs({"dbsize"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(Command::fmtLongLong(0), expect.value());

  sess.setArgs({"keys"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  std::stringstream ss;
  Command::fmtMultiBulkLen(ss, 0);
  EXPECT_EQ(ss.str(), expect.value());
}

void testnovadbadminSleep(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext, ioContext2;
  asio::ip::tcp::socket socket(ioContext), socket2(ioContext2);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);
  NetSession sess2(svr, std::move(socket2), 1, false, nullptr, nullptr);

  int i = 4;
  std::thread thd1([&sess2, &i]() {
    uint32_t now = msSinceEpoch();
    sess2.setArgs({"novadbadmin", "sleep", std::to_string(i)});
    auto expect = Command::runSessionCmd(&sess2);
    auto val = expect.value();
    EXPECT_TRUE(expect.ok());
    uint32_t end = msSinceEpoch();
    EXPECT_TRUE(end - now > (unsigned)(i - 1) * 1000);
  });

  std::this_thread::sleep_for(std::chrono::seconds(1));

  std::thread thd2([&svr, &i]() {
    uint32_t now = msSinceEpoch();
    runCommand(svr, {"ping"});
    runCommand(svr, {"info"});
    runCommand(svr, {"info", "replication"});
    runCommand(svr, {"info", "all"});
    uint32_t end = msSinceEpoch();

    EXPECT_LT(end - now, 500);
    LOG(INFO) << "info used " << end - now
              << "ms when running novadbadmin sleep ";
  });

  // it must be a slowlog
  sess.setArgs({"set", "a", "b"});
  uint32_t now = msSinceEpoch();
  auto expect = Command::runSessionCmd(&sess);

  EXPECT_TRUE(expect.ok());
  uint32_t end = msSinceEpoch();
  EXPECT_TRUE(end - now > (unsigned)(i - 2) * 1000);

  thd1.join();
  thd2.join();

  // check the slowlog
  auto slowlist = svr->getSlowlogStat().getSlowlogData(1);
  EXPECT_EQ(slowlist.size(), 1);
  auto s = slowlist.begin();
  EXPECT_TRUE(s->duration > (unsigned)(i - 2) * 1000 * 1000);
  EXPECT_TRUE(s->duration < (unsigned)100 * 1000 * 1000);
  EXPECT_EQ(s->argv[0], "set");
  EXPECT_EQ(s->argv[1], "a");
  EXPECT_EQ(s->argv[2], "b");
  // waiting for key lock
  EXPECT_TRUE(s->execTime > (unsigned)(i - 2) * 1000 * 1000);
}

void testDbEmptyCommand(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext, ioContext2;
  asio::ip::tcp::socket socket(ioContext), socket2(ioContext2);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);
  NetSession sess2(svr, std::move(socket2), 1, false, nullptr, nullptr);

  sess.setArgs({"set", "key", "value"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess2.setArgs({"dbempty"});
  expect = Command::runSessionCmd(&sess2);
  std::stringstream ss;
  Command::fmtMultiBulkLen(ss, 0);
  EXPECT_EQ(ss.str(), expect.value());

  sess.setArgs({"del", "key"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess2.setArgs({"dbempty"});
  expect = Command::runSessionCmd(&sess2);
  std::stringstream ss2;
  Command::fmtMultiBulkLen(ss2, 1);
  EXPECT_EQ(ss2.str(), expect.value());
}

void testCommandCommand(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  sess.setArgs({"command"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  auto& cmdmap = commandMap();
  sess.setArgs({"command", "count"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(Command::fmtLongLong(cmdmap.size()), expect.value());

  sess.setArgs({"command", "set"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(Command::fmtLongLong(cmdmap.size()), expect.value());

  sess.setArgs({"keys"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  std::stringstream ss;
  Command::fmtMultiBulkLen(ss, 0);
  EXPECT_EQ(ss.str(), expect.value());
}

TEST(Command, novadbadminCommand) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  testnovadbadminSleep(server);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

TEST(Command, TestSlowLogQueueTime) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  cfg->executorWorkPoolSize = 1;
  cfg->executorThreadNum = 1;
  // only one worker thread, make another session waiting in the queue
  auto server = makeServerEntry(cfg);

  uint64_t time = 10;
  std::thread thd1([&server, &time]() {
    std::string t = std::to_string(time);
    std::string cmd =
      "*3\r\n$11\r\nnovadbadmin\r\n$5\r\nsleep\r\n$2\r\n" + t + "\r\n";
    uint32_t now = msSinceEpoch();
    runCommandFromNetwork(server, cmd);
    uint32_t end = msSinceEpoch();
    EXPECT_TRUE(end - now > (unsigned)(time - 1) * 1000);
  });

  std::this_thread::sleep_for(std::chrono::seconds(1));
  std::string cmd = "*3\r\n$3\r\nset\r\n$1\r\na\r\n$1\r\nb\r\n";
  runCommandFromNetwork(server, cmd);
  thd1.join();

  // check the slowlog
  auto slowlist = server->getSlowlogStat().getSlowlogData(1);
  EXPECT_EQ(slowlist.size(), 1);
  auto s = slowlist.begin();
  EXPECT_TRUE(s->duration > (unsigned)(time - 3) * 1000 * 1000);
  EXPECT_TRUE(s->duration < (unsigned)100 * 1000 * 1000);
  EXPECT_EQ(s->argv[0], "set");
  EXPECT_EQ(s->argv[1], "a");
  EXPECT_EQ(s->argv[2], "b");
  // waiting in queue
  EXPECT_TRUE(s->execTime < 1000 * 1000);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

void testDelTTLIndex(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext, ioContext2;
  asio::ip::tcp::socket socket(ioContext), socket2(ioContext2);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  sess.setArgs({"zadd", "zset1", "10", "a"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"expire", "zset1", "3"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(Command::fmtLongLong(1), expect.value());

  sess.setArgs({"zrem", "zset1", "a"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(Command::fmtLongLong(1), expect.value());

  {
    sess.setArgs({"sadd", "set2", "three"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"expire", "set2", "3"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_EQ(Command::fmtLongLong(1), expect.value());

    sess.setArgs({"srem", "set2", "three"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_EQ(Command::fmtLongLong(1), expect.value());
  }

  {
    sess.setArgs({"sadd", "set1", "one"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"expire", "set1", "3"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_EQ(Command::fmtLongLong(1), expect.value());

    sess.setArgs({"spop", "set1"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
  }

  {
    // srem not exist key
    sess.setArgs({"srem", "setxxx", "three"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_EQ(Command::fmtZero(), expect.value());

    // srem expire key
    sess.setArgs({"sadd", "setxxx1", "one"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"expire", "setxxx1", "1"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_EQ(Command::fmtLongLong(1), expect.value());

    std::this_thread::sleep_for(std::chrono::seconds(2));

    sess.setArgs({"srem", "setxxx1", "one"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_EQ(Command::fmtZero(), expect.value());
  }

  {
    sess.setArgs({"rpush", "list1", "one"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"expire", "list1", "3"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_EQ(Command::fmtLongLong(1), expect.value());

    sess.setArgs({"lrem", "list1", "0", "one"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_EQ(Command::fmtLongLong(1), expect.value());
  }

  {
    sess.setArgs({"rpush", "list2", "one"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"expire", "list2", "3"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_EQ(Command::fmtLongLong(1), expect.value());

    sess.setArgs({"ltrim", "list2", "1", "-1"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
  }

  {
    sess.setArgs({"rpush", "list3", "one"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"expire", "list3", "2"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_EQ(Command::fmtLongLong(1), expect.value());

    sess.setArgs({"lpop", "list3"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
  }

  {
    sess.setArgs({"hset", "hash1", "hh", "one"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"expire", "hash1", "2"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_EQ(Command::fmtLongLong(1), expect.value());

    sess.setArgs({"hdel", "hash1", "hh"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_EQ(Command::fmtLongLong(1), expect.value());
  }

  std::this_thread::sleep_for(std::chrono::seconds(3));

  sess.setArgs({"dbsize"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(Command::fmtLongLong(0), expect.value());

  {
    sess.setArgs({"zadd", "zset1", "10", "a"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"sadd", "set2", "three"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"sadd", "set1", "one"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"rpush", "list1", "one"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"rpush", "list2", "one"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"rpush", "list3", "one"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"hset", "hash1", "hh", "one"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
  }

  std::this_thread::sleep_for(std::chrono::seconds(3));

  sess.setArgs({"dbsize"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(Command::fmtLongLong(7), expect.value());
}

void testRenameCommandTTL(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext, ioContext2;
  asio::ip::tcp::socket socket(ioContext), socket2(ioContext2);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  sess.setArgs({"zadd", "ss", "10", "a"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"expire", "ss", "3"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(Command::fmtLongLong(1), expect.value());

  sess.setArgs({"rename", "ss", "sa"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(Command::fmtOK(), expect.value());

  sess.setArgs({"dbsize"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(Command::fmtLongLong(1), expect.value());

  std::this_thread::sleep_for(std::chrono::seconds(4));

  sess.setArgs({"dbsize"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(Command::fmtLongLong(0), expect.value());

  sess.setArgs({"zadd", "ss", "3", "a"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  std::this_thread::sleep_for(std::chrono::seconds(3));

  sess.setArgs({"dbsize"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(Command::fmtLongLong(1), expect.value());
}

TEST(Command, DelTTLIndex) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  testDelTTLIndex(server);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

TEST(Command, RenameCommandTTL) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  testRenameCommandTTL(server);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

void testRenameCommandDelete(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext, ioContext2;
  asio::ip::tcp::socket socket(ioContext), socket2(ioContext2);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  sess.setArgs({"zadd", "ss{a}", "10", "a"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"zadd", "zz{a}", "101", "ab"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"rename", "ss{a}", "ss"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"zcount", "zz{a}", "0", "1000"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(Command::fmtLongLong(1), expect.value());
}

TEST(Command, RenameCommandDelete) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  testRenameCommandDelete(server);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

/*
TEST(Command, keys) {
    const auto guard = MakeGuard([] {
       destroyEnv();
    });

    EXPECT_TRUE(setupEnv());
    auto cfg = makeServerParam();
    auto server = makeServerEntry(cfg);

    asio::io_context ioContext;
    asio::ip::tcp::socket socket(ioContext);
    NetSession sess(server, std::move(socket), 1, false, nullptr, nullptr);

    sess.setArgs({"set", "a", "a"});
    auto expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    EXPECT_EQ(expect.value(), Command::fmtOK());
    sess.setArgs({"set", "b", "b"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    EXPECT_EQ(expect.value(), Command::fmtOK());
    sess.setArgs({"set", "c", "c"});
    expect = Command::runSessionCmd(&sess);

    sess.setArgs({"keys", "*"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    std::stringstream ss;
    Command::fmtMultiBulkLen(ss, 3);
    Command::fmtBulk(ss, "a");
    Command::fmtBulk(ss, "b");
    Command::fmtBulk(ss, "c");
    EXPECT_EQ(expect.value(), ss.str());

    sess.setArgs({"keys", "a*"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    ss.str("");
    Command::fmtMultiBulkLen(ss, 1);
    Command::fmtBulk(ss, "a");
    EXPECT_EQ(expect.value(), ss.str());
}
*/

void testCommandArray(std::shared_ptr<ServerEntry> svr,
                      const std::vector<std::vector<std::string>>& arr,
                      bool isError) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  for (auto& args : arr) {
    sess.setArgs(args);

    // need precheck for args check, after exp.ok(), can execute runSessionCmd()
    // EXPECT_FALSE when !exp.ok()
    auto exp = Command::precheck(&sess);
    if (!exp.ok()) {
      std::stringstream ss;
      for (auto& str : args) {
        ss << str << " ";
      }
      LOG(INFO) << ss.str() << "ERROR:" << exp.status().toString();
      EXPECT_FALSE(exp.ok());
      continue;
    }

    auto expect = Command::runSessionCmd(&sess);
    if (!expect.ok()) {
      std::stringstream ss;
      for (auto& str : args) {
        ss << str << " ";
      }
      LOG(INFO) << ss.str() << "ERROR:" << expect.status().toString();
    }

    if (isError) {
      EXPECT_FALSE(expect.ok());
    } else {
      EXPECT_TRUE(expect.ok());
    }
  }
}

void testCommandArrayResult(
  std::shared_ptr<ServerEntry> svr,
  const std::vector<std::pair<std::vector<std::string>, std::string>>& arr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  for (auto& p : arr) {
    sess.setArgs(p.first);
    auto expect = Command::runSessionCmd(&sess);
    if (expect.ok()) {
      auto ret = expect.value();
      EXPECT_EQ(p.second, ret);
    } else {
      auto ret = expect.status().toString();
      EXPECT_EQ(p.second, ret);
    }
  }
}

void testDiffCommandArray(
  std::shared_ptr<ServerEntry> svr,
  const std::vector<
    std::pair<std::vector<std::string>, std::vector<std::string>>>& arr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  for (auto& p : arr) {
    LOG(INFO) << p.first[0];
    sess.setArgs(p.first);
    auto expect = Command::runSessionCmd(&sess);

    sess.setArgs(p.second);
    auto expect1 = Command::runSessionCmd(&sess);

    if (expect.ok()) {
      INVARIANT_D(expect1.ok());
      EXPECT_TRUE(expect1.ok());
      INVARIANT_D(expect.value() == expect1.value());
      ASSERT_EQ(expect.value(), expect1.value());
    } else {
      INVARIANT_D(!expect1.ok());
      EXPECT_FALSE(expect1.ok());
      auto ret = expect.status().toString();
      auto ret2 = expect.status().toString();
      EXPECT_EQ(ret, ret2);
    }
  }
}

TEST(Command, syncversion) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  cfg->kvStoreCount = 5;
  auto server = makeServerEntry(cfg);

  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext);
  NetSession sess(server, std::move(socket), 1, false, nullptr, nullptr);

  sess.setArgs({"syncversion", "k", "?", "?", "v1"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(expect.value(), "*2\r\n:-1\r\n:-1\r\n");

  sess.setArgs({"syncversion", "k", "25000", "1", "v1"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(expect.value(), Command::fmtOK());

  sess.setArgs({"syncversion", "k", "?", "?", "v1"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(expect.value(), "*2\r\n:25000\r\n:1\r\n");

  sess.setArgs({"syncversion", "*", "?", "?", "v1"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(expect.value(),
            "*5\r\n*1\r\n*3\r\n$1\r\nk\r\n:25000\r\n:1\r\n"
            "*1\r\n*3\r\n$1\r\nk\r\n:25000\r\n:1\r\n"
            "*1\r\n*3\r\n$1\r\nk\r\n:25000\r\n:1\r\n"
            "*1\r\n*3\r\n$1\r\nk\r\n:25000\r\n:1\r\n"
            "*1\r\n*3\r\n$1\r\nk\r\n:25000\r\n:1\r\n");

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

TEST(Command, info) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  std::vector<std::vector<std::string>> correctArr = {
    {"info", "all"},
    {"info", "default"},
    {"info", "server"},
    {"info", "clients"},
    {"info", "memory"},
    {"info", "persistence"},
    {"info", "stats"},
    {"info", "replication"},
    {"info", "binloginfo"},
    {"info", "cpu"},
    {"info", "commandstats"},
    {"info", "cluster"},
    {"info", "keyspace"},
    {"info", "backup"},
    {"info", "dataset"},
    {"info", "compaction"},
    {"info", "levelstats"},
    {"info", "rocksdbstats"},
    {"info", "rocksdbperfstats"},
    {"info", "rocksdbbgerror"},
    {"info", "invalid"},  // it's ok
    {"rocksproperty", "rocksdb.base-level", "0"},
    {"rocksproperty", "all", "0"},
    {"rocksproperty", "rocksdb.base-level"},
    {"rocksproperty", "all"},
  };

  std::vector<std::pair<std::vector<std::string>, std::string>> okArr = {
    {{"config", "set", "session", "perf_level", "enable_count"},
     Command::fmtOK()},
    {{"config", "set", "session", "perf_level", "enable_time_expect_for_mutex"},
     Command::fmtOK()},
    {{"config",
      "set",
      "session",
      "perf_level",
      "enable_time_and_cputime_expect_for_mutex"},
     Command::fmtOK()},
    {{"config", "set", "session", "perf_level", "enable_time"},
     Command::fmtOK()},
    {{"config", "resetstat", "all"}, Command::fmtOK()},
    {{"config", "resetstat", "unseencommands"}, Command::fmtOK()},
    {{"config", "resetstat", "commandstats"}, Command::fmtOK()},
    {{"config", "resetstat", "stats"}, Command::fmtOK()},
    {{"config", "resetstat", "rocksdbstats"}, Command::fmtOK()},
    {{"config", "resetstat", "invalid"}, Command::fmtOK()},  // it's ok
    {{"novadbadmin", "sleep", "1"}, Command::fmtOK()},
    {{"novadbadmin", "recovery"}, Command::fmtOK()},
  };

  std::vector<std::vector<std::string>> wrongArr = {
    {"info", "all", "1"},
    {"rocksproperty", "rocks.base_level", "100"},
    {"rocksproperty", "all1", "0"},
    {"rocksproperty", "rocks.base_level1"},
    {"rocksproperty", "all1"},
    {"config", "set", "session", "perf_level", "invalid"},
    {"config", "set", "session", "invalid", "invalid"},
    {"config", "set", "session", "perf_level"},
    {"novadbadmin", "sleep"},
    {"novadbadmin", "sleep", "1", "2"},
    {"novadbadmin", "recovery", "1"},
    {"novadbadmin", "invalid"},
  };

  testCommandArray(server, correctArr, false);
  testCommandArrayResult(server, okArr);
  testCommandArray(server, wrongArr, true);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

TEST(Command, command) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  std::vector<std::vector<std::string>> correctArr = {
    {"command"},
    {"command", "info"},
    {"command", "info", "get"},
    {"command", "info", "get", "set"},
    {"command", "info", "get", "set", "wrongcommand"},
    {"command", "count"},
    {"command", "getkeys", "get", "a"},
    {"command", "getkeys", "set", "a", "b"},
    {"command", "getkeys", "mset", "a", "b", "c", "d"},
  };

  std::vector<std::vector<std::string>> wrongArr = {
    {"command", "invalid"},
    {"command", "count", "invalid"},
    {"command", "getkeys"},
    {"command", "getkeys", "get", "a", "c"},
  };

  std::vector<std::pair<std::vector<std::string>, std::string>> resultArr = {
    {{"command", "info", "get"},
     "*1\r\n*6\r\n$3\r\nget\r\n:2\r\n*2\r\n+readonly\r\n+fast\r\n:1\r\n:"
     "1\r\n:"
     "1\r\n"},
    {{"command", "getkeys", "get", "a"}, "*1\r\n$1\r\na\r\n"},
    {{"command", "getkeys", "mset", "a", "b", "c", "d"},
     "*2\r\n$1\r\na\r\n$1\r\nc\r\n"},
  };

  testCommandArray(server, correctArr, false);
  testCommandArray(server, wrongArr, true);
  testCommandArrayResult(server, resultArr);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

void testRevisionCommand(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  sess.setArgs({"set", "a", "b"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(expect.value(), Command::fmtOK());

  // 1893430861000 :: 2030/1/1 1:1:1
  sess.setArgs({"revision", "a", "100", "1893430861000"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(expect.value(), Command::fmtOK());

  sess.setArgs({"object", "revision", "a"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(expect.value(), ":100\r\n");

  sess.setArgs({"set", "key_1", "b"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(expect.value(), Command::fmtOK());

  // 1577811661000 :: 2010/1/1 1:1:1 key should be deleted
  sess.setArgs({"revision", "key_1", "110", "1577811661000"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(expect.value(), Command::fmtOK());

  sess.setArgs({"exists", "key_1"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(expect.value(), Command::fmtZero());
}

TEST(Command, revision) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  testRevisionCommand(server);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

// NOTE(tanninzhu) restorevalue command will return multi response,
// it's only for redis-sync, we fix sendbuffer structure, cant support it,
// so we dont use this test
// TEST(Command, restorevalue) {
//   const auto guard = MakeGuard([] { destroyEnv(); });

//   EXPECT_TRUE(setupEnv("restore1"));
//   auto port1 = 5438;
//   auto cfg1 = makeServerParam(port1, 0, "restore1");
//   auto server1 = makeServerEntry(cfg1);

//   EXPECT_TRUE(setupEnv("restore2"));
//   auto port2 = 5439;
//   auto cfg2 = makeServerParam(port2, 0, "restore2");
//   auto server2 = makeServerEntry(cfg2);

//   auto allkeys =
//     writeComplexDataWithTTLToServer(server1, 1000, 2500, "restorevalue_");

//   for (const auto& keyset : allkeys) {
//     for (const auto& key : keyset) {
//       asio::io_context ioContext;
//       asio::ip::tcp::socket socket(ioContext);
//       NoSchedNetSession sess(
//         server1, std::move(socket), 1, false, nullptr, nullptr);

//       sess.setArgs({"restorevalue", key});
//       auto expect = Command::runSessionCmd(&sess);
//       EXPECT_TRUE(expect.ok());
//       auto cmdvec = sess.getResponse();
//       cmdvec.emplace_back(expect.value());

//       asio::io_context ioContext2;
//       asio::ip::tcp::socket socket2(ioContext2);
//       NoSchedNetSession sess2(
//         server2, std::move(socket2), 1, false, nullptr, nullptr);

//       // skip the first and last cmd
//       for (uint32_t i = 1; i < cmdvec.size() - 1; i++) {
//         auto cmd = cmdvec[i];
//         sess2.setArgsFromAof(cmd);

//         auto expect = Command::runSessionCmd(&sess2);
//         EXPECT_TRUE(expect.ok());
//       }
//     }
//   }

//   // compare data
//   for (const auto& keyset : allkeys) {
//     for (const auto& key : keyset) {
//       asio::io_context ioContext;
//       asio::ip::tcp::socket socket(ioContext);
//       NoSchedNetSession sess(
//         server1, std::move(socket), 1, false, nullptr, nullptr);

//       auto keystr1 = key2Aof(&sess, key);
//       INVARIANT_D(keystr1.ok());

//       asio::io_context ioContext2;
//       asio::ip::tcp::socket socket2(ioContext2);
//       NoSchedNetSession sess2(
//         server2, std::move(socket2), 1, false, nullptr, nullptr);

//       auto keystr2 = key2Aof(&sess2, key);
//       INVARIANT_D(keystr2.ok());

//       EXPECT_EQ(keystr1.value(), keystr2.value());
//     }
//   }

// #ifndef _WIN32
//   server1->stop();
//   EXPECT_EQ(server1.use_count(), 1);

//   server2->stop();
//   EXPECT_EQ(server2.use_count(), 1);
// #endif
// }

TEST(Command, dexec) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  std::vector<std::pair<std::vector<std::string>, std::string>> resultArr = {
    {{"set", "a", "b"}, Command::fmtOK()},
    {{"dexec", "2", "get", "a"},
     "*3\r\n$7\r\ndreturn\r\n$1\r\n2\r\n$7\r\n$1\r\nb\r\n\r\n"},
    {{"dexec", "-1", "set", "a", "c"},
     "*3\r\n$7\r\ndreturn\r\n$2\r\n-1\r\n$5\r\n+OK\r\n\r\n"},
    {{"dexec", "-1", "cluster", "nodes"},
     "*3\r\n$7\r\ndreturn\r\n$2\r\n-1\r\n$56\r\n-ERR:18,msg:This instance "
     "has cluster support disabled\r\n\r\n"},
    {{"dexec", "1", "dexec", "2", "get", "a"},
     "*3\r\n$7\r\ndreturn\r\n$1\r\n1\r\n$37\r\n*3\r\n$7\r\ndreturn\r\n$"
     "1\r\n2\r\n$7\r\n$1\r\nc\r\n\r\n\r\n"},
  };

  testCommandArrayResult(server, resultArr);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

void testRocksOptionCommand(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  sess.setArgs({"CONFIG", "GET", "rocks.enable_blob_files"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ("*2\r\n$23\r\nrocks.enable_blob_files\r\n$1\r\n1\r\n",
            expect.value());

  sess.setArgs({"CONFIG", "GET", "rocks.binlogcf.enable_blob_files"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ("*2\r\n$32\r\nrocks.binlogcf.enable_blob_files\r\n$1\r\n1\r\n",
            expect.value());

  sess.setArgs({"CONFIG", "GET", "rocks.blob_garbage_collection_age_cutoff"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(
    "*2\r\n$40\r\nrocks.blob_garbage_collection_age_cutoff\r\n$4\r\n0.12\r\n",
    expect.value());

  sess.setArgs({"CONFIG", "GET", "rocks.blob_compression_type"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ("*2\r\n$27\r\nrocks.blob_compression_type\r\n$3\r\nlz4\r\n",
            expect.value());

  std::stringstream ss;

  sess.setArgs({"CONFIG", "GET", "rocks.max_background_jobs"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  Command::fmtMultiBulkLen(ss, 2);
  Command::fmtBulk(ss, "rocks.max_background_jobs");
  Command::fmtBulk(ss, "2");
  EXPECT_EQ(ss.str(), expect.value());

  sess.setArgs({"CONFIG", "SET", "rocks.max_background_jobs", "3"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  for (uint32_t i = 0; i < svr->getKVStoreCount(); i++) {
    auto exptDb = svr->getSegmentMgr()->getDb(&sess, 0, mgl::LockMode::LOCK_IS);
    EXPECT_TRUE(exptDb.ok());

    auto store = exptDb.value().store;
    EXPECT_EQ(store->getOption("rocks.max_background_jobs"), 3);
  }

  sess.setArgs({"CONFIG", "GET", "rocks.max_background_jobs"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  ss.str("");
  Command::fmtMultiBulkLen(ss, 2);
  Command::fmtBulk(ss, "rocks.max_background_jobs");
  Command::fmtBulk(ss, "3");
  EXPECT_EQ(ss.str(), expect.value());

  sess.setArgs({"CONFIG", "GET", "rocks.max_open_files"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  ss.str("");
  Command::fmtMultiBulkLen(ss, 2);
  Command::fmtBulk(ss, "rocks.max_open_files");
  Command::fmtBulk(ss, "-1");
  EXPECT_EQ(ss.str(), expect.value());

  sess.setArgs({"CONFIG", "SET", "rocks.max_open_files", "3000"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  for (uint32_t i = 0; i < svr->getKVStoreCount(); i++) {
    auto exptDb = svr->getSegmentMgr()->getDb(&sess, 0, mgl::LockMode::LOCK_IS);
    EXPECT_TRUE(exptDb.ok());

    auto store = exptDb.value().store;
    EXPECT_EQ(store->getOption("rocks.max_open_files"), 3000);
  }

  sess.setArgs({"CONFIG", "GET", "rocks.max_open_files"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  ss.str("");
  Command::fmtMultiBulkLen(ss, 2);
  Command::fmtBulk(ss, "rocks.max_open_files");
  Command::fmtBulk(ss, "3000");
  EXPECT_EQ(ss.str(), expect.value());

  sess.setArgs({"CONFIG", "SET", "rocks.max_open_files", "-1"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  for (uint32_t i = 0; i < svr->getKVStoreCount(); i++) {
    auto exptDb = svr->getSegmentMgr()->getDb(&sess, 0, mgl::LockMode::LOCK_IS);
    EXPECT_TRUE(exptDb.ok());

    auto store = exptDb.value().store;
    EXPECT_EQ(store->getOption("rocks.max_open_files"), -1);
  }

  sess.setArgs({"CONFIG", "GET", "rocks.max_open_files"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  ss.str("");
  Command::fmtMultiBulkLen(ss, 2);
  Command::fmtBulk(ss, "rocks.max_open_files");
  Command::fmtBulk(ss, "-1");
  EXPECT_EQ(ss.str(), expect.value());

  sess.setArgs({"CONFIG", "SET", "rocks.periodic_compaction_seconds", "3"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  for (uint32_t i = 0; i < svr->getKVStoreCount(); i++) {
    auto exptDb = svr->getSegmentMgr()->getDb(&sess, 0, mgl::LockMode::LOCK_IS);
    EXPECT_TRUE(exptDb.ok());

    auto store = exptDb.value().store;
    EXPECT_EQ(store->getOption("rocks.periodic_compaction_seconds"), 3);
  }

  sess.setArgs({"CONFIG", "GET", "rocks.periodic_compaction_seconds"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  ss.str("");
  Command::fmtMultiBulkLen(ss, 2);
  Command::fmtBulk(ss, "rocks.periodic_compaction_seconds");
  Command::fmtBulk(ss, "3");
  EXPECT_EQ(ss.str(), expect.value());

  // we will adjust these tests when we use rocksdb(version > 6.11)
  std::string err;
  sess.setArgs({"CONFIG", "SET", "rocks.compaction_deletes_window", "100"});
  expect = Command::runSessionCmd(&sess);
#if ROCKSDB_MAJOR > 6 || (ROCKSDB_MAJOR == 6 && ROCKSDB_MINOR > 11)
  EXPECT_TRUE(expect.ok());
#else
  EXPECT_FALSE(expect.ok());
  err = Command::fmtErr(
    "-ERR:3,msg:rocks.compaction_deletes_window can't be changed dynmaically "
    "in rocksdb(version < 6.11)\r\n");
  EXPECT_EQ(err, expect.status().toString());
#endif

  sess.setArgs({"CONFIG", "SET", "rocks.compaction_deletes_trigger", "50"});
  expect = Command::runSessionCmd(&sess);
#if ROCKSDB_MAJOR > 6 || (ROCKSDB_MAJOR == 6 && ROCKSDB_MINOR > 11)
  EXPECT_TRUE(expect.ok());
#else
  EXPECT_FALSE(expect.ok());
  err.clear();
  err = Command::fmtErr(
    "-ERR:3,msg:rocks.compaction_deletes_trigger can't be changed "
    "dynmaically "
    "in rocksdb(version < 6.11)\r\n");
  EXPECT_EQ(err, expect.status().toString());
#endif

  sess.setArgs({"CONFIG", "SET", "rocks.compaction_deletes_ratio", "0.5"});
  expect = Command::runSessionCmd(&sess);
#if ROCKSDB_MAJOR > 6 || (ROCKSDB_MAJOR == 6 && ROCKSDB_MINOR > 11)
  EXPECT_TRUE(expect.ok());
#else
  EXPECT_FALSE(expect.ok());
  err.clear();
  err = Command::fmtErr(
    "-ERR:3,msg:rocks.compaction_deletes_ratio can't be changed dynmaically "
    "in rocksdb(version < 6.11)\r\n");
  EXPECT_EQ(err, expect.status().toString());
#endif

#if ROCKSDB_MAJOR > 6 || (ROCKSDB_MAJOR == 6 && ROCKSDB_MINOR > 11)
  std::ostringstream tableProperties;
  tableProperties << "CompactOnDeletionCollector"
                  << " (Sliding window size = " << 100
                  << " Deletion trigger = " << 50 << " Deletion ratio = " << 0.5
                  << ')';
  for (uint32_t i = 0; i < svr->getKVStoreCount(); i++) {
    auto exptDb = svr->getSegmentMgr()->getDb(&sess, 0, mgl::LockMode::LOCK_IS);
    EXPECT_TRUE(exptDb.ok());

    auto store = exptDb.value().store;
    auto rocksStore = static_cast<novadbplus::RocksKVStore*>(store.get());
    auto tableFactory = rocksStore->getUnderlayerPesDB()
                          ->GetOptions()
                          .table_properties_collector_factories;
    for (auto factory : tableFactory) {
      if (std::string(factory->Name()) == "CompactOnDeletionCollector") {
        auto compactOnDel =
          static_cast<rocksdb::CompactOnDeletionCollectorFactory*>(
            factory.get());
        EXPECT_EQ(tableProperties.str(), compactOnDel->ToString());
        break;
      }
    }
  }
#endif

  sess.setArgs({"CONFIG", "SET", "rocks.abc", "-1"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_FALSE(expect.ok());
}

void testConfigSetAndGet(std::shared_ptr<ServerEntry> master) {
  auto ctx = std::make_shared<asio::io_context>();
  auto session = makeSession(master, ctx);
  session->setArgs({"set", "aaa", "2"});
  auto expect = Command::runSessionCmd(session.get());
  EXPECT_EQ(expect.ok(), true);

  session->setArgs({"config", "set", "slowlog-log-slower-than", "2000000"});
  expect = Command::runSessionCmd(session.get());
  EXPECT_EQ(expect.ok(), true);

  session->setArgs({"config", "get", "slowlog-log-slower-than"});
  expect = Command::runSessionCmd(session.get());
  EXPECT_EQ(expect.value(),
            "*2\r\n$23\r\nslowlog-log-slower-than\r\n$7\r\n2000000\r\n");

  session->setArgs({"config", "set", "not_exist_param", "2"});
  expect = Command::runSessionCmd(session.get());
  EXPECT_EQ(expect.ok(), false);

  session->setArgs({"config", "get", "not_exist_param"});
  expect = Command::runSessionCmd(session.get());
  EXPECT_EQ(expect.value(), "*0\r\n");

  session->setArgs({"config", "set", "rocks.max_open_files", "2"});
  expect = Command::runSessionCmd(session.get());
  EXPECT_EQ(expect.ok(), true);

  session->setArgs({"config", "get", "rocks.max_open_files"});
  expect = Command::runSessionCmd(session.get());
  EXPECT_EQ(expect.value(), "*2\r\n$20\r\nrocks.max_open_files\r\n$1\r\n2\r\n");

  session->setArgs({"config", "set", "rocks.not_exist_param", "2"});
  expect = Command::runSessionCmd(session.get());
  EXPECT_EQ(expect.ok(), false);

  session->setArgs({"config", "get", "rocks.not_exist_param"});
  expect = Command::runSessionCmd(session.get());
  EXPECT_EQ(expect.value(), "*0\r\n");

  session->setArgs({"config", "set", "rocks.binlogcf.enable_blob_files", "1"});
  expect = Command::runSessionCmd(session.get());
  EXPECT_EQ(expect.ok(), true);

  session->setArgs({"config", "get", "rocks.binlogcf.enable_blob_files"});
  expect = Command::runSessionCmd(session.get());
  EXPECT_EQ(expect.value(),
            "*2\r\n$32\r\nrocks.binlogcf.enable_blob_files\r\n$1\r\n1\r\n");

  session->setArgs(
    {"config", "set", "rocks.not_exist_cf.enable_blob_files", "1"});
  expect = Command::runSessionCmd(session.get());
  EXPECT_EQ(expect.ok(), false);

  session->setArgs({"config", "get", "rocks.not_exist_cf.enable_blob_files"});
  expect = Command::runSessionCmd(session.get());
  EXPECT_EQ(expect.value(), "*0\r\n");
}

void testResizeCommand(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  sess.setArgs({"CONFIG", "SET", "incrPushThreadnum", "8"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(svr->getParams()->incrPushThreadnum, 8);

  sess.setArgs({"CONFIG", "SET", "fullPushThreadnum", "8"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(svr->getParams()->fullPushThreadnum, 8);

  sess.setArgs({"CONFIG", "SET", "fullReceiveThreadnum", "8"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(svr->getParams()->fullReceiveThreadnum, 8);

  sess.setArgs({"CONFIG", "SET", "logRecycleThreadnum", "8"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(svr->getParams()->logRecycleThreadnum, 8);

  //     need _enable_cluster flag on.
  sess.setArgs({"CONFIG", "SET", "migrateSenderThreadnum", "8"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(svr->getParams()->migrateSenderThreadnum, 8);

  sess.setArgs({"CONFIG", "SET", "migrateReceiveThreadnum", "8"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(svr->getParams()->migrateReceiveThreadnum, 8);

  // index manager
  sess.setArgs({"CONFIG", "SET", "scanJobCntIndexMgr", "8"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(svr->getParams()->scanJobCntIndexMgr, 8);

  sess.setArgs({"CONFIG", "SET", "delJobCntIndexMgr", "8"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(svr->getParams()->delJobCntIndexMgr, 8);

  // total sleep 10s to wait thread resize ok
  sleep(10);
  EXPECT_EQ(svr->getReplManager()->incrPusherSize(), 8);
  EXPECT_EQ(svr->getReplManager()->fullPusherSize(), 8);
  EXPECT_EQ(svr->getReplManager()->fullReceiverSize(), 8);
  EXPECT_EQ(svr->getReplManager()->logRecycleSize(), 8);
  EXPECT_EQ(svr->getMigrateManager()->migrateSenderSize(), 8);
  EXPECT_EQ(svr->getMigrateManager()->migrateReceiverSize(), 8);
  EXPECT_EQ(svr->getIndexMgr()->indexScannerSize(), 8);
  EXPECT_EQ(svr->getIndexMgr()->keyDeleterSize(), 8);

  sess.setArgs({"CONFIG", "SET", "fullPushThreadnum", "1"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(svr->getParams()->fullPushThreadnum, 1);

  sess.setArgs({"CONFIG", "SET", "fullReceiveThreadnum", "1"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(svr->getParams()->fullReceiveThreadnum, 1);

  sess.setArgs({"CONFIG", "SET", "logRecycleThreadnum", "1"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(svr->getParams()->logRecycleThreadnum, 1);

  sess.setArgs({"CONFIG", "SET", "migrateSenderThreadnum", "1"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(svr->getParams()->migrateSenderThreadnum, 1);

  sess.setArgs({"CONFIG", "SET", "incrPushThreadnum", "1"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(svr->getParams()->incrPushThreadnum, 1);

  sess.setArgs({"CONFIG", "SET", "migrateReceiveThreadnum", "1"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(svr->getParams()->migrateReceiveThreadnum, 1);

  // index manager
  sess.setArgs({"CONFIG", "SET", "scanJobCntIndexMgr", "1"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(svr->getParams()->scanJobCntIndexMgr, 1);

  sess.setArgs({"CONFIG", "SET", "delJobCntIndexMgr", "1"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(svr->getParams()->delJobCntIndexMgr, 1);

  // total sleep 10s to wait thread resize ok
  sleep(10);
  EXPECT_EQ(svr->getReplManager()->incrPusherSize(), 1);
  EXPECT_EQ(svr->getReplManager()->fullPusherSize(), 1);
  EXPECT_EQ(svr->getReplManager()->fullReceiverSize(), 1);
  EXPECT_EQ(svr->getReplManager()->logRecycleSize(), 1);
  EXPECT_EQ(svr->getMigrateManager()->migrateSenderSize(), 1);
  EXPECT_EQ(svr->getMigrateManager()->migrateReceiverSize(), 1);
  EXPECT_EQ(svr->getIndexMgr()->indexScannerSize(), 1);
  EXPECT_EQ(svr->getIndexMgr()->keyDeleterSize(), 1);
}

TEST(Command, resizeCommand) {
  const auto guard = MakeGuard([]() { destroyEnv(); });
  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();

  // note: migrate resize needs cluster-enabled flag on, default is off.
  cfg->clusterEnabled = true;
  getGlobalServer() = makeServerEntry(cfg);

  testResizeCommand(getGlobalServer());

#ifndef _WIN32
  getGlobalServer()->stop();
  EXPECT_EQ(getGlobalServer().use_count(), 1);
#endif
}

TEST(Command, adminSet_Get_DelCommand) {
  const auto guard = MakeGuard([] { destroyEnv(); });
  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  cfg->kvStoreCount = 3;
  auto server = makeServerEntry(cfg);

  std::vector<std::vector<std::string>> wrongArr = {
    {"ADMINSET"},
    {"ADMINSET", "test"},

    {"ADMINGET"},
    {"ADMINGET", "test", "storeid", std::to_string(cfg->kvStoreCount + 1)},
    {"ADMINGET", "test", "storeid", "("},

    {"ADMINDEL"},
  };

  std::vector<std::pair<std::vector<std::string>, std::string>> resultArr = {
    {{"ADMINSET", "test", "xx"}, Command::fmtOK()},

    {{"ADMINGET", "test"},
     "*3\r\n*2\r\n$1\r\n0\r\n$2\r\nxx\r\n"
     "*2\r\n$1\r\n1\r\n$2\r\nxx\r\n*2\r\n$1\r\n2\r\n$2\r\nxx\r\n"},
    {{"ADMINGET", "test", "storeid", "2"},
     "*1\r\n*2\r\n$1\r\n2\r\n$2\r\nxx\r\n"},

    {{"ADMINDEL", "test"}, Command::fmtOne()},
    {{"ADMINDEL", "test"}, Command::fmtZero()},
    {{"ADMINGET", "test"},
     "*3\r\n*2\r\n$1\r\n0\r\n$-1\r\n*2\r\n"
     "$1\r\n1\r\n$-1\r\n*2\r\n$1\r\n2\r\n$-1\r\n"},
  };

  testCommandArray(server, wrongArr, true);
  testCommandArrayResult(server, resultArr);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

TEST(Command, LogError) {
  const auto guard = MakeGuard([] { destroyEnv(); });
  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  cfg->kvStoreCount = 3;
  auto server = makeServerEntry(cfg);

  EXPECT_EQ(server->getInternalErrorCnt(), 0);
  std::string key = "logerrortest";
  std::vector<std::pair<std::vector<std::string>, std::string>> resultArr = {
    {{"set", key, "a"}, Command::fmtOK()},
    {{"hset", key, "f1", "0"},
     "-WRONGTYPE Operation against a key holding the wrong kind of "
     "value(" +
       key + ")\r\n"},
  };

  testCommandArrayResult(server, resultArr);
  // log-error is off
  EXPECT_EQ(server->getInternalErrorCnt(), 0);

  std::vector<std::pair<std::vector<std::string>, std::string>> resultArr2 = {
    {{"config", "set", "log-error", "1"}, Command::fmtOK()},
    {{"hset", key, "f1", "0"},
     "-WRONGTYPE Operation against a key holding the wrong kind of "
     "value(" +
       key + ")\r\n"},
    {{"sadd", key, "f1"},
     "-WRONGTYPE Operation against a key holding the wrong kind of "
     "value(" +
       key + ")\r\n"},
  };

  testCommandArrayResult(server, resultArr2);
  // log-error is on
  EXPECT_EQ(server->getInternalErrorCnt(), 2);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

TEST(Command, tbitmap) {
  const auto guard = MakeGuard([] { destroyEnv(); });
  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext);
  NetSession sess(server, std::move(socket), 1, false, nullptr, nullptr);

  std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>>
    resultArr = {
      {{"tsetbit", "tsrckey1", "8192", "1"},
       {"setbit", "srckey1", "8192", "1"}},
      {{"tsetbit", "tsrckey2", "8193", "1"},
       {"setbit", "srckey2", "8193", "1"}},
      {{"dump", "tsrckey1"}, {"dump", "srckey1"}},
      {{"dump", "tsrckey2"}, {"dump", "srckey2"}},
      {{"tbitop", "or", "tdestkey", "tsrckey1", "tsrckey2"},
       {"bitop", "or", "destkey", "srckey1", "srckey2"}},
      {{"dump", "tdestkey"}, {"dump", "destkey"}},

      {{"tsetbit", "tsrckey3", "8194", "1"},
       {"setbit", "srckey3", "8194", "1"}},
      {{"tsetbit", "tsrckey4", "24289", "1"},
       {"setbit", "srckey4", "24289", "1"}},
      {{"tbitop",
        "or",
        "tdestkey",
        "tsrckey1",
        "tsrckey2",
        "tsrckey3",
        "tsrckey4"},  // NOLINT
       {"bitop", "or", "destkey", "srckey1", "srckey2", "srckey3", "srckey4"}},
      {{"dump", "tdestkey"}, {"dump", "destkey"}},

      {{"tsetbit", "tsrckey5", "24290", "1"},
       {"setbit", "srckey5", "24290", "1"}},
      {{"tbitop", "and", "tdestkey", "tdestkey", "tsrckey5"},
       {"bitop", "and", "destkey", "destkey", "srckey5"}},
      {{"dump", "tdestkey"}, {"dump", "destkey"}},

      {{"tbitcount", "tbc1"}, {"bitcount", "bc1"}},
      {{"tbitpos", "tbc1", "1"}, {"bitpos", "bc1", "1"}},
      {{"tbitpos", "tbc1", "0"}, {"bitpos", "bc1", "0"}},

      {{"tsetbit", "tbc1", "7", "1"}, {"setbit", "bc1", "7", "1"}},
      {{"tsetbit", "tbc1", "8", "1"}, {"setbit", "bc1", "8", "1"}},
      {{"tbitcount", "tbc1"}, {"bitcount", "bc1"}},
      {{"tbitcount", "tbc1", "10", "2"}, {"bitcount", "bc1", "10", "2"}},
      {{"tbitcount", "tbc1", "0", "100"}, {"bitcount", "bc1", "0", "100"}},
      {{"tbitcount", "tbc1", "1", "100"}, {"bitcount", "bc1", "1", "100"}},
      {{"tbitcount", "tbc1", "10", "100"}, {"bitcount", "bc1", "10", "100"}},

      {{"tsetbit", "tbc1", "50000", "1"}, {"setbit", "bc1", "50000", "1"}},
      {{"tbitcount", "tbc1"}, {"bitcount", "bc1"}},
      {{"tbitcount", "tbc1", "0", "100"}, {"bitcount", "bc1", "0", "100"}},
      {{"tbitcount", "tbc1", "1", "100"}, {"bitcount", "bc1", "1", "100"}},
      {{"tbitcount", "tbc1", "1000", "2000"},
       {"bitcount", "bc1", "1000", "2000"}},
      {{"tbitcount", "tbc1", "1000", "-1"}, {"bitcount", "bc1", "1000", "-1"}},
      {{"tbitcount", "tbc1", "2000", "-1"}, {"bitcount", "bc1", "2000", "-1"}},
      {{"dump", "tbc1"}, {"dump", "bc1"}},

      {{"tbitpos", "tbc1", "1"}, {"bitpos", "bc1", "1"}},
      {{"tbitpos", "tbc1", "1", "10", "2"}, {"bitpos", "bc1", "1", "10", "2"}},
      {{"tbitpos", "tbc1", "0", "10", "2"}, {"bitpos", "bc1", "0", "10", "2"}},
      {{"tbitpos", "tbc1", "1", "1", "2"}, {"bitpos", "bc1", "1", "1", "2"}},
      {{"tbitpos", "tbc1", "1", "100", "200"},
       {"bitpos", "bc1", "1", "100", "200"}},
    };

  auto fragArr = {8, 1024};
  for (auto fraglen : fragArr) {
    sess.setArgs(
      {"config", "set", "tbitmap-fragment-size", std::to_string(fraglen)});
    auto expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"del",
                  "srckey1",
                  "srckey2",
                  "srckey3",
                  "srckey4",
                  "destkey",
                  "srckey5",
                  "bc1",
                  "bc2",
                  "bc3"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    sess.setArgs({"del",
                  "tsrckey1",
                  "tsrckey2",
                  "tsrckey3",
                  "tsrckey4",
                  "tdestkey",
                  "tsrckey5",
                  "tbc1",
                  "tbc2",
                  "tbc3"});
    expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());

    testDiffCommandArray(server, resultArr);
  }

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

TEST(Command, rocksdbOptionsCommand) {
  const auto guard = MakeGuard([]() { destroyEnv(); });
  EXPECT_TRUE(setupEnv());
  std::map<std::string, std::string> configMap = {
    {"rocks.enable_blob_files", "1"},
    {"rocks.binlogcf.enable_blob_files", "1"},
    {"rocks.blob_garbage_collection_age_cutoff", "0.12"},
    {"rocks.blob_compression_type", "lz4"}};
  auto cfg = makeServerParam(8811, 0, "", true, configMap);

  getGlobalServer() = makeServerEntry(cfg);

  testRocksOptionCommand(getGlobalServer());
  testConfigSetAndGet(getGlobalServer());

#ifndef _WIN32
  getGlobalServer()->stop();
  EXPECT_EQ(getGlobalServer().use_count(), 1);
#endif
}

void testSort(bool clusterEnabled) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  cfg->clusterEnabled = clusterEnabled;
  cfg->generalLog = true;
  cfg->logLevel = "debug";
  auto server = makeServerEntry(cfg);

  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext);
  NetSession sess(server, std::move(socket), 1, false, nullptr, nullptr);

  if (clusterEnabled) {
    sess.setArgs({"cluster", "addslots", "{0..16383}"});
    auto expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
  }
  std::this_thread::sleep_for(std::chrono::seconds(3));

  // uid
  sess.setArgs({"LPUSH", "uid", "2", "3", "1"});
  auto expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  // name
  sess.setArgs({"set", "user_name_1", "admin"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  sess.setArgs({"set", "user_name_2", "jack"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  sess.setArgs({"set", "user_name_3", "mary"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  // level
  sess.setArgs({"set", "user_level_1", "10"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  sess.setArgs({"set", "user_level_2", "5"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  sess.setArgs({"set", "user_level_3", "8"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  // sort
  sess.setArgs({"sort", "uid"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_EQ(expect.value(), "*3\r\n$1\r\n1\r\n$1\r\n2\r\n$1\r\n3\r\n");

  // sort by
  sess.setArgs({"sort", "uid", "by", "user_level_*"});
  expect = Command::runSessionCmd(&sess);
  if (!clusterEnabled) {
    EXPECT_EQ(expect.value(), "*3\r\n$1\r\n2\r\n$1\r\n3\r\n$1\r\n1\r\n");
  } else {
    EXPECT_EQ(expect.status().toString(),
              "-ERR BY option of SORT denied in Cluster mode.\r\n");
  }

  // sort get
  sess.setArgs({"sort", "uid", "get", "user_name_*"});
  expect = Command::runSessionCmd(&sess);
  if (!clusterEnabled) {
    EXPECT_EQ(expect.value(),
              "*3\r\n$5\r\nadmin\r\n$4\r\njack\r\n$4\r\nmary\r\n");
  } else {
    EXPECT_EQ(expect.status().toString(),
              "-ERR GET option of SORT denied in Cluster mode.\r\n");
  }

  // sort get nil
  sess.setArgs({"sort", "uid", "get", "user_name_*", "get", "_:*"});
  expect = Command::runSessionCmd(&sess);
  if (!clusterEnabled) {
    EXPECT_EQ(expect.value(),
              "*6\r\n$5\r\nadmin\r\n$-1\r\n$4\r\njack\r\n"
              "$-1\r\n$4\r\nmary\r\n$-1\r\n");
  } else {
    EXPECT_EQ(expect.status().toString(),
              "-ERR GET option of SORT denied in Cluster mode.\r\n");
  }

  sess.setArgs({"LPUSH", "{a}list1", "2", "3", ""});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());

  // sort get *
  sess.setArgs({"sort", "{a}list1", "alpha", "get", "*"});
  expect = Command::runSessionCmd(&sess);
  if (!clusterEnabled) {
    // nil, nil, nil
    EXPECT_EQ(expect.value(), "*3\r\n$-1\r\n$-1\r\n$-1\r\n");
  } else {
    EXPECT_EQ(expect.status().toString(),
              "-ERR GET option of SORT denied in Cluster mode.\r\n");
  }

  // sort get #
  sess.setArgs({"sort", "{a}list1", "alpha", "get", "#"});
  expect = Command::runSessionCmd(&sess);
  if (!clusterEnabled) {
    // "", 2, 3
    EXPECT_EQ(expect.value(), "*3\r\n$0\r\n\r\n$1\r\n2\r\n$1\r\n3\r\n");
  } else {
    EXPECT_EQ(expect.status().toString(),
              "-ERR GET option of SORT denied in Cluster mode.\r\n");
  }

  // sort store, cross slot
  sess.setArgs({"sort", "{a}list1", "alpha", "store", "list1"});
  expect = Command::runSessionCmd(&sess);
  if (!clusterEnabled) {
    EXPECT_TRUE(expect.ok());
    EXPECT_EQ(expect.value(), ":3\r\n");
  } else {
    EXPECT_EQ(expect.status().toString(),
              "-CROSSSLOT Keys in request don't hash to the same slot\r\n");
  }

  // sort store, contain ""
  sess.setArgs({"sort", "{a}list1", "alpha", "store", "{a}list2"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  EXPECT_EQ(expect.value(), ":3\r\n");

  sess.setArgs({"lrange", "{a}list2", "0", "-1"});
  expect = Command::runSessionCmd(&sess);
  // "", 2, 3
  EXPECT_EQ(expect.value(), "*3\r\n$0\r\n\r\n$1\r\n2\r\n$1\r\n3\r\n");

  // sort store, contain nil
  sess.setArgs({"sort", "{a}list1", "alpha", "get", "*", "store", "{a}list3"});
  expect = Command::runSessionCmd(&sess);
  if (!clusterEnabled) {
    EXPECT_TRUE(expect.ok());
    EXPECT_EQ(expect.value(), ":3\r\n");
  } else {
    EXPECT_EQ(expect.status().toString(),
              "-ERR GET option of SORT denied in Cluster mode.\r\n");
  }
  sess.setArgs({"lrange", "{a}list3", "0", "-1"});
  expect = Command::runSessionCmd(&sess);
  if (!clusterEnabled) {
    // sort store: nil will be changed to ""
    // "", "", ""
    EXPECT_EQ(expect.value(), "*3\r\n$0\r\n\r\n$0\r\n\r\n$0\r\n\r\n");
  } else {
    EXPECT_EQ(expect.value(), "*0\r\n");
  }

  sess.setArgs({"set", "2", "b"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  // sort get *, need return value:b of key:2
  sess.setArgs({"sort", "{a}list1", "alpha", "get", "*"});
  expect = Command::runSessionCmd(&sess);
  if (!clusterEnabled) {
    EXPECT_TRUE(expect.ok());
    EXPECT_EQ(expect.value(), "*3\r\n$-1\r\n$1\r\nb\r\n$-1\r\n");
  } else {
    EXPECT_EQ(expect.status().toString(),
              "-ERR GET option of SORT denied in Cluster mode.\r\n");
  }

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

TEST(Command, sort_cluster) {
  testSort(false);
  testSort(true);
}

// call dbsize and flushall at the same time
TEST(Command, testDbsizeAndFlushall) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());

  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  testCommand(server);

  std::thread thd1([&server]() {
    for (int i = 0; i < 100; i++) {
      asio::io_context ioContext;
      asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
      NetSession sess(server, std::move(socket), 1, false, nullptr, nullptr);

      sess.setArgs({"dbsize"});
      auto expect = Command::runSessionCmd(&sess);
      EXPECT_TRUE(expect.ok());
      EXPECT_EQ(":0\r\n", expect.value());
    }
  });

  std::thread thd2([&server]() {
    for (int i = 0; i < 10; i++) {
      asio::io_context ioContext;
      asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
      NetSession sess(server, std::move(socket), 1, false, nullptr, nullptr);

      sess.setArgs({"flushall"});
      auto expect = Command::runSessionCmd(&sess);
      EXPECT_TRUE(expect.ok());
      EXPECT_EQ("+OK\r\n", expect.value());
    }
  });

  thd1.join();
  thd2.join();

  remove(cfg->getConfFile().c_str());

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

// flushall with WalDir different with DBPath
TEST(Command, testFlushallWithRocksDBPath) {
  std::string walPath = "./wal";
  std::error_code ec;

  const auto guard = MakeGuard([&walPath, &ec] {
    destroyEnv();
    filesystem::remove_all(walPath, ec);
  });

  EXPECT_TRUE(setupEnv());
  EXPECT_TRUE(filesystem::create_directory(walPath));

  auto cfg = makeServerParam();
  cfg->rocksWALDir = walPath;
  auto server = makeServerEntry(cfg);

  testCommand(server);

  {
    asio::io_context ioContext;
    asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
    NetSession sess(server, std::move(socket), 1, false, nullptr, nullptr);

    sess.setArgs({"flushall"});
    auto expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
    EXPECT_EQ("+OK\r\n", expect.value());
  }

  remove(cfg->getConfFile().c_str());

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

void testHsize(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  for (uint32_t i = 0; i < 10; i++) {
    sess.setArgs({"hset", "hkey", "field_" + std::to_string(i), "value"});
    auto expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
  }

  sess.setArgs({"hsize", "hkey"});
  auto expect = Command::runSessionCmd(&sess);
  // the value is not constant, only check not be zero.
  // NOTE(Raffertyyu): may get wrong size (0) in some cases.
  // EXPECT_NE(expect.value(), Command::fmtLongLong(0));
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"hsize", "hkey", "withoutmemtables"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  // data is too little, havn't flush to disk.
  // EXPECT_EQ(expect.value(), Command::fmtLongLong(0));

  sess.setArgs({"hsize", "hkey", "err_arg"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(!expect.ok());
}

void testLsize(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  for (uint32_t i = 0; i < 10; i++) {
    sess.setArgs({"lpush", "lkey", "field_" + std::to_string(i)});
    auto expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
  }

  sess.setArgs({"lsize", "lkey"});
  auto expect = Command::runSessionCmd(&sess);
  // the value is not constant, only check not be zero.
  // EXPECT_NE(expect.value(), Command::fmtLongLong(0));
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"lsize", "lkey", "withoutmemtables"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  // data is too little, havn't flush to disk.
  // EXPECT_EQ(expect.value(), Command::fmtLongLong(0));

  sess.setArgs({"lsize", "lkey", "err_arg"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(!expect.ok());
}

void testSsize(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  for (uint32_t i = 0; i < 10; i++) {
    sess.setArgs({"sadd", "skey", "field_" + std::to_string(i)});
    auto expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
  }

  sess.setArgs({"ssize", "skey"});
  auto expect = Command::runSessionCmd(&sess);
  // the value is not constant, only check not be zero.
  // EXPECT_NE(expect.value(), Command::fmtLongLong(0));
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"ssize", "skey", "withoutmemtables"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  // data is too little, havn't flush to disk.
  // EXPECT_EQ(expect.value(), Command::fmtLongLong(0));

  sess.setArgs({"ssize", "skey", "err_arg"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(!expect.ok());
}

void testZsize(std::shared_ptr<ServerEntry> svr) {
  asio::io_context ioContext;
  asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
  NetSession sess(svr, std::move(socket), 1, false, nullptr, nullptr);

  for (uint32_t i = 0; i < 10; i++) {
    sess.setArgs(
      {"zadd", "zkey", std::to_string(i), "field_" + std::to_string(i)});
    auto expect = Command::runSessionCmd(&sess);
    EXPECT_TRUE(expect.ok());
  }

  sess.setArgs({"zsize", "zkey"});
  auto expect = Command::runSessionCmd(&sess);
  // the value is not constant, only check not be zero.
  // EXPECT_NE(expect.value(), Command::fmtLongLong(0));
  EXPECT_TRUE(expect.ok());

  sess.setArgs({"zsize", "zkey", "withoutmemtables"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(expect.ok());
  // data is too little, havn't flush to disk.
  // EXPECT_EQ(expect.value(), Command::fmtLongLong(0));

  sess.setArgs({"zsize", "zkey", "err_arg"});
  expect = Command::runSessionCmd(&sess);
  EXPECT_TRUE(!expect.ok());
}

TEST(Command, XsizeCommand) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);

  testHsize(server);
  testLsize(server);
  testSsize(server);
  testZsize(server);

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

// NOTE(takenliu): renameCommand may change command's name or behavior, so put
// it in the end
// INSTANTIATE_TEST_CASE_P may run later TEST(Command, renameCommand)
extern std::string gRenameCmdList;
extern std::string gMappingCmdList;
TEST(Command, renameCommand) {
  const auto guard = MakeGuard([] { destroyEnv(); });

  EXPECT_TRUE(setupEnv());
  auto cfg = makeServerParam();
  auto server = makeServerEntry(cfg);
  gRenameCmdList += ",set set_rename";
  gMappingCmdList += ",dbsize emptyint,keys emptymultibulk";
  Command::changeCommand(gRenameCmdList, "rename");
  Command::changeCommand(gMappingCmdList, "mapping");

  testRenameCommand(server);

  gRenameCmdList = ",set_rename set";
  gMappingCmdList = ",emptyint dbsize,emptymultibulk keys";
  Command::changeCommand(gRenameCmdList, "rename");
  Command::changeCommand(gMappingCmdList, "mapping");

  // reset commandMap() back
  {
    asio::io_context ioContext;
    asio::ip::tcp::socket socket(ioContext), socket1(ioContext);
    NetSession sess(server, std::move(socket), 1, false, nullptr, nullptr);

    sess.setArgs({"set_rename"});
    auto eprecheck = Command::precheck(&sess);
    EXPECT_EQ(Command::fmtErr("unknown command 'set_rename'"),
              eprecheck.status().toString());

    sess.setArgs({"set", "a", "1"});
    auto expect = Command::runSessionCmd(&sess);
    EXPECT_EQ(Command::fmtOK(), expect.value());
  }
  gRenameCmdList = "";
  gMappingCmdList = "";

#ifndef _WIN32
  server->stop();
  EXPECT_EQ(server.use_count(), 1);
#endif
}

// NOTE(takenliu): don't add test here, and it before Command.renameCommand

}  // namespace novadbplus
