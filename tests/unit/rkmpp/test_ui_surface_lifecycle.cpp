/**
 * @file tests/unit/rkmpp/test_ui_surface_lifecycle.cpp
 * @brief Verify UI page replacement under a one-surface CMA budget.
 */
#include "src/platform/linux/vulkan_ui_surface.h"

#include <gtest/gtest.h>
#include <stdexcept>
#include <system_error>

namespace {
  /** @brief Fake surface that accounts for live CMA allocations. */
  struct surface_t {
    /** @brief Construct a page only after the previous allocation is released. */
    surface_t(int &live, int page):
        live(live),
        page(page) {
      if (live != 0) {
        throw std::system_error(std::make_error_code(std::errc::not_enough_memory));
      }
      ++live;
    }

    /** @brief Release the fake CMA allocation. */
    ~surface_t() {
      --live;
    }

    int &live;  ///< Shared live allocation counter.
    int page;  ///< Page-family identity.
  };
}  // namespace

TEST(VulkanUiSurfaceLifecycle, RepeatedPageAndSizeChangesNeverOverlapAllocations) {
  int live = 0;
  auto surface = std::make_unique<surface_t>(live, 0);
  for (int page = 1; page <= 100; ++page) {
    platf::vulkan_ui::replace_surface_without_overlap(
      surface,
      [&] {
        EXPECT_EQ(live, 0);
        return std::make_unique<surface_t>(live, page);
      },
      [&]() -> std::unique_ptr<surface_t> {
        ADD_FAILURE() << "A one-surface budget must support page replacement";
        return nullptr;
      }
    );
    ASSERT_TRUE(surface);
    EXPECT_EQ(surface->page, page);
    EXPECT_EQ(live, 1);
  }
  surface.reset();
  EXPECT_EQ(live, 0);
}

TEST(VulkanUiSurfaceLifecycle, FailedChangeReleasesPartialAllocationAndRestoresPreviousLayout) {
  int live = 0;
  auto surface = std::make_unique<surface_t>(live, 1);
  EXPECT_THROW(
    platf::vulkan_ui::replace_surface_without_overlap(
      surface,
      [&]() -> std::unique_ptr<surface_t> {
        auto partial = std::make_unique<surface_t>(live, 2);
        throw std::runtime_error("renderer initialization failed");
      },
      [&] {
        EXPECT_EQ(live, 0);
        return std::make_unique<surface_t>(live, 1);
      }
    ),
    std::runtime_error
  );
  ASSERT_TRUE(surface);
  EXPECT_EQ(surface->page, 1);
  EXPECT_EQ(live, 1);
  surface.reset();
  EXPECT_EQ(live, 0);
}

TEST(VulkanUiSurfaceLifecycle, InitialConstructionFailureLeavesNoSurface) {
  std::unique_ptr<surface_t> surface;
  EXPECT_THROW(
    platf::vulkan_ui::replace_surface_without_overlap(
      surface,
      []() -> std::unique_ptr<surface_t> {
        throw std::runtime_error("initial allocation failed");
      },
      []() -> std::unique_ptr<surface_t> {
        return nullptr;
      }
    ),
    std::runtime_error
  );
  EXPECT_FALSE(surface);
}

TEST(VulkanUiSurfaceLifecycle, FailedRestoreLeavesNoStaleResourceForRendering) {
  int live = 0;
  auto surface = std::make_unique<surface_t>(live, 1);
  EXPECT_THROW(
    platf::vulkan_ui::replace_surface_without_overlap(
      surface,
      []() -> std::unique_ptr<surface_t> {
        throw std::runtime_error("requested allocation failed");
      },
      []() -> std::unique_ptr<surface_t> {
        throw std::system_error(std::make_error_code(std::errc::not_enough_memory));
      }
    ),
    std::system_error
  );
  EXPECT_FALSE(surface);
  EXPECT_EQ(live, 0);
}
