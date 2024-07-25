// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#include "gtest/gtest.h"

#include "novadbplus/network/worker_pool.h"
#include "novadbplus/utils/scopeguard.h"
#include "novadbplus/utils/test_util.h"

TEST(Workerpool, resize) {
  auto matrix = std::make_shared<novadbplus::PoolMatrix>();
  novadbplus::WorkerPool pool("test-pool", matrix);
  EXPECT_TRUE(novadbplus::setupEnv());

  std::thread t([&pool]() { pool.startup(5); });

  // note: startup need necessary time to get ready
  usleep(10000);
  ASSERT_EQ(pool.size(), 5);

  pool.resize(10);
  ASSERT_EQ(pool.size(), 10);

  // note: thread resize to decrease is async op, need time to complete.
  pool.resize(5);
  usleep(10000);
  ASSERT_EQ(pool.size(), 5);

  pool.stop();
  t.join();

  auto guard = novadbplus::MakeGuard([]() { novadbplus::destroyEnv(); });
}

TEST(Workerpool, isFull) {
  auto matrix = std::make_shared<novadbplus::PoolMatrix>();
  novadbplus::WorkerPool pool("test-pool", matrix);
  EXPECT_TRUE(novadbplus::setupEnv());

  std::thread t([&pool]() { pool.startup(5); });

  // note: usleep() a short time to wait pool get ready
  // post tasks( >5 ) to make queue become full
  usleep(10000);
  for (size_t i = 0; i < 8; ++i) {
    auto task = []() { sleep(5); };
    pool.schedule(std::move(task));
  }
  ASSERT_EQ(pool.size(), 5);
  ASSERT_TRUE(pool.isFull());

  pool.stop();
  t.join();

  auto guard = novadbplus::MakeGuard([]() { novadbplus::destroyEnv(); });
}

TEST(Workerpool, schedule) {
  auto matrix = std::make_shared<novadbplus::PoolMatrix>();
  novadbplus::WorkerPool pool("test-pool", matrix);

  std::thread t([&pool]() { pool.startup(3); });

  std::atomic<int> val{5};
  pool.schedule([&val]() { val.store(10); });

  usleep(10000);
  ASSERT_EQ(val.load(), 10);

  pool.stop();
  t.join();
  auto guard = novadbplus::MakeGuard([]() { novadbplus::destroyEnv(); });
}
