/**
 * @file src/platform/linux/vulkan_ui_surface.h
 * @brief Bounded replacement of CMA-backed UI surfaces.
 */
#pragma once

#include <memory>

namespace platf::vulkan_ui {
  /**
   * @brief Replace a surface without overlapping old and new CMA allocations.
   *
   * Destroying the old surface also releases renderer imports before allocating
   * its replacement. If construction fails, recreate the previous layout and
   * rethrow the failure so the caller can retain its last working size metadata.
   *
   * @param surface The only owned page-family surface.
   * @param create Create the requested surface, cleaning up partial allocations on failure.
   * @param restore Recreate the previous surface, or return null on initial creation.
   */
  template<class Surface, class Create, class Restore>
  void replace_surface_without_overlap(std::unique_ptr<Surface> &surface, Create &&create, Restore &&restore) {
    surface.reset();
    try {
      surface = create();
    } catch (...) {
      surface = restore();
      throw;
    }
  }
}  // namespace platf::vulkan_ui
