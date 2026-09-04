/**
 * @file tests/unit/rkmpp/test_rga.cpp
 * @brief Unit tests for the hardware-independent librga wrapper boundary.
 */
#include <linux/videodev2.h>

#include <gtest/gtest.h>

#include <src/platform/linux/rga.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
  /**
   * @brief Return a successful fake RGA status.
   *
   * @param native_code Deliberately nonzero native success code when desired.
   * @return Normalized successful status.
   */
  platf::rga::status_t successful_status(int native_code = 0) {
    return {true, native_code, "fake success"};
  }

  class fake_backend_t final : public platf::rga::backend_t {
  public:
    platf::rga::status_t import_dma_buf(const platf::rga::image_layout_t &, std::uintptr_t &handle) override {
      ++imports;
      if (fail_import) {
        return {false, 7, "fake import failure"};
      }
      handle = next_handle++;
      return successful_status();
    }

    platf::rga::status_t release_dma_buf(std::uintptr_t handle) override {
      events.push_back("release:" + std::to_string(handle));
      if (throw_on_release) {
        throw std::runtime_error("fake release exception");
      }
      return release_status;
    }

    platf::rga::status_t check_fill(std::uintptr_t, const platf::rga::image_layout_t &, const platf::rga::rectangle_t &) override {
      ++fill_checks;
      return fill_check_status;
    }

    platf::rga::status_t fill(std::uintptr_t, const platf::rga::image_layout_t &, const platf::rga::rectangle_t &, std::uint32_t) override {
      ++fills;
      return fill_status;
    }

    platf::rga::status_t check_process(std::uintptr_t, const platf::rga::image_layout_t &, const platf::rga::rectangle_t &, std::uintptr_t, const platf::rga::image_layout_t &, const platf::rga::rectangle_t &, platf::rga::color_space_e color_space) override {
      ++process_checks;
      last_process_check_color_space = color_space;
      return process_check_status;
    }

    platf::rga::status_t process(std::uintptr_t, const platf::rga::image_layout_t &, const platf::rga::rectangle_t &, std::uintptr_t, const platf::rga::image_layout_t &, const platf::rga::rectangle_t &, platf::rga::color_space_e color_space) override {
      ++processes;
      last_process_color_space = color_space;
      return process_status;
    }

    platf::rga::status_t check_resize(std::uintptr_t, const platf::rga::image_layout_t &, std::uintptr_t, const platf::rga::image_layout_t &) override {
      ++resize_checks;
      return resize_check_status;
    }

    platf::rga::status_t resize(std::uintptr_t, const platf::rga::image_layout_t &, std::uintptr_t, const platf::rga::image_layout_t &) override {
      ++resizes;
      return resize_status;
    }

    platf::rga::status_t check_color_convert(std::uintptr_t, const platf::rga::image_layout_t &, std::uintptr_t, const platf::rga::image_layout_t &) override {
      ++color_checks;
      return color_check_status;
    }

    platf::rga::status_t color_convert(std::uintptr_t, const platf::rga::image_layout_t &, std::uintptr_t, const platf::rga::image_layout_t &) override {
      ++color_converts;
      return color_status;
    }

    bool fail_import {};
    bool throw_on_release {};
    std::uintptr_t next_handle {1};
    int imports {};
    int fill_checks {};
    int fills {};
    int process_checks {};
    int processes {};
    int resize_checks {};
    int resizes {};
    int color_checks {};
    int color_converts {};
    platf::rga::color_space_e last_process_check_color_space {platf::rga::color_space_e::default_};
    platf::rga::color_space_e last_process_color_space {platf::rga::color_space_e::default_};
    std::vector<std::string> events;
    platf::rga::status_t release_status {successful_status()};
    platf::rga::status_t fill_check_status {successful_status()};
    platf::rga::status_t fill_status {successful_status()};
    platf::rga::status_t process_check_status {successful_status()};
    platf::rga::status_t process_status {successful_status()};
    platf::rga::status_t resize_check_status {successful_status()};
    platf::rga::status_t resize_status {successful_status()};
    platf::rga::status_t color_check_status {successful_status()};
    platf::rga::status_t color_status {successful_status()};
  };

  class fake_allocator_t final : public platf::rga::dma_allocator_t {
  public:
    int allocate(std::uint64_t size) override {
      allocation_sizes.push_back(size);
      if (fail_allocate) {
        throw std::runtime_error("fake allocation failure");
      }
      return next_fd++;
    }

    void close(int dma_buf_fd) noexcept override {
      events->push_back("close:" + std::to_string(dma_buf_fd));
      closed_fds.push_back(dma_buf_fd);
    }

    bool fail_allocate {};
    int next_fd {100};
    std::vector<std::string> *events {};
    std::vector<std::uint64_t> allocation_sizes;
    std::vector<int> closed_fds;
  };

  platf::rga::image_layout_t nv12_layout(int fd = 10) {
    return {fd, 128, 64, 128, 12'288, platf::rga::pixel_format_e::nv12};
  }

  /**
   * @brief Build a valid layout for each wrapper format.
   *
   * @param format Requested pixel format.
   * @return Valid format-specific DMA-BUF metadata.
   */
  platf::rga::image_layout_t layout_for(platf::rga::pixel_format_e format) {
    switch (format) {
      case platf::rga::pixel_format_e::rgba8888:
        return {10, 128, 64, 512, 32'768, format};
      case platf::rga::pixel_format_e::bgr888:
        return {10, 128, 64, 384, 24'576, format};
      case platf::rga::pixel_format_e::nv24:
        return {10, 128, 64, 128, 24'576, format};
      case platf::rga::pixel_format_e::nv16:
        return {10, 128, 64, 128, 16'384, format};
      case platf::rga::pixel_format_e::nv12:
        return nv12_layout();
    }
    return nv12_layout();
  }

  TEST(RgaFormat, MapsKnownHdmirxFourccsWithoutClaimingRuntimeSupport) {
    EXPECT_EQ(platf::rga::format_from_v4l2_fourcc(V4L2_PIX_FMT_BGR24), platf::rga::pixel_format_e::bgr888);
    EXPECT_EQ(platf::rga::format_from_v4l2_fourcc(V4L2_PIX_FMT_NV24), platf::rga::pixel_format_e::nv24);
    EXPECT_EQ(platf::rga::format_from_v4l2_fourcc(V4L2_PIX_FMT_NV16), platf::rga::pixel_format_e::nv16);
    EXPECT_EQ(platf::rga::format_from_v4l2_fourcc(V4L2_PIX_FMT_NV12), platf::rga::pixel_format_e::nv12);
    EXPECT_FALSE(platf::rga::format_from_v4l2_fourcc(v4l2_fourcc('T', 'E', 'S', 'T')));
  }

  TEST(RgaImport, RejectsInvalidFdAndLayoutBeforeCallingBackend) {
    fake_backend_t backend;
    auto invalid_fd = nv12_layout(-1);
    EXPECT_THROW(platf::rga::imported_buffer_t::import(backend, invalid_fd), platf::rga::error_t);
    auto invalid_size = nv12_layout();
    invalid_size.allocation_size = 1;
    EXPECT_THROW(platf::rga::imported_buffer_t::import(backend, invalid_size), platf::rga::error_t);
    auto invalid_dimensions = nv12_layout();
    invalid_dimensions.width = 127;
    EXPECT_THROW(platf::rga::imported_buffer_t::import(backend, invalid_dimensions), platf::rga::error_t);
    EXPECT_EQ(backend.imports, 0);
  }

  TEST(RgaImport, ValidatesMinimumAllocationAndStrideForEveryMappedFormat) {
    fake_backend_t backend;
    for (const auto format : {platf::rga::pixel_format_e::rgba8888, platf::rga::pixel_format_e::bgr888, platf::rga::pixel_format_e::nv24, platf::rga::pixel_format_e::nv16, platf::rga::pixel_format_e::nv12}) {
      auto valid = layout_for(format);
      EXPECT_TRUE(platf::rga::imported_buffer_t::import(backend, valid));
      auto undersized = valid;
      --undersized.allocation_size;
      EXPECT_THROW(platf::rga::imported_buffer_t::import(backend, undersized), platf::rga::error_t);
    }
    auto invalid_bgr_stride = layout_for(platf::rga::pixel_format_e::bgr888);
    invalid_bgr_stride.stride = 386;
    invalid_bgr_stride.allocation_size = 24'704;
    EXPECT_THROW(platf::rga::imported_buffer_t::import(backend, invalid_bgr_stride), platf::rga::error_t);
  }

  TEST(RgaImport, RejectsAllocationThatCannotUseTheIntImportOverload) {
    fake_backend_t backend;
    auto too_large = nv12_layout();
    too_large.allocation_size = static_cast<std::uint64_t>(std::numeric_limits<int>::max()) + 1U;
    EXPECT_THROW(platf::rga::imported_buffer_t::import(backend, too_large), platf::rga::error_t);
    EXPECT_EQ(backend.imports, 0);
  }

  TEST(RgaImport, ReleasesExactlyOnceAfterSuccessfulImportAndMove) {
    fake_backend_t backend;
    {
      auto first = platf::rga::imported_buffer_t::import(backend, nv12_layout());
      auto second = std::move(first);
      EXPECT_FALSE(first);
      EXPECT_TRUE(second);
    }
    EXPECT_EQ(backend.events, (std::vector<std::string> {"release:1"}));
  }

  TEST(RgaImport, MoveAssignmentReleasesThePreviousHandleBeforeTakingOwnership) {
    fake_backend_t backend;
    {
      auto first = platf::rga::imported_buffer_t::import(backend, nv12_layout(10));
      auto second = platf::rga::imported_buffer_t::import(backend, nv12_layout(11));
      second = std::move(first);
    }
    EXPECT_EQ(backend.events, (std::vector<std::string> {"release:2", "release:1"}));
  }

  TEST(RgaImport, ReportsBackendErrorTextAndDoesNotReleaseMissingHandle) {
    fake_backend_t backend;
    backend.fail_import = true;
    try {
      (void) platf::rga::imported_buffer_t::import(backend, nv12_layout());
      FAIL() << "expected RGA import failure";
    } catch (const platf::rga::error_t &error) {
      EXPECT_NE(std::string(error.what()).find("RGA status=7"), std::string::npos);
      EXPECT_NE(std::string(error.what()).find("fake import failure"), std::string::npos);
    }
    EXPECT_TRUE(backend.events.empty());
  }

  TEST(RgaImport, LogsFailedReleaseStatusWithoutThrowingFromDestruction) {
    fake_backend_t backend;
    backend.release_status = {false, 12, "fake release failure"};
    testing::internal::CaptureStderr();
    {
      auto buffer = platf::rga::imported_buffer_t::import(backend, nv12_layout());
      EXPECT_TRUE(buffer);
    }
    const auto log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("releasebuffer_handle failed"), std::string::npos);
    EXPECT_NE(log.find("RGA status=12"), std::string::npos);
    EXPECT_NE(log.find("fake release failure"), std::string::npos);
  }

  TEST(RgaImport, CatchesBackendReleaseExceptionsDuringDestruction) {
    fake_backend_t backend;
    backend.throw_on_release = true;
    testing::internal::CaptureStderr();
    {
      auto buffer = platf::rga::imported_buffer_t::import(backend, nv12_layout());
      EXPECT_TRUE(buffer);
    }
    const auto log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("releasebuffer_handle threw during cleanup"), std::string::npos);
    EXPECT_NE(log.find("fake release exception"), std::string::npos);
  }

  TEST(RgaTarget, ReleasesRgaHandleBeforeClosingOwnedDmaBuf) {
    fake_backend_t backend;
    fake_allocator_t allocator;
    allocator.events = &backend.events;
    {
      auto target = platf::rga::target_buffer_t::allocate_nv12(backend, allocator, 128, 64);
      EXPECT_EQ(target.layout().stride, 128U);
      EXPECT_EQ(target.layout().allocation_size, 12'288U);
      EXPECT_TRUE(target.rga_buffer());
    }
    EXPECT_EQ(backend.events, (std::vector<std::string> {"release:1", "close:100"}));
    EXPECT_EQ(allocator.closed_fds, (std::vector<int> {100}));
  }

  TEST(RgaTarget, ReleasesAllocatedFdWhenImportFailsMidConstruction) {
    fake_backend_t backend;
    fake_allocator_t allocator;
    allocator.events = &backend.events;
    backend.fail_import = true;
    EXPECT_THROW(platf::rga::target_buffer_t::allocate_nv12(backend, allocator, 128, 64), platf::rga::error_t);
    EXPECT_EQ(allocator.closed_fds, (std::vector<int> {100}));
    EXPECT_EQ(backend.events, (std::vector<std::string> {"close:100"}));
  }

  TEST(RgaTarget, AllocatesPackedRgbaForExternalVulkanImport) {
    fake_backend_t backend;
    fake_allocator_t allocator;
    allocator.events = &backend.events;
    {
      auto target = platf::rga::target_buffer_t::allocate_rgba8888(backend, allocator, 320, 180);
      EXPECT_EQ(target.layout().format, platf::rga::pixel_format_e::rgba8888);
      EXPECT_EQ(target.layout().stride, 1280U);
      EXPECT_EQ(target.layout().allocation_size, 230'400U);
      EXPECT_TRUE(target.rga_buffer());
    }
    EXPECT_EQ(backend.events, (std::vector<std::string> {"release:1", "close:100"}));
  }

  TEST(RgaTarget, AllocatesPackedBgrForVulkanAndRgaSharing) {
    fake_backend_t backend;
    fake_allocator_t allocator;
    allocator.events = &backend.events;
    {
      auto target = platf::rga::target_buffer_t::allocate_bgr888(backend, allocator, 320, 180);
      EXPECT_EQ(target.layout().format, platf::rga::pixel_format_e::bgr888);
      EXPECT_EQ(target.layout().stride, 960U);
      EXPECT_EQ(target.layout().allocation_size, 172'800U);
      EXPECT_TRUE(target.rga_buffer());
    }
    EXPECT_EQ(backend.events, (std::vector<std::string> {"release:1", "close:100"}));
  }

  TEST(RgaTarget, RejectsOddDimensionsAndInvalidStrideBeforeAllocation) {
    fake_backend_t backend;
    fake_allocator_t allocator;
    EXPECT_THROW(platf::rga::target_buffer_t::allocate_nv12(backend, allocator, 127, 64), platf::rga::error_t);
    EXPECT_THROW(platf::rga::target_buffer_t::allocate_nv12(backend, allocator, 128, 64, 129), platf::rga::error_t);
    EXPECT_TRUE(allocator.allocation_sizes.empty());
  }

  TEST(RgaTarget, RejectsOversizedTargetBeforeCallingAllocator) {
    fake_backend_t backend;
    fake_allocator_t allocator;
    EXPECT_THROW(platf::rga::target_buffer_t::allocate_nv12(backend, allocator, 1'500'000'000U, 2), platf::rga::error_t);
    EXPECT_THROW(platf::rga::target_buffer_t::allocate_nv12(backend, allocator, static_cast<std::uint32_t>(std::numeric_limits<int>::max()) + 1U, 2), platf::rga::error_t);
    EXPECT_TRUE(allocator.allocation_sizes.empty());
  }

  TEST(RgaOperations, RunsConcreteImcheckBeforeEachSynchronousOperation) {
    fake_backend_t backend;
    auto source = platf::rga::imported_buffer_t::import(backend, nv12_layout(10));
    auto destination = platf::rga::imported_buffer_t::import(backend, nv12_layout(11));
    platf::rga::fill(destination, {0, 0, 128, 64}, 0x00800080U);
    platf::rga::process(source, {0, 0, 128, 64}, destination, {0, 0, 128, 64});
    platf::rga::resize(source, destination);
    platf::rga::color_convert(source, destination);
    EXPECT_EQ(backend.fill_checks, 1);
    EXPECT_EQ(backend.fills, 1);
    EXPECT_EQ(backend.process_checks, 1);
    EXPECT_EQ(backend.processes, 1);
    EXPECT_EQ(backend.resize_checks, 1);
    EXPECT_EQ(backend.resizes, 1);
    EXPECT_EQ(backend.color_checks, 1);
    EXPECT_EQ(backend.color_converts, 1);
  }

  TEST(RgaOperations, PreservesExplicitBt709LimitedProcessMode) {
    fake_backend_t backend;
    auto source = platf::rga::imported_buffer_t::import(backend, layout_for(platf::rga::pixel_format_e::rgba8888));
    auto destination = platf::rga::imported_buffer_t::import(backend, nv12_layout(11));
    platf::rga::process(source, {0, 0, 128, 64}, destination, {0, 0, 128, 64}, platf::rga::color_space_e::rgb_to_yuv_bt709_limited);
    EXPECT_EQ(backend.last_process_check_color_space, platf::rga::color_space_e::rgb_to_yuv_bt709_limited);
    EXPECT_EQ(backend.last_process_color_space, platf::rga::color_space_e::rgb_to_yuv_bt709_limited);
  }

  TEST(RgaOperations, CoversAdaptiveFourKUiPanelIntoNv12CaptureRoi) {
    fake_backend_t backend;
    const platf::rga::image_layout_t panel_layout {
      10,
      2'560,
      1'440,
      7'680,
      11'059'200,
      platf::rga::pixel_format_e::bgr888
    };
    const platf::rga::image_layout_t capture_layout {
      11,
      3'840,
      2'160,
      3'840,
      12'441'600,
      platf::rga::pixel_format_e::nv12
    };
    auto panel = platf::rga::imported_buffer_t::import(backend, panel_layout);
    auto capture = platf::rga::imported_buffer_t::import(backend, capture_layout);

    platf::rga::process(
      panel,
      {0, 0, 2'560, 1'440},
      capture,
      {640, 648, 2'560, 1'440},
      platf::rga::color_space_e::rgb_to_yuv_bt709_limited
    );

    EXPECT_EQ(backend.process_checks, 1);
    EXPECT_EQ(backend.processes, 1);
    EXPECT_EQ(backend.last_process_check_color_space, platf::rga::color_space_e::rgb_to_yuv_bt709_limited);
    EXPECT_EQ(backend.last_process_color_space, platf::rga::color_space_e::rgb_to_yuv_bt709_limited);
  }

  TEST(RgaOperations, ConvertsImcheckFailureToErrorAndSkipsOperation) {
    fake_backend_t backend;
    backend.process_check_status = {false, 9, "fake imcheck rejection"};
    auto source = platf::rga::imported_buffer_t::import(backend, nv12_layout(10));
    auto destination = platf::rga::imported_buffer_t::import(backend, nv12_layout(11));
    try {
      platf::rga::process(source, {0, 0, 128, 64}, destination, {0, 0, 128, 64});
      FAIL() << "expected imcheck failure";
    } catch (const platf::rga::error_t &error) {
      EXPECT_NE(std::string(error.what()).find("imcheck(process)"), std::string::npos);
      EXPECT_NE(std::string(error.what()).find("fake imcheck rejection"), std::string::npos);
    }
    EXPECT_EQ(backend.process_checks, 1);
    EXPECT_EQ(backend.processes, 0);
  }

  TEST(RgaOperations, ConvertsExecutionFailuresToErrorsAndStopsEachOperation) {
    fake_backend_t backend;
    auto source = platf::rga::imported_buffer_t::import(backend, nv12_layout(10));
    auto destination = platf::rga::imported_buffer_t::import(backend, nv12_layout(11));
    backend.fill_status = {false, 31, "fake fill execution failure"};
    try {
      platf::rga::fill(destination, {0, 0, 128, 64}, 0);
      FAIL() << "expected fill execution failure";
    } catch (const platf::rga::error_t &error) {
      EXPECT_NE(std::string(error.what()).find("RGA status=31"), std::string::npos);
      EXPECT_NE(std::string(error.what()).find("fake fill execution failure"), std::string::npos);
    }
    backend.process_status = {false, 32, "fake process execution failure"};
    EXPECT_THROW(platf::rga::process(source, {0, 0, 128, 64}, destination, {0, 0, 128, 64}), platf::rga::error_t);
    backend.resize_status = {false, 33, "fake resize execution failure"};
    EXPECT_THROW(platf::rga::resize(source, destination), platf::rga::error_t);
    backend.color_status = {false, 34, "fake color execution failure"};
    EXPECT_THROW(platf::rga::color_convert(source, destination), platf::rga::error_t);
    EXPECT_EQ(backend.fills, 1);
    EXPECT_EQ(backend.processes, 1);
    EXPECT_EQ(backend.resizes, 1);
    EXPECT_EQ(backend.color_converts, 1);
  }

  TEST(RgaOperations, RejectsCrossBackendOperationsBeforeCallingEitherBackend) {
    fake_backend_t source_backend;
    fake_backend_t destination_backend;
    auto source = platf::rga::imported_buffer_t::import(source_backend, nv12_layout(10));
    auto destination = platf::rga::imported_buffer_t::import(destination_backend, nv12_layout(11));
    EXPECT_THROW(platf::rga::resize(source, destination), platf::rga::error_t);
    EXPECT_THROW(platf::rga::process(source, {0, 0, 128, 64}, destination, {0, 0, 128, 64}), platf::rga::error_t);
    EXPECT_THROW(platf::rga::color_convert(source, destination), platf::rga::error_t);
    EXPECT_EQ(source_backend.resize_checks, 0);
    EXPECT_EQ(source_backend.resizes, 0);
    EXPECT_EQ(source_backend.process_checks, 0);
    EXPECT_EQ(source_backend.processes, 0);
    EXPECT_EQ(source_backend.color_checks, 0);
    EXPECT_EQ(source_backend.color_converts, 0);
    EXPECT_EQ(destination_backend.resize_checks, 0);
    EXPECT_EQ(destination_backend.resizes, 0);
    EXPECT_EQ(destination_backend.color_checks, 0);
    EXPECT_EQ(destination_backend.color_converts, 0);
  }

  TEST(RgaOperations, RejectsOutOfBoundsAndChromaMisalignedRectangles) {
    fake_backend_t backend;
    auto destination = platf::rga::imported_buffer_t::import(backend, nv12_layout());
    EXPECT_THROW(platf::rga::fill(destination, {0, 0, 129, 64}, 0), platf::rga::error_t);
    EXPECT_THROW(platf::rga::fill(destination, {1, 0, 126, 64}, 0), platf::rga::error_t);
    auto source = platf::rga::imported_buffer_t::import(backend, layout_for(platf::rga::pixel_format_e::rgba8888));
    EXPECT_THROW(platf::rga::process(source, {0, 0, 129, 64}, destination, {0, 0, 128, 64}), platf::rga::error_t);
    EXPECT_THROW(platf::rga::process(source, {0, 0, 128, 64}, destination, {1, 0, 126, 64}), platf::rga::error_t);
    EXPECT_EQ(backend.fill_checks, 0);
    EXPECT_EQ(backend.fills, 0);
    EXPECT_EQ(backend.process_checks, 0);
    EXPECT_EQ(backend.processes, 0);
  }

  TEST(RgaBuild, ReportsDisabledWhenTheRequiredSdkWasNotConfigured) {
#if defined(SUNSHINE_BUILD_RGA)
    SUCCEED() << "CMake checked and enabled the librga im2d API";
#else
    EXPECT_FALSE(platf::rga::is_compiled());
    EXPECT_THROW(platf::rga::make_backend(), std::runtime_error);
#endif
  }
}  // namespace
