// crop-preparator skeleton
// Consumes ObjectEnvelope (with or without crop_jpeg), produces improved /
// multi-ROI crops, and can re-publish an enriched ObjectEnvelope or side-channel
// PreparedCrop messages. For now: OpenCV-only, no avplumber.

#include "crop_preparator.hpp"
#include "common/env.hpp"
#include "common/otel.hpp"

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>
#include <opencv2/imgcodecs.hpp>
#include <google/protobuf/util/time_util.h>

#include "ingest/v1/ingest_service.grpc.pb.h"
#include "detection/v1/detection.pb.h"

#include <atomic>
#include <csignal>
#include <iostream>
#include <nats.h>  // if we subscribe to NATS; for skeleton we can also accept file input

// Skeleton demo: read a JPEG + bbox from args and print prepared crops.
// Real deployment: subscribe to cv.object.* (NATS) or gRPC stream of ObjectEnvelope.

namespace {
std::atomic<bool> g_running{true};
void on_signal(int) { g_running = false; }
}  // namespace

int main(int argc, char** argv) {
  edge::otel::init("crop-preparator", "0.1.0");
  spdlog::set_level(spdlog::level::info);

  if (argc < 6) {
    std::cerr << "Usage (demo): " << argv[0]
              << " <image.jpg> <x1> <y1> <x2> <y2> [class_name]\n"
              << "  Real mode will subscribe to ObjectEnvelopes.\n";
    return 1;
  }

  const std::string path = argv[1];
  float x1 = std::stof(argv[2]);
  float y1 = std::stof(argv[3]);
  float x2 = std::stof(argv[4]);
  float y2 = std::stof(argv[5]);
  std::string cls = argc > 6 ? argv[6] : "car";

  cv::Mat img = cv::imread(path);
  if (img.empty()) {
    spdlog::error("failed to load {}", path);
    return 1;
  }

  crop::Config cfg;
  cfg.pad_px = 12;
  cfg.make_plate_roi = true;

  auto prepared = crop::prepare(img, x1, y1, x2, y2, cls, cfg);
  spdlog::info("produced {} crop(s) for class={}", prepared.size(), cls);
  for (const auto& c : prepared) {
    auto jpeg = crop::to_jpeg(c, cfg.jpeg_quality);
    spdlog::info("  role={}  size={}x{}  jpeg_bytes={}",
                 c.role, c.image.cols, c.image.rows,
                 jpeg ? jpeg->size() : 0);
  }

  // TODO: wire NATS / gRPC consumer of ObjectEnvelope and re-publish
  // enriched envelopes or separate PreparedCrop messages for specialists.

  return 0;
}
