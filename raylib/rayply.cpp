// Copyright (c) 2020
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Thomas Lowe
#include "rayply.h"
#include <iostream>

namespace ray
{
// Save the polygon file to disk
void writePly(const std::string &file_name, const std::vector<Eigen::Vector3d> &starts, const std::vector<Eigen::Vector3d> &ends,
                   const std::vector<double> &times, const std::vector<RGBA> &colours)
{
  std::cout << "saving to " << file_name << ", " << ends.size() << " rays." << std::endl;
  if (ends.size() == 0)
    std::cerr << "Error: save out ray file with zero rays" << std::endl;

  std::vector<RGBA> rgb(times.size());
  if (colours.size() > 0)
    rgb = colours;
  else
    colourByTime(times, rgb);

  std::vector<Eigen::Matrix<float, 9, 1>> vertices(ends.size());  // 4d to give space for colour
  bool warned = false;
  for (size_t i = 0; i < ends.size(); i++)
  {
    if (!warned)
    {
      if (!(ends[i] == ends[i]))
      {
        std::cout << "WARNING: nans in point: " << i << ": " << ends[i].transpose() << std::endl;
        warned = true;
      }
      if (abs(ends[i][0]) > 100000.0)
      {
        std::cout << "WARNING: very large point location at: " << i << ": " << ends[i].transpose() << ", suspicious" << std::endl;
        warned = true;
      }
      bool b = starts[i] == starts[i];
      if (!b)
      {
        std::cout << "WARNING: nans in start: " << i << ": " << starts[i].transpose() << std::endl;
        warned = true;
      }
    }
    Eigen::Vector3d n = starts[i] - ends[i];

    union U  // TODO: this is nasty, better to just make vertices an unsigned char vector
    {
      float f[2];
      double d;
    };
    U u;
    u.d = times[i];
    vertices[i] << (float)ends[i][0], (float)ends[i][1], (float)ends[i][2], (float)u.f[0], (float)u.f[1], (float)n[0],
      (float)n[1], (float)n[2], (float &)rgb[i];
  }
  std::ofstream out(file_name, std::ios::binary | std::ios::out);

  // do we put the starts in as normals?
  out << "ply" << std::endl;
  out << "format binary_little_endian 1.0" << std::endl;
  out << "comment generated by raycloudtools library" << std::endl;
  out << "element vertex " << vertices.size() << std::endl;
  out << "property float x" << std::endl;
  out << "property float y" << std::endl;
  out << "property float z" << std::endl;
  out << "property double time" << std::endl;
  out << "property float nx" << std::endl;
  out << "property float ny" << std::endl;
  out << "property float nz" << std::endl;
  out << "property uchar red" << std::endl;
  out << "property uchar green" << std::endl;
  out << "property uchar blue" << std::endl;
  out << "property uchar alpha" << std::endl;
  out << "end_header" << std::endl;

  out.write((const char *)&vertices[0], sizeof(Eigen::Matrix<float, 9, 1>) * vertices.size());
}

bool readPly(const std::string &file_name, std::vector<Eigen::Vector3d> &starts, std::vector<Eigen::Vector3d> &ends, std::vector<double> &times,
                  std::vector<RGBA> &colours, bool is_ray_cloud)
{
  std::cout << "reading: " << file_name << std::endl;
  std::ifstream input(file_name.c_str());
  if (!input.is_open())
  {
    std::cerr << "Couldn't open file: " << file_name << std::endl;
    return false;
  }
  std::string line;
  int row_size = 0;
  int offset = -1, normal_offset = -1, time_offset = -1, colour_offset = -1;
  int intensity_offset = -1;
  bool time_is_float = false;
  bool pos_is_float = false;
  bool normal_is_float = false;
  bool intensity_is_float = false;
  while (line != "end_header\r" && line != "end_header")
  {
    getline(input, line);
    if (line.find("property float x") != std::string::npos || line.find("property double x") != std::string::npos)
    {
      offset = row_size;
      if (line.find("float") != std::string::npos)
        pos_is_float = true;
    }
    if (line.find("property float nx") != std::string::npos || line.find("property double nx") != std::string::npos)
    {
      normal_offset = row_size;
      if (line.find("float") != std::string::npos)
        normal_is_float = true;
    }
    if (line.find("time") != std::string::npos)
    {
      time_offset = row_size;
      if (line.find("float") != std::string::npos)
        time_is_float = true;
    }
    if (line.find("intensity") != std::string::npos)
    {
      intensity_offset = row_size;
      if (line.find("float") != std::string::npos)
        intensity_is_float = true;
    }
    if (line.find("property uchar red") != std::string::npos)
      colour_offset = row_size;
    if (line.find("float") != std::string::npos)
      row_size += int(sizeof(float));
    if (line.find("double") != std::string::npos)
      row_size += int(sizeof(double));
    if (line.find("property uchar") != std::string::npos)
      row_size += int(sizeof(unsigned char));
  }
  if (offset == -1)
  {
    std::cerr << "could not find position properties of file: " << file_name << std::endl;
    return false;
  }
  if (is_ray_cloud && normal_offset == -1)
  {
    std::cerr << "could not find normal properties of file: " << file_name << std::endl;
    std::cerr << "ray clouds store the ray starts using the normal field" << std::endl;
    return false;
  }

  std::streampos start = input.tellg();
  input.seekg(0, input.end);
  size_t length = input.tellg() - start;
  input.seekg(start);
  size_t size = length / row_size;
  std::vector<unsigned char> vertices(row_size);
  bool times_need_sorting = false;
  size_t num_bounded = 0;
  size_t num_unbounded = 0;
  bool warning_set = false;
  if (size == 0)
  {
    std::cerr << "no entries found in ply file" << std::endl;
    return false;
  }

  // pre-reserving avoids memory fragmentation
  ends.reserve(size);
  starts.reserve(size);
  if (time_offset != -1)
    times.reserve(size);
  if (colour_offset != -1)
    colours.reserve(size);
  std::vector<uint8_t> intensities;
  if (intensity_offset != -1)
    intensities.resize(size);
  for (size_t i = 0; i < size; i++)
  {
    input.read((char *)&vertices[0], row_size);
    Eigen::Vector3d end;
    if (pos_is_float)
    {
      Eigen::Vector3f e = (Eigen::Vector3f &)vertices[offset];
      end = Eigen::Vector3d(e[0], e[1], e[2]);
    }
    else
      end = (Eigen::Vector3d &)vertices[offset];
    bool end_valid = end == end;
    if (!warning_set)
    {
      if (!end_valid)
      {
        std::cout << "warning, NANs in point " << i << ", removing all NANs." << std::endl;
        warning_set = true;
      }
      if (abs(end[0]) > 100000.0)
      {
        std::cout << "warning: very large data in point " << i << ", suspicious: " << end.transpose() << std::endl;
        warning_set = true;
      }
    }
    if (!end_valid)
      continue;

    Eigen::Vector3d normal(0,0,0);
    if (is_ray_cloud)
    {
      if (normal_is_float)
      {
        Eigen::Vector3f n = (Eigen::Vector3f &)vertices[normal_offset];
        normal = Eigen::Vector3d(n[0], n[1], n[2]);
      }
      else
        normal = (Eigen::Vector3d &)vertices[normal_offset];
      bool norm_valid = normal == normal;
      if (!warning_set)
      {
        if (!norm_valid)
        {
          std::cout << "warning, NANs in raystart stored in normal " << i << ", removing all such rays." << std::endl;
          warning_set = true;
        }
        if (abs(normal[0]) > 100000.0)
        {
          std::cout << "warning: very large data in normal " << i << ", suspicious: " << normal.transpose() << std::endl;
          warning_set = true;
        }
      }
      if (!norm_valid)
        continue;
    }

    ends.push_back(end);
    if (time_offset != -1)
    {
      double time;
      if (time_is_float)
        time = (double)((float &)vertices[time_offset]);
      else
        time = (double &)vertices[time_offset];
      if (times.size() > 0 && times.back() > time)
        times_need_sorting = true;
      times.push_back(time);
    }
    starts.push_back(end + normal);

    if (colour_offset != -1)
    {
      RGBA colour = (RGBA &)vertices[colour_offset];
      colours.push_back(colour);
      if (colour.alpha > 0)
        num_bounded++;
      else
        num_unbounded++;
    }
    if (!is_ray_cloud)
    {
      if (intensity_offset != -1)
      {
        double intensity;
        const double maximum_intensity = 100.0;
        if (intensity_is_float)
          intensity = (double)((float &)vertices[intensity_offset]);
        else
          intensity = (double &)vertices[intensity_offset];
        intensities[i] = uint8_t(255.0 * clamped(intensity / maximum_intensity, 0.0, 1.0));
      }
    }
  }
  if (times.size() == 0)
  {
    std::cout << "warning: no time information found in " << file_name << ", setting times at 1 second intervals per ray"
         << std::endl;
    times.resize(ends.size());
    for (size_t i = 0; i < times.size(); i++) 
      times[i] = (double)i;
  }
  if (colours.size() == 0)
  {
    std::cout << "warning: no colour information found in " << file_name
         << ", setting colours red->green->blue based on time" << std::endl;
    colourByTime(times, colours);
    num_bounded = ends.size();
  }
  if (!is_ray_cloud && intensity_offset != -1)
  {
    if (colour_offset != -1)
      std::cout << "warning: intensity and colour information found in file. Replacing alpha with intensity value." << std::endl;
    else
      std::cout << "intensity information found in file, storing this in the ray cloud alpha channel. Potential precision loss." << std::endl;
    for (size_t i = 0; i<size; i++)
      colours[i].alpha = intensities[i];
  }
  std::cout << "reading from " << file_name << ", " << size << " rays, of which " << num_bounded << " bounded and "
       << num_unbounded << " unbounded" << std::endl;
  if (num_bounded == 0)
  {
    std::cout << "ERROR: no ray end locations. This is a degenerate case, most functions will not be able to operate on this ray cloud" << std::endl;
    return false;
  }
  if (times_need_sorting)
  {
    std::cout << "warning, ray times are not in order. This is required, so sorting rays now." << std::endl;
    struct Temp
    {
      double time;
      size_t index;
    };
    std::vector<Temp> time_list(times.size());
    for (size_t i = 0; i < time_list.size(); i++)
    {
      time_list[i].time = times[i];
      time_list[i].index = i;
    }
    // now how can I sort in-place, without wasting memory?
    // one idea is to do it one variable at a time!
    sort(time_list.begin(), time_list.end(), [](const Temp &a, const Temp &b) { return a.time < b.time; });
    std::vector<Eigen::Vector3d> new_starts(starts.size());
    for (size_t i = 0; i < starts.size(); i++)
      new_starts[i] = starts[time_list[i].index];
    starts = new_starts;  
    new_starts.clear();
    new_starts.shrink_to_fit(); // deallocate
    std::vector<Eigen::Vector3d> new_ends(ends.size());
    for (size_t i = 0; i < ends.size(); i++)
      new_ends[i] = ends[time_list[i].index];
    ends = new_ends;
    new_ends.clear();
    new_ends.shrink_to_fit();
    std::vector<double> new_times(times.size());
    for (size_t i = 0; i < times.size(); i++)
      new_times[i] = time_list[i].time;
    times = new_times;
    new_times.clear();
    new_times.shrink_to_fit();
    if (colour_offset != -1)
    {
      std::vector<RGBA> new_colours(colours.size());
      for (size_t i = 0; i < colours.size(); i++)
        new_colours[i] = colours[time_list[i].index];
      colours = new_colours;
    }
    std::cout << "finished sorting" << std::endl;
  }
  return true;
}

void writePlyMesh(const std::string &file_name, const Mesh &mesh, bool flip_normals)
{
  std::cout << "saving to " << file_name << ", " << mesh.vertices().size() << " vertices." << std::endl;

  std::vector<Eigen::Vector4f> vertices(mesh.vertices().size());  // 4d to give space for colour
  for (size_t i = 0; i < mesh.vertices().size(); i++)
    vertices[i] << (float)mesh.vertices()[i][0], (float)mesh.vertices()[i][1], (float)mesh.vertices()[i][2], 1.0;


  FILE *fid = fopen(file_name.c_str(), "w+");
  fprintf(fid, "ply\n");
  fprintf(fid, "format binary_little_endian 1.0\n");
  fprintf(fid, "comment SDK generated\n");  // TODO: add version here
  fprintf(fid, "element vertex %u\n", unsigned(vertices.size()));
  fprintf(fid, "property float x\n");
  fprintf(fid, "property float y\n");
  fprintf(fid, "property float z\n");
  fprintf(fid, "property uchar red\n");
  fprintf(fid, "property uchar green\n");
  fprintf(fid, "property uchar blue\n");
  fprintf(fid, "property uchar alpha\n");
  fprintf(fid, "element face %u\n", (unsigned)mesh.index_list().size());
  fprintf(fid, "property list int int vertex_indices\n");
  fprintf(fid, "end_header\n");

  fwrite(&vertices[0], sizeof(Eigen::Vector4f), vertices.size(), fid);

  std::vector<Eigen::Vector4i> triangles(mesh.index_list().size());
  auto &list = mesh.index_list();
  if (flip_normals)
    for (size_t i = 0; i < list.size(); i++)
      triangles[i] = Eigen::Vector4i(3, list[i][2], list[i][1], list[i][0]);
  else
    for (size_t i = 0; i < list.size(); i++)
      triangles[i] = Eigen::Vector4i(3, list[i][0], list[i][1], list[i][2]);
  fwrite(&triangles[0], sizeof(Eigen::Vector4i), triangles.size(), fid);
  fclose(fid);

  Eigen::Array<double, 6, 1> mom = mesh.getMoments();

  std::cout << "stats: " << std::endl;
  for (int i = 0; i<mom.rows(); i++)
    std::cout << ", " << mom[i];
  std::cout << std::endl;
}


bool readPlyMesh(const std::string &file, Mesh &mesh)
{
  std::ifstream input(file.c_str());
  if (!input.is_open())
  {
    std::cerr << "Couldn't open file: " << file << std::endl;
    return false;
  }
  std::string line;
  unsigned row_size = 0;
  unsigned number_of_faces = 0;
  unsigned number_of_vertices = 0;
  char char1[100], char2[100];
  while (line != "end_header\r" && line != "end_header")
  {
    getline(input, line);
    if (line.find("float") != std::string::npos)
    {
      row_size += 4;
    }
    if (line.find("property uchar") != std::string::npos)
    {
      row_size++;
    }
    if (line.find("element vertex") != std::string::npos)
      sscanf(line.c_str(), "%s %s %u", char1, char2, &number_of_vertices);
    if (line.find("element face") != std::string::npos)
      sscanf(line.c_str(), "%s %s %u", char1, char2, &number_of_faces);
  }

  std::vector<Eigen::Vector4f> vertices(number_of_vertices);
  // read data as a block:
  input.read((char *)&vertices[0], sizeof(Eigen::Vector4f) * vertices.size());
  std::vector<Eigen::Vector4i> triangles(number_of_faces);
  input.read((char *)&triangles[0], sizeof(Eigen::Vector4i) * triangles.size());

  mesh.vertices().resize(vertices.size());
  for (int i = 0; i < (int)vertices.size(); i++)
    mesh.vertices()[i] = Eigen::Vector3d(vertices[i][0], vertices[i][1], vertices[i][2]);

  mesh.index_list().resize(triangles.size());
  for (int i = 0; i < (int)triangles.size(); i++)
    mesh.index_list()[i] = Eigen::Vector3i(triangles[i][1], triangles[i][2], triangles[i][3]);
  std::cout << "reading from " << file << ", " << mesh.index_list().size() << " triangles." << std::endl;
  return true;
}
} // ray
