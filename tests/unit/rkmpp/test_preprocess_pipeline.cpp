/**
 * @file tests/unit/rkmpp/test_preprocess_pipeline.cpp
 * @brief Deterministic tests for RKMPP preprocess/encode handoff contracts.
 */

#include "src/platform/linux/rkmpp_preprocess.h"
#include "src/thread_safe.h"

#include <atomic>
#include <gtest/gtest.h>
#include <latch>
#include <thread>

namespace {
  platf::rkmpp::prepared_frame_t prepared(bool request_idr, const std::shared_ptr<void> &holder = {}) {
    return {
      .layout = {1920, 1080, 1920, 1080, MPP_FMT_YUV420SP},
      .dma_buf_fd = 7,
      .allocation_size = 1920U * 1620U,
      .pts = 42,
      .generation = 3,
      .cache_key = {3, 1},
      .holder = holder,
      .route = platf::rkmpp::prepared_route_e::direct,
      .request_idr = request_idr
    };
  }
}  // namespace

TEST(RkmppPreprocessPipeline, PreparedInputPinsProducerThroughSubmissionView) {
  auto released = std::make_shared<std::atomic_bool>(false);
  auto holder = std::shared_ptr<void>(released.get(), [released](void *) {
    released->store(true);
  });
  auto frame = prepared(false, holder);
  holder.reset();

  {
    auto input = frame.input_frame();
    frame.holder.reset();
    EXPECT_FALSE(released->load());
    ASSERT_TRUE(input.cache_key);
    EXPECT_EQ(input.cache_key->generation, 3U);
    EXPECT_EQ(input.cache_key->index, 1U);
  }
  EXPECT_TRUE(released->load());
}

TEST(RkmppPreprocessPipeline, LatestPreparedReplacementTransfersStickyIdrAndReleasesOldHolder) {
  using frame_ptr_t = std::shared_ptr<platf::rkmpp::prepared_frame_t>;
  safe::event_t<frame_ptr_t> mailbox;
  auto old_released = std::make_shared<std::atomic_bool>(false);
  auto old_holder = std::shared_ptr<void>(old_released.get(), [old_released](void *) {
    old_released->store(true);
  });
  mailbox.raise(std::make_shared<platf::rkmpp::prepared_frame_t>(prepared(true, old_holder)));
  old_holder.reset();

  auto replacement = std::make_shared<platf::rkmpp::prepared_frame_t>(prepared(false));
  EXPECT_TRUE(mailbox.raise_latest(replacement, [](frame_ptr_t &old_frame, frame_ptr_t &new_frame) {
    new_frame->request_idr = new_frame->request_idr || old_frame->request_idr;
  }));
  EXPECT_TRUE(old_released->load());
  auto popped = mailbox.pop();
  ASSERT_TRUE(popped);
  EXPECT_TRUE(popped->request_idr);
}

TEST(RkmppPreprocessPipeline, StopTokenInterruptsEmptyRawWaitWithoutTimeout) {
  safe::event_t<std::shared_ptr<int>> mailbox;
  std::latch entered {1};
  std::atomic_bool cancelled {false};
  std::jthread worker {[&](std::stop_token stop_token) {
    entered.count_down();
    cancelled.store(!mailbox.pop(stop_token));
  }};
  entered.wait();
  worker.request_stop();
  worker.join();
  EXPECT_TRUE(cancelled.load());
}

TEST(RkmppPreprocessPipeline, VisibleMatchingBgrUsesDirectVulkanCoverForExclusiveFrame) {
  EXPECT_EQ(
    platf::rkmpp::select_ui_preprocess_path(MPP_FMT_BGR888, true, true, true),
    platf::rkmpp::ui_preprocess_path_e::direct_bgr
  );
}

TEST(RkmppPreprocessPipeline, VisibleNv12UsesPrivateBgrTarget) {
  EXPECT_EQ(
    platf::rkmpp::select_ui_preprocess_path(MPP_FMT_YUV420SP, true, true, true),
    platf::rkmpp::ui_preprocess_path_e::private_bgr
  );
}

TEST(RkmppPreprocessPipeline, SharedOrScaledBgrUsesPrivateBgrTarget) {
  EXPECT_EQ(
    platf::rkmpp::select_ui_preprocess_path(MPP_FMT_BGR888, true, true, false),
    platf::rkmpp::ui_preprocess_path_e::private_bgr
  );
  EXPECT_EQ(
    platf::rkmpp::select_ui_preprocess_path(MPP_FMT_BGR888, false, true, true),
    platf::rkmpp::ui_preprocess_path_e::private_bgr
  );
}

TEST(RkmppPreprocessPipeline, HiddenUiNeverChangesTheNativeRoute) {
  EXPECT_EQ(
    platf::rkmpp::select_ui_preprocess_path(MPP_FMT_YUV420SP, true, false, false),
    platf::rkmpp::ui_preprocess_path_e::hidden
  );
  EXPECT_EQ(
    platf::rkmpp::select_ui_preprocess_path(MPP_FMT_BGR888, true, false, true),
    platf::rkmpp::ui_preprocess_path_e::hidden
  );
}
