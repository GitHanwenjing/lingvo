/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "lingvo/tasks/car/ops/ps_utils.h"

#include <algorithm>
#include <random>

#include <gtest/gtest.h>
#include "tensorflow/core/lib/strings/stringprintf.h"
#include "tensorflow/core/platform/test_benchmark.h"

namespace tensorflow {
namespace lingvo {
namespace car {

struct Point {
  float x;
  float y;
};

// Generates batch_size sets of points, the k-th example in the batch has n-k
// centers (i-th center is on (i, i)) and m points near each center.
void GeneratePoints(int batch_size, int n, int m, Tensor* points,
                    Tensor* points_padding) {
  std::mt19937 rng(39183);
  *points = Tensor(DT_FLOAT, {batch_size, n * m, 3});
  *points_padding = Tensor(DT_FLOAT, {batch_size, n * m});
  auto points_t = points->tensor<float, 3>();
  auto points_padding_t = points_padding->matrix<float>();
  points_padding_t.setConstant(0.0);

  for (int cur_batch = 0; cur_batch < batch_size; ++cur_batch) {
    std::vector<Point> points;
    for (int i = 0; i < n - cur_batch; ++i) {
      for (int j = 0; j < m; ++j) {
        float v = i + j / 1000.0;
        points.push_back(Point{v, v});
      }
    }
    std::shuffle(points.begin(), points.end(), rng);
    for (int i = 0; i < (n - cur_batch) * m; ++i) {
      points_t(cur_batch, i, 0) = points[i].x;
      points_t(cur_batch, i, 1) = points[i].y;
      points_t(cur_batch, i, 2) = 0;
    }
    for (int i = (n - cur_batch) * m; i < n * m; ++i) {
      points_padding_t(cur_batch, i) = 1.0;
    }
  }
}

void Log(const Tensor& points, const PSUtils::Result& result) {
  const int batch_size = result.center.dim_size(0);
  const int n = result.center.dim_size(1);
  CHECK_EQ(result.indices.dim_size(0), batch_size);
  CHECK_EQ(result.indices.dim_size(1), n);

  const int m = result.indices.dim_size(2);
  CHECK_EQ(result.indices_padding.dim_size(1), n);
  CHECK_EQ(result.indices_padding.dim_size(2), m);

  auto points_t = points.tensor<float, 3>();
  auto center_t = result.center.matrix<int32>();
  auto center_padding_t = result.center_padding.matrix<float>();
  auto indices_t = result.indices.tensor<int32, 3>();
  auto indices_padding_t = result.indices_padding.tensor<float, 3>();
  for (int cur_batch = 0; cur_batch < batch_size; ++cur_batch) {
    fprintf(stdout, "batch id %d\n", cur_batch);
    for (int i = 0; i < n; ++i) {
      CHECK_EQ(0.0, center_padding_t(cur_batch, i));
      fprintf(stdout,
              "(%5.3f %5.3f): ", points_t(cur_batch, center_t(cur_batch, i), 0),
              points_t(cur_batch, center_t(cur_batch, i), 1));
      for (int j = 0; j < m; ++j) {
        fprintf(stdout, "(%5.3f %5.3f)/%1.0f, ",
                points_t(cur_batch, indices_t(cur_batch, i, j), 0),
                points_t(cur_batch, indices_t(cur_batch, i, j), 1),
                indices_padding_t(cur_batch, i, j));
      }
      fprintf(stdout, "\n");
    }
  }
}

std::vector<int> GetCenters(const Tensor& points,
                            const PSUtils::Result& result) {
  const int batch_size = result.center.dim_size(0);
  const int n = result.center.dim_size(1);
  CHECK_EQ(result.indices.dim_size(0), batch_size);
  CHECK_EQ(result.indices.dim_size(1), n);

  const int m = result.indices.dim_size(2);
  CHECK_EQ(result.indices_padding.dim_size(1), n);
  CHECK_EQ(result.indices_padding.dim_size(2), m);

  auto points_t = points.tensor<float, 3>();
  auto center_t = result.center.matrix<int32>();
  auto center_padding_t = result.center_padding.matrix<float>();
  auto indices_t = result.indices.tensor<int32, 3>();
  std::vector<int> centers;
  for (int cur_batch = 0; cur_batch < batch_size; ++cur_batch) {
    for (int i = 0; i < n; ++i) {
      CHECK_EQ(0.0, center_padding_t(cur_batch, i));
      const int center =
          static_cast<int>(points_t(cur_batch, center_t(cur_batch, i), 0));
      for (int j = 0; j < m; ++j) {
        CHECK_EQ(points_t(cur_batch, indices_t(cur_batch, i, j), 0),
                 points_t(cur_batch, indices_t(cur_batch, i, j), 1));
        CHECK_EQ(center, static_cast<int>(points_t(
                             cur_batch, indices_t(cur_batch, i, j), 0)));
      }
      centers.push_back(center);
    }
  }
  return centers;
}

#if defined(PLATFORM_GOOGLE)
TEST(PSUtilsTest, Uniform_Uniform) {
  PSUtils::Options opts;
  opts.cmethod = PSUtils::Options::C_UNIFORM;
  opts.nmethod = PSUtils::Options::N_UNIFORM;
  opts.num_centers = 8;
  opts.num_neighbors = 16;
  opts.max_dist = 1.0;
  opts.random_seed = 12345;
  PSUtils fu(opts);
  Tensor points;
  Tensor points_padding;
  GeneratePoints(3, 8, 100, &points, &points_padding);
  auto ret = fu.Sample(points, points_padding);
  Log(points, ret);
  // Generated points will correspond to clusters like:
  // (0, 0), (0, 0.001), (0, 0.002), ...
  // (1, 1), (1, 1.001), (1, 1.002), ...
  // ...
  // (7, 7), (7, 7.001), (7, 7.002), ...
  //
  // GetCenters returns the first coordinate of each point, and hence, we
  // expect them to repeat.
  // Some clusters are sampled more than once.
  EXPECT_EQ(GetCenters(points, ret),
            std::vector<int>({1, 0, 2, 4, 0, 3, 1, 3,     // 1st example.
                              1, 2, 5, 3, 5, 2, 0, 0,     // 2nd example.
                              1, 2, 5, 2, 5, 4, 5, 4}));  // 3rd example.
}

TEST(PSUtilsTest, Uniform_Closest) {
  PSUtils::Options opts;
  opts.cmethod = PSUtils::Options::C_UNIFORM;
  opts.nmethod = PSUtils::Options::N_CLOSEST;
  opts.num_centers = 8;
  opts.num_neighbors = 16;
  opts.max_dist = 1.0;
  opts.random_seed = 12345;
  PSUtils fu(opts);
  Tensor points;
  Tensor points_padding;
  GeneratePoints(3, 8, 100, &points, &points_padding);
  auto ret = fu.Sample(points, points_padding);
  Log(points, ret);
  // Some clusters are sampled more than once.
  EXPECT_EQ(GetCenters(points, ret),
            std::vector<int>({1, 0, 2, 4, 0, 3, 1, 3,     // 1st example.
                              1, 2, 5, 3, 5, 2, 0, 0,     // 2nd example.
                              1, 2, 5, 2, 5, 4, 5, 4}));  // 3rd example.
}

TEST(PSUtilsTest, Farthest_Uniform) {
  PSUtils::Options opts;
  opts.cmethod = PSUtils::Options::C_FARTHEST;
  opts.nmethod = PSUtils::Options::N_UNIFORM;
  opts.num_centers = 8;
  opts.num_neighbors = 16;
  opts.max_dist = 1.0;
  opts.random_seed = 12345;
  PSUtils fu(opts);
  Tensor points;
  Tensor points_padding;
  GeneratePoints(3, 8, 100, &points, &points_padding);
  auto ret = fu.Sample(points, points_padding);
  Log(points, ret);
  // Generated points will correspond to clusters like:
  // (0, 0), (0, 0.001), (0, 0.002), ...
  // (1, 1), (1, 1.001), (1, 1.002), ...
  // ...
  // (7, 7), (7, 7.001), (7, 7.002), ...
  //
  // GetCenters returns the first coordinate of each point. With farthest point
  // sampling, for an example without any paddings, we expect the samples to
  // have all different first coordinates, and all 8 clusters should be covered.
  EXPECT_EQ(
      GetCenters(points, ret),
      std::vector<int>(
          {3, 7, 0, 5, 1, 6, 4, 2,     // 1st example.
           6, 0, 3, 1, 4, 5, 2, 0,     // 2nd example, last one is a duplicate.
           3, 0, 5, 1, 4, 2, 0, 1}));  // 3rd example, last two are duplicates.
}

TEST(PSUtilsTest, Farthest_Closest) {
  PSUtils::Options opts;
  opts.cmethod = PSUtils::Options::C_FARTHEST;
  opts.nmethod = PSUtils::Options::N_CLOSEST;
  opts.num_centers = 8;
  opts.num_neighbors = 16;
  opts.max_dist = 10.0;
  opts.random_seed = 12345;
  PSUtils fu(opts);
  Tensor points;
  Tensor points_padding;
  GeneratePoints(3, 8, 100, &points, &points_padding);
  auto ret = fu.Sample(points, points_padding);
  Log(points, ret);
  // All 8 clusters are covered.
  EXPECT_EQ(
      GetCenters(points, ret),
      std::vector<int>(
          {3, 7, 0, 5, 1, 6, 4, 2,     // 1st example.
           6, 0, 3, 1, 4, 5, 2, 0,     // 2nd example, last one is a duplicate.
           3, 0, 5, 1, 4, 2, 0, 1}));  // 3rd example, last two are duplicates.
}

void BM_Farthest(int iters, int num_centers, int num_neighbors) {
  testing::StopTiming();
  testing::SetLabel(strings::Printf("#Centers=%4d #Neighbors=%4d", num_centers,
                                    num_neighbors));
  PSUtils::Options opts;
  opts.cmethod = PSUtils::Options::C_FARTHEST;
  opts.nmethod = PSUtils::Options::N_UNIFORM;
  opts.num_centers = num_centers;
  opts.num_neighbors = num_neighbors;
  opts.max_dist = 1.0;
  opts.random_seed = -1;
  PSUtils fu(opts);
  Tensor points;
  Tensor points_padding;
  GeneratePoints(1, 1000, 100, &points, &points_padding);
  testing::StartTiming();
  for (int i = 0; i < iters; ++i) {
    auto ret = fu.Sample(points, points_padding);
  }
}

BENCHMARK(BM_Farthest)->RangePair(1, 1024, 1, 1024);
#endif

}  // namespace car
}  // namespace lingvo
}  // namespace tensorflow