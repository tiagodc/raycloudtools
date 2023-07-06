// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Kazys Stepanas, Tom Lowe
#ifndef RAYMERGER_H
#define RAYMERGER_H

#include "raylib/raylibconfig.h"

#include "raycloud.h"
#include "rayellipsoid.h"
#include "raygrid.h"

#include <atomic>
#include <limits>
#include <vector>

namespace ray
{
class Cloud;
class Progress;

/// Mode selection for @c Merger
enum class RAYLIB_EXPORT MergeType : int
{
  /// Preserve oldest samples.
  Oldest,
  /// Preserve newest samples.
  Newest,
  Mininum,
  Maximum,
  Order,  // in file priority order, first to last
  All
};

/// Parameter configuration structure for @c Merger
struct RAYLIB_EXPORT MergerConfig
{
  double voxel_size = 0;  ///< Ray grid voxel size. Use zero use an estimated voxel size.
  double num_rays_filter_threshold = 20;
  MergeType merge_type = MergeType::Mininum;
  bool colour_cloud = true;
};

/// A cloud merger which supports filtering 'transient' rays and merging from a ray clouds. A transient ray is one which
/// is in conflict with sample observations and rays passing through the observation. For example, transient points are
/// generated by movable objects in a ray cloud such as people moving through a scan or doors being openned and closed.
class RAYLIB_EXPORT Merger
{
public:
#if RAYLIB_WITH_TBB
  using Bool = std::atomic_bool;
#else   // RAYLIB_WITH_TBB
  using Bool = bool;
#endif  // RAYLIB_WITH_TBB

  Merger(const MergerConfig &config);
  ~Merger();

  inline const MergerConfig &config() const { return config_; }

  /// Query the removed ray results. Empty before @c filter() is called.
  inline const Cloud &differenceCloud() const { return difference_; }
  /// Query the preserved ray results. Empty before @c filter() is called.
  inline const Cloud &fixedCloud() const { return fixed_; }

  /// Perform the transient filtering on the given @p cloud .
  bool filter(const Cloud &cloud, Progress *progress = nullptr);

  /// Multi-merge
  bool mergeMultiple(std::vector<Cloud> &clouds, Progress *progress = nullptr);

  /// Three way merger
  bool mergeThreeWay(const Cloud &base_cloud, Cloud &cloud1, Cloud &cloud2, Progress *progress = nullptr);

  /// Reset previous results. Memory is retained.
  void clear();

  /// Fill a @p grid with with rays from @p cloud . For each ray we add its index to each grid cell it traces through.
  ///
  /// The grid bounds must be set sufficiently large to hold the rays before calling. The grid resolution is also set
  /// before calling
  ///
  /// @param grid The grid to populate
  /// @param cloud The cloud which grid indices reference rays in.
  /// @param progress Optional progress tracker.
  /// @todo This needs a more global home
  static void fillRayGrid(Grid<unsigned> *grid, const Cloud &cloud, Progress *progress, bool store_only_occupied_voxels = false);

private:
  double voxelSizeForCloud(const Cloud &cloud) const;

  /// For all ellipsoids_ intersect with rays in @c cloud (accelerated using @c ray_grid)
  /// depending on config.merge_type, either mark the ellipsoid object as removed, or
  /// mark the ray (through @c transient_ray_marks) as removed.
  /// @c ellipsoid_cloud_first is used only for the 'order' merge type, to choose which to mark
  void markIntersectedEllipsoids(const Cloud &cloud, const Grid<unsigned> &ray_grid,
                                 std::vector<Bool> *transient_ray_marks, double num_rays, bool self_transient,
                                 Progress *progress, bool ellipsoid_cloud_first = false);

  /// Finalise the cloud filter and populate @c transientResults() and @c fixedResults() .
  void finaliseFilter(const Cloud &cloud, const std::vector<Bool> &transient_ray_marks);

  Cloud difference_;
  Cloud fixed_;
  MergerConfig config_;
  std::vector<Ellipsoid> ellipsoids_;
};
}  // namespace ray

#endif  // RAYMERGER_H
