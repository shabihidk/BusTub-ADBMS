//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// disk_scheduler_test.cpp
//
// Identification: test/storage/disk_scheduler_test.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <cstring>
#include <future>  // NOLINT
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/exception.h"
#include "gtest/gtest.h"
#include "storage/disk/disk_manager_memory.h"
#include "storage/disk/disk_scheduler.h"

namespace bustub {

using bustub::DiskManagerUnlimitedMemory;

// NOLINTNEXTLINE
TEST(DiskSchedulerTest, ScheduleWriteReadPageTest) {
  char buf[BUSTUB_PAGE_SIZE] = {0};
  char data[BUSTUB_PAGE_SIZE] = {0};

  auto dm = std::make_unique<DiskManagerUnlimitedMemory>();
  auto disk_scheduler = std::make_unique<DiskScheduler>(dm.get());

  std::strncpy(data, "A test string.", sizeof(data));

  auto promise1 = disk_scheduler->CreatePromise();
  auto future1 = promise1.get_future();
  auto promise2 = disk_scheduler->CreatePromise();
  auto future2 = promise2.get_future();

  DiskRequest r1{/*is_write=*/true, data, /*page_id=*/0, std::move(promise1)};
  std::vector<DiskRequest> requests1;
  requests1.push_back(std::move(r1));
  disk_scheduler->Schedule(requests1);

  DiskRequest r2{/*is_write=*/false, buf, /*page_id=*/0, std::move(promise2)};
  std::vector<DiskRequest> requests2;
  requests2.push_back(std::move(r2));
  disk_scheduler->Schedule(requests2);

  ASSERT_TRUE(future1.get());
  ASSERT_TRUE(future2.get());
  ASSERT_EQ(std::memcmp(buf, data, sizeof(buf)), 0);

  disk_scheduler = nullptr;  // Call the DiskScheduler destructor to finish all scheduled jobs.
  dm->ShutDown();
}

TEST(DiskSchedulerTest, EmptyAndMultiRequestBatch) {
  auto dm = std::make_unique<DiskManagerUnlimitedMemory>();
  auto disk_scheduler = std::make_unique<DiskScheduler>(dm.get());

  std::vector<DiskRequest> empty;
  disk_scheduler->Schedule(empty);

  char data0[BUSTUB_PAGE_SIZE] = {0};
  char data1[BUSTUB_PAGE_SIZE] = {0};
  char buf0[BUSTUB_PAGE_SIZE] = {0};
  char buf1[BUSTUB_PAGE_SIZE] = {0};
  std::strncpy(data0, "page-zero", sizeof(data0));
  std::strncpy(data1, "page-one", sizeof(data1));

  auto p0 = disk_scheduler->CreatePromise();
  auto f0 = p0.get_future();
  auto p1 = disk_scheduler->CreatePromise();
  auto f1 = p1.get_future();
  std::vector<DiskRequest> writes;
  writes.push_back(DiskRequest{true, data0, 0, std::move(p0)});
  writes.push_back(DiskRequest{true, data1, 1, std::move(p1)});
  disk_scheduler->Schedule(writes);
  ASSERT_TRUE(f0.get());
  ASSERT_TRUE(f1.get());

  auto p2 = disk_scheduler->CreatePromise();
  auto f2 = p2.get_future();
  auto p3 = disk_scheduler->CreatePromise();
  auto f3 = p3.get_future();
  std::vector<DiskRequest> reads;
  reads.push_back(DiskRequest{false, buf0, 0, std::move(p2)});
  reads.push_back(DiskRequest{false, buf1, 1, std::move(p3)});
  disk_scheduler->Schedule(reads);
  ASSERT_TRUE(f2.get());
  ASSERT_TRUE(f3.get());
  ASSERT_EQ(0, std::memcmp(buf0, data0, BUSTUB_PAGE_SIZE));
  ASSERT_EQ(0, std::memcmp(buf1, data1, BUSTUB_PAGE_SIZE));
}

TEST(DiskSchedulerTest, ConcurrentProducersAndShutdown) {
  auto dm = std::make_unique<DiskManagerUnlimitedMemory>();
  auto disk_scheduler = std::make_unique<DiskScheduler>(dm.get());

  constexpr int kProducers = 4;
  constexpr int kPerProducer = 20;
  std::vector<std::thread> threads;
  std::vector<std::vector<char>> buffers(kProducers * kPerProducer, std::vector<char>(BUSTUB_PAGE_SIZE, 0));
  std::vector<std::future<bool>> futures;
  std::mutex futures_mu;

  for (int t = 0; t < kProducers; t++) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kPerProducer; i++) {
        int idx = t * kPerProducer + i;
        std::snprintf(buffers[idx].data(), BUSTUB_PAGE_SIZE, "t%d-i%d", t, i);
        auto promise = disk_scheduler->CreatePromise();
        auto future = promise.get_future();
        std::vector<DiskRequest> reqs;
        reqs.push_back(DiskRequest{true, buffers[idx].data(), static_cast<page_id_t>(idx), std::move(promise)});
        disk_scheduler->Schedule(reqs);
        std::scoped_lock lock(futures_mu);
        futures.push_back(std::move(future));
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }
  for (auto &f : futures) {
    ASSERT_TRUE(f.get());
  }

  disk_scheduler = nullptr;
  char check[BUSTUB_PAGE_SIZE];
  dm->ReadPage(0, check);
  ASSERT_EQ(0, std::memcmp(check, buffers[0].data(), BUSTUB_PAGE_SIZE));
  dm->ShutDown();
}

}  // namespace bustub
