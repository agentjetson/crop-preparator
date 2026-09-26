#pragma once

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include <string>
#include <vector>
#include <optional>

namespace crop {

struct PreparedCrop {
  std::string role;          // "full", "plate_roi", "face_roi", ...
  cv::Mat     image;         // BGR
  float       x1{0}, y1{0}, x2{0}, y2{0};  // in original frame coords
};

struct Config {
  int  jpeg_quality{85};
  int  pad_px{12};
  int  target_size{0};       // 0 = keep original crop size; else square resize
  bool make_plate_roi{true}; // heuristic plate ROI for vehicles
  bool make_face_roi{false}; // future
};

// Produce one or more prepared crops from an object bbox + original frame
// (or from an existing crop_jpeg).
std::vector<PreparedCrop> prepare(
    const cv::Mat& frame_or_crop,
    float box_x1, float box_y1, float box_x2, float box_y2,
    const std::string& class_name,
    const Config& cfg);

// Encode a PreparedCrop to JPEG bytes
std::optional<std::vector<uint8_t>> to_jpeg(const PreparedCrop& c, int quality);

}  // namespace crop
