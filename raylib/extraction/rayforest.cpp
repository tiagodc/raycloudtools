// Copyright (c) 2020
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Thomas Lowe
#include "rayforest.h"
#include "rayterrain.h"
#include "../rayconvexhull.h"
#include "../raymesh.h"
#include "../raycuboid.h"
#include "../rayply.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#define USE_GROWTH_RATIO

namespace ray
{
static const double wrap_gradient = 1.0;

bool Forest::findSpace(const Cluster &cluster, const std::vector<Eigen::Vector3d> &points, Eigen::Vector3d &tip)
{
  if (cluster.ids.empty())
  {
    tip = (cluster.min_bound + cluster.max_bound) / 2.0;
    tip[2] = cluster.max_bound[2];
    return true; // TODO: do we believe trunks absolutely? or should we double check against the free space?
  }
  if (cluster.trunk_id >= 0) // if this cluster is associated with a trunk, then use the trunk location, not the centroid
  {
    tip = trunks_[cluster.trunk_id].first - min_bounds_;
    tip[2] = cluster.max_bound[2];
    return true;
  }
  Eigen::Vector3d weighted_sum(0,0,0);
  double weight = 0.0;
  for (auto &i: cluster.ids)
  {
    weighted_sum += points[i][2] * points[i];
    weight += points[i][2];
  }
  tip = weighted_sum/weight;
  Eigen::Vector3d tip_local = tip/voxel_width_;
  const double search_down_gradient = 0.2;
  double radius = tip_local[2]*search_down_gradient;

  // now find the closest bit of space to put the tree in:
  int min_x = std::max(0, (int)(tip_local[0] - radius));
  int max_x = std::min((int)spacefield_.rows()-1, (int)(tip_local[0] + radius));
  int min_y = std::max(0, (int)(tip_local[1] - radius));
  int max_y = std::min((int)spacefield_.cols()-1, (int)(tip_local[1] + radius));
  double best_score = -1e10;
  int best_x = -1;
  int best_y = -1;
  for (int x = min_x; x<=max_x; x++)
  {
    for (int y = min_y; y<=max_y; y++)
    {
      double dist2 = sqr(((double)x-tip_local[0])/radius) + sqr(((double)y-tip_local[1])/radius);
      double score = spacefield_(x, y) - 0.25*dist2; // slight preference for result near the centroid
      if (score > best_score)
      {
        best_score = score;
        best_x = x;
        best_y = y;
      }
    }
  }
  if (best_x != -1) 
  {
    tip[0] = ((double)best_x+0.5)*voxel_width_;
    tip[1] = ((double)best_y+0.5)*voxel_width_;
    return true;
  }
  return false;
}

// extract the ray cloud canopy to a height field, then call the heightfield based forest extraction
std::vector<TreeSummary> Forest::extract(const std::string &cloud_name_stub, Mesh &mesh, const std::vector<std::pair<Eigen::Vector3d, double> > &trunks) 
{
  trunks_ = trunks;
  Cloud::Info info;
  if (!Cloud::getInfo(cloud_name_stub + ".ply", info))
    return std::vector<TreeSummary>();
  min_bounds_ = info.ends_bound.min_bound_;
  max_bounds_ = info.ends_bound.max_bound_;
  #if defined AGGLOMERATE
  double voxel_width = 0.25; // 6.0 * Cloud::estimatePointSpacing(cloud_name_stub, info.ends_bound, info.num_bounded);
  #else
  double voxel_width = 1.0;
  #endif
  std::cout << "voxel width: " << voxel_width << " m" << std::endl;

  double width = (max_bounds_[0] - min_bounds_[0])/voxel_width;
  double length = (max_bounds_[1] - min_bounds_[1])/voxel_width;
  Eigen::Vector2i grid_dims(ceil(width), ceil(length));
  std::cout << "dims for heightfield: " << grid_dims.transpose() << std::endl;
  Eigen::ArrayXXd highs = Eigen::ArrayXXd::Constant(grid_dims[0], grid_dims[1], -1e10);

  auto fillHeightField = [&](std::vector<Eigen::Vector3d> &, std::vector<Eigen::Vector3d> &ends, std::vector<double> &, std::vector<ray::RGBA> &colours)
  {
    for (size_t i = 0; i < ends.size(); i++)
    {
      if (colours[i].alpha == 0)
        continue;
      Eigen::Vector3d pos = (ends[i] - min_bounds_)/voxel_width;
      double &h = highs((int)pos[0], (int)pos[1]);
      h = std::max(h, ends[i][2]);
    }
  };
  if (!ray::Cloud::read(cloud_name_stub + ".ply", fillHeightField))
    return std::vector<TreeSummary>();

  Eigen::ArrayXXd lows;
  if (mesh.vertices().empty())
    lows = Eigen::ArrayXXd::Constant(highs.rows(), highs.cols(), min_bounds_[2]);
  else 
    mesh.toHeightField(lows, min_bounds_, max_bounds_, voxel_width);
  if (lows.rows() != highs.rows() || lows.cols() != highs.cols())
    std::cerr << "error: arrays are different widths" << std::endl;

  // generate grid
  Occupancy2D grid2D;
  if (!grid2D.load(cloud_name_stub + "_occupied.dat"))
  {
    grid2D.init(info.ends_bound.min_bound_, info.ends_bound.max_bound_, voxel_width);
    // walk the rays to fill densities
    grid2D.fillDensities(cloud_name_stub + ".ply", lows, 1.0, 1.5);
    grid2D.save(cloud_name_stub + "_occupied.dat");
  }
  if (grid2D.dims_[0] != lows.rows() || grid2D.dims_[1] != lows.cols())
    std::cerr << "error: arrays are different widths" << std::endl;
  Eigen::ArrayXXd space(grid2D.dims_[0], grid2D.dims_[1]);
  for (int i = 0; i<space.rows(); i++)
  {
    for (int j = 0; j<space.cols(); j++)
      space(i,j) = grid2D.pixel(Eigen::Vector3i(i, j, 0)).density();
  }

  return extract(highs, lows, space, voxel_width, cloud_name_stub);
}

std::vector<TreeSummary> Forest::extract(const Eigen::ArrayXXd &highs, const Eigen::ArrayXXd &lows, const Eigen::ArrayXXd &space, double voxel_width, const std::string &cloud_name_stub)
{
  voxel_width_ = voxel_width;
  heightfield_ = highs;
  lowfield_ = lows;
  spacefield_ = space;
  drawHeightField(cloud_name_stub + "_highfield.png", heightfield_);
  drawHeightField(cloud_name_stub + "_lowfield.png", lowfield_);

  #if defined AGGLOMERATE
  int count = 0;
  std::vector<Eigen::Vector3d> points;
  for (int x = 0; x < heightfield_.rows(); x++)
  {
    for (int y = 0; y < heightfield_.cols(); y++)
    {
      double &h = heightfield_(x, y);
      double l = lowfield_(x, y);
      if (h < l+undercroft_height)
      {
        h = -1e10;
        count++;
      }
      if (h > -1e10 && l > -1e10 && h>=l)
        points.push_back(Eigen::Vector3d(voxel_width_*((double)x + 0.5), voxel_width_*((double)y + 0.5), h-l)); // get heightfield relative to ground
    }
  }
  std::cout << "undercroft at height " << undercroft_height << " removed = " << count << " out of " << heightfield_.rows()*heightfield_.cols() << std::endl;

  const double max_diameter_per_height = 1.5; // 0.9 for bellbworie, 1.5 for T
  const double min_diameter_per_height = 0.15; // for T, 0.15 or 0.25 are about equal

  Terrain terrain;
  terrain.growDownwards(points, wrap_gradient);
  Mesh &mesh = terrain.mesh();

  std::cout << "num points " << mesh.vertices().size() << std::endl;
  mesh.reduce();
  std::cout << "num verts: " << mesh.vertices().size() << std::endl;


  // 2. cluster according to radius based on height of points
  std::vector<Cluster> point_clusters;
  agglomerate(mesh.vertices(), mesh.indexList(), min_diameter_per_height, max_diameter_per_height, point_clusters);
  std::cout << "number found: " << point_clusters.size() << std::endl;
  std::vector<Eigen::Vector3d> &verts = mesh.vertices();
  const double height_per_radius = 50.0; // TODO: temporary until we have a better parameter choice

  if (verbose)
    renderAgglomeration(point_clusters, verts);
  
  #else // watershed

  indexfield_ = Eigen::ArrayXXi::Constant(heightfield_.rows(), heightfield_.cols(), -1);
  // ignore the undercroft
  int count = 0;
  for (int x = 0; x < heightfield_.rows(); x++)
  {
    for (int y = 0; y < heightfield_.cols(); y++)
    {
      if (heightfield_(x, y) < lowfield_(x, y)+undercroft_height)
      {
        heightfield_(x, y) = -1e10;
        count++;
      }
    }
  }
  std::cout << "undercroft removed = " << count << " out of " << heightfield_.rows()*heightfield_.cols() << std::endl;
  std::vector<TreeNode> trees;
  std::set<int> heads;
  hierarchicalWatershed(trees, heads);

  std::cout << "number of raw candidates: " << trees.size() << " number largest size: " << heads.size() << std::endl;
  calculateTreeParaboloids(trees);
  drawSegmentation("segmented.png", trees);

  std::vector<int> indices;
  for (auto &head: heads)
  {
    searchTrees(trees, head, 1.0/tree_roundness, indices);
  }
  for (auto &ind: indices)
  {
    TreeSummary result;
    result.tree_tip = trees[ind].node.pixelMean(); // peak;//tip();
    result.tree_tip[2] = trees[ind].peak[2];
    int x = (int) result.tree_tip[0];
    int y = (int) result.tree_tip[1];
    if (x < 0 || x >= lowfield_.rows())
      continue;
    if (y < 0 || y >= lowfield_.cols())
      continue;
    result.ground_height = lowfield_(x, y);
    result.radius = trees[ind].approx_radius;
    result.curvature = trees[ind].node.curvature();
    results_.push_back(result);
  }

  drawTrees("result_trees.png", results_, (int)heightfield_.rows(), (int)heightfield_.cols());
    
  drawFinalSegmentation("result_tree_shapes.png", trees, indices);

  
  #endif

  int no_space_trees = 0;
  std::vector<TreeSummary> results;
  for (auto &cluster: point_clusters)
  {
    Eigen::Vector3d tip;
    if (findSpace(cluster, verts, tip))
    {
      TreeSummary tree;
      tree.base = min_bounds_ + tip;
      tree.base[2] = lowfield_(int(tip[0] / voxel_width_), int(tip[1] / voxel_width_));
      tree.height = tip[2];
      tree.trunk_identified = true;
      if (cluster.trunk_id >= 0)
        tree.radius = trunks_[cluster.trunk_id].second;
      else 
      {
        tree.radius = tree.height / height_per_radius;
        tree.trunk_identified = false;
      }
      results.push_back(tree);
    }
    else
    {
      no_space_trees++;
    }
  }
  std::sort(results.begin(), results.end(), [](const TreeSummary &a, const TreeSummary &b){ return a.height > b.height; });
  if (no_space_trees > 0)
  {
    std::cout << no_space_trees << " trees from _trunks.txt have rays passing through, so appear to be falsly identified as trees. Removing." << std::endl;
  }
  return results;
}


}
