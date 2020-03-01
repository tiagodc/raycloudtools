#pragma once
#include "rayutils.h"
#include "raypose.h"
#include "raytrajectory.h"

namespace RAY
{

struct Cloud
{
  vector<Eigen::Vector3d> starts;
  vector<Eigen::Vector3d> ends; 
  vector<double> times;
  vector<double> intensities;

  void save(const std::string &fileName);
  bool load(const std::string &fileName);

  void transform(const Pose &pose, double timeDelta);
  
protected:  
  enum CloudType
  {
    CT_RayCloudPLY,
    CT_LAZandTrajFile,
  } cloudType;
  void calculateStarts(const Trajectory &trajectory);
  bool loadPLY(const string &file);
  bool loadLazTraj(const string &lazFile, const string &trajFile);
};


}