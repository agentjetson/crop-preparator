#include "crop_preparator.hpp"

#include <algorithm>
#include <cmath>

namespace crop {

static cv::Rect clamp_rect(int x1, int y1, int x2, int y2, int w, int h) {
  x1 = std::max(0, x1);
  y1 = std::max(0, y1);
  x2 = std::min(w, x2);
  y2 = std::min(h, y2);
  if (x2 <= x1 || y2 <= y1) return {};
  return {x1, y1, x2 - x1, y2 - y1};
}

// Heuristic plate ROI relative to a vehicle crop (normalized)
static cv::Rect plate_roi_in_crop(int w, int h) {
  int px = static_cast<int>(w * 0.22f);
  int py = static_cast<int>(h * 0.60f);
  int pw = static_cast<int>(w * 0.56f);
  int ph = static_cast<int>(h * 0.22f);
  return clamp_rect(px, py, px + pw, py + ph, w, h);
}

std::vector<PreparedCrop> prepare(
    const cv::Mat& frame_or_crop,
    float box_x1, float box_y1, float box_x2, float box_y2,
    const std::string& class_name,
    const Config& cfg) {

  std::vector<PreparedCrop> out;
  if (frame_or_crop.empty()) return out;

  const int W = frame_or_crop.cols;
  const int H = frame_or_crop.rows;

  // Primary full-object crop with padding
  int x1 = static_cast<int>(box_x1) - cfg.pad_px;
  int y1 = static_cast<int>(box_y1) - cfg.pad_px;
  int x2 = static_cast<int>(box_x2) + cfg.pad_px;
  int y2 = static_cast<int>(box_y2) + cfg.pad_px;
  auto full_rect = clamp_rect(x1, y1, x2, y2, W, H);
  if (full_rect.empty()) return out;

  PreparedCrop full;
  full.role = "full";
  full.image = frame_or_crop(full_rect).clone();
  full.x1 = static_cast<float>(full_rect.x);
  full.y1 = static_cast<float>(full_rect.y);
  full.x2 = static_cast<float>(full_rect.x + full_rect.width);
  full.y2 = static_cast<float>(full_rect.y + full_rect.height);

  if (cfg.target_size > 0) {
    cv::Mat resized;
    cv::resize(full.image, resized, {cfg.target_size, cfg.target_size});
    full.image = std::move(resized);
  }
  out.push_back(std::move(full));

  // Secondary: plate ROI for vehicles
  const bool is_vehicle =
      class_name == "car" || class_name == "truck" || class_name == "bus" ||
      class_name == "motorcycle" || class_name == "vehicle";
  if (cfg.make_plate_roi && is_vehicle) {
    auto roi = plate_roi_in_crop(full_rect.width, full_rect.height);
    if (!roi.empty()) {
      PreparedCrop plate;
      plate.role = "plate_roi";
      // Coordinates relative to original frame
      plate.x1 = static_cast<float>(full_rect.x + roi.x);
      plate.y1 = static_cast<float>(full_rect.y + roi.y);
      plate.x2 = plate.x1 + roi.width;
      plate.y2 = plate.y1 + roi.height;
      plate.image = frame_or_crop(cv::Rect(
          static_cast<int>(plate.x1), static_cast<int>(plate.y1),
          roi.width, roi.height)).clone();
      if (cfg.target_size > 0) {
        // Keep aspect for plate; just ensure min side
        // (left as-is for skeleton)
      }
      out.push_back(std::move(plate));
    }
  }

  return out;
}

std::optional<std::vector<uint8_t>> to_jpeg(const PreparedCrop& c, int quality) {
  if (c.image.empty()) return std::nullopt;
  std::vector<uint8_t> buf;
  std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, quality};
  if (!cv::imencode(".jpg", c.image, buf, params)) return std::nullopt;
  return buf;
}

}  // namespace crop
