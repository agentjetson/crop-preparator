// crop-preparator — production NATS path
//
// Sits between object-classifier and specialists (alpr-consumer, …):
//
//   object-classifier  →  cv.object.<class>  →  crop-preparator
//                                                   │
//                         enriched ObjectEnvelope ──┬──▶ cv.object.<class>
//                         optional PreparedCrop ────└──▶ cv.crop.<kind>
//                                                   │
//                                                   ▼
//                                            specialists
//
// Loop guard: envelopes already marked labels["crop_prepared"]="1" are skipped.
// Prefer working from envelope.crop_jpeg when present (no full-frame bus needed).

#include "crop_preparator.hpp"
#include "common/env.hpp"
#include "common/otel.hpp"

#include <nats.h>
#include <spdlog/spdlog.h>
#include <opencv2/imgcodecs.hpp>
#include <google/protobuf/util/time_util.h>

#include "detection/v1/detection.pb.h"
#include "crop/v1/crop.pb.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_running{true};
void on_signal(int) { g_running = false; }

bool already_prepared(const detection::v1::ObjectEnvelope& obj) {
  auto it = obj.labels().find("crop_prepared");
  return it != obj.labels().end() && it->second == "1";
}

// Decode crop_jpeg → BGR Mat. Returns empty on failure / missing.
cv::Mat decode_crop(const detection::v1::ObjectEnvelope& obj) {
  if (obj.crop_jpeg().empty()) return {};
  const auto& bytes = obj.crop_jpeg();
  std::vector<uint8_t> buf(bytes.begin(), bytes.end());
  return cv::imdecode(buf, cv::IMREAD_COLOR);
}

// Build enriched ObjectEnvelope: replace crop_jpeg with the "full" prepared
// crop, stamp crop_prepared + crop role labels, keep identity fields.
detection::v1::ObjectEnvelope enrich_envelope(
    const detection::v1::ObjectEnvelope& in,
    const std::vector<crop::PreparedCrop>& prepared,
    const crop::Config& cfg) {
  detection::v1::ObjectEnvelope out = in;

  // Prefer the "full" role for the primary crop_jpeg field.
  const crop::PreparedCrop* full = nullptr;
  for (const auto& c : prepared) {
    if (c.role == "full") {
      full = &c;
      break;
    }
  }
  if (!full && !prepared.empty()) full = &prepared.front();

  if (full) {
    if (auto jpeg = crop::to_jpeg(*full, cfg.jpeg_quality)) {
      out.set_crop_jpeg(jpeg->data(), static_cast<int>(jpeg->size()));
    }
    // Box stays the original object box (specialists use it for correlation).
  }

  (*out.mutable_labels())["crop_prepared"] = "1";
  for (const auto& c : prepared) {
    (*out.mutable_labels())["crop_role_" + c.role] = "1";
  }
  return out;
}

crop::v1::PreparedCrop to_proto_crop(
    const detection::v1::ObjectEnvelope& src,
    const crop::PreparedCrop& c,
    const crop::Config& cfg) {
  crop::v1::PreparedCrop msg;
  msg.set_frame_id(src.frame_id());
  if (src.timestamp().seconds() != 0 || src.timestamp().nanos() != 0) {
    *msg.mutable_timestamp() = src.timestamp();
  } else {
    *msg.mutable_timestamp() =
        google::protobuf::util::TimeUtil::GetCurrentTime();
  }
  msg.set_source(src.source());
  msg.set_track_id(src.track_id());
  msg.set_class_name(src.class_name());
  msg.set_crop_kind(c.role);
  auto* box = msg.mutable_box();
  box->set_x1(c.x1);
  box->set_y1(c.y1);
  box->set_x2(c.x2);
  box->set_y2(c.y2);
  msg.set_width(c.image.cols);
  msg.set_height(c.image.rows);
  if (auto jpeg = crop::to_jpeg(c, cfg.jpeg_quality)) {
    msg.set_jpeg(jpeg->data(), static_cast<int>(jpeg->size()));
  }
  for (const auto& [k, v] : src.labels()) {
    (*msg.mutable_labels())[k] = v;
  }
  (*msg.mutable_labels())["crop_prepared"] = "1";
  return msg;
}

// JetStream publish (durable subjects on CV_EVENTS: cv.object.*, cv.result.*, …)
bool publish_js(jsCtx* js, const std::string& subject,
                const std::string& payload) {
  jsPubOptions popts;
  jsPubOptions_Init(&popts);
  jsPubAck* ack = nullptr;
  jsErrCode jerr{};
  natsStatus ps = js_Publish(&ack, js, subject.c_str(), payload.data(),
                             static_cast<int>(payload.size()), &popts, &jerr);
  if (ack) jsPubAck_Destroy(ack);
  if (ps != NATS_OK) {
    spdlog::warn("js publish {} failed: {} (jerr={})", subject,
                 natsStatus_GetText(ps), static_cast<int>(jerr));
    return false;
  }
  return true;
}

// Core NATS publish for hot-path side-channel (cv.crop.* is not on CV_EVENTS).
bool publish_core(natsConnection* nc, const std::string& subject,
                  const std::string& payload) {
  natsStatus ps = natsConnection_Publish(
      nc, subject.c_str(), payload.data(), static_cast<int>(payload.size()));
  if (ps != NATS_OK) {
    spdlog::warn("core publish {} failed: {}", subject, natsStatus_GetText(ps));
    return false;
  }
  return true;
}

int run_demo(int argc, char** argv) {
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
    spdlog::info("  role={}  size={}x{}  jpeg_bytes={}", c.role, c.image.cols,
                 c.image.rows, jpeg ? jpeg->size() : 0);
  }
  return 0;
}

int run_nats() {
  const std::string nats_url =
      edge::getenv_or("NATS_URL", "nats://localhost:4222");
  const bool emit_side_channel =
      edge::getenv_or("CROP_SIDE_CHANNEL", "1") == "1";
  const bool republish_envelope =
      edge::getenv_or("CROP_REPUBLISH_ENVELOPE", "1") == "1";

  crop::Config cfg;
  cfg.pad_px =
      std::stoi(edge::getenv_or("CROP_PAD_PX", "12"));
  cfg.jpeg_quality =
      std::stoi(edge::getenv_or("CROP_JPEG_QUALITY", "85"));
  cfg.make_plate_roi =
      edge::getenv_or("CROP_MAKE_PLATE_ROI", "1") == "1";
  cfg.target_size =
      std::stoi(edge::getenv_or("CROP_TARGET_SIZE", "0"));

  constexpr const char* kStream = "CV_EVENTS";
  constexpr const char* kConsumer = "crop-prep-worker";
  constexpr const char* kFilter = "cv.object.*";

  natsConnection* nc = nullptr;
  natsOptions* opts = nullptr;
  natsOptions_Create(&opts);
  natsOptions_SetURL(opts, nats_url.c_str());
  natsOptions_SetName(opts, "crop-preparator");
  natsOptions_SetMaxReconnect(opts, -1);
  natsOptions_SetReconnectWait(opts, 2000);

  natsStatus s = natsConnection_Connect(&nc, opts);
  natsOptions_Destroy(opts);
  if (s != NATS_OK) {
    spdlog::error("nats.Connect: {}", natsStatus_GetText(s));
    return 1;
  }

  jsCtx* js = nullptr;
  jsOptions jsOpts;
  jsOptions_Init(&jsOpts);
  s = natsConnection_JetStream(&js, nc, &jsOpts);
  if (s != NATS_OK) {
    spdlog::error("JetStream: {}", natsStatus_GetText(s));
    natsConnection_Destroy(nc);
    return 1;
  }

  jsStreamInfo* si = nullptr;
  jsErrCode jerr{};
  for (int i = 0; i < 60; ++i) {
    s = js_GetStreamInfo(&si, js, kStream, nullptr, &jerr);
    if (s == NATS_OK) {
      spdlog::info("stream \"{}\" found", kStream);
      break;
    }
    spdlog::info("waiting for stream \"{}\" ... ({})", kStream, i + 1);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  if (si) jsStreamInfo_Destroy(si);
  if (s != NATS_OK) {
    spdlog::error("stream {} not found — is contract nats-publisher up?",
                  kStream);
    jsCtx_Destroy(js);
    natsConnection_Destroy(nc);
    return 1;
  }

  jsConsumerConfig cfg_js;
  jsConsumerConfig_Init(&cfg_js);
  cfg_js.Durable = const_cast<char*>(kConsumer);
  cfg_js.AckPolicy = js_AckExplicit;
  cfg_js.FilterSubject = const_cast<char*>(kFilter);
  cfg_js.DeliverPolicy = js_DeliverNew;

  jsConsumerInfo* ci = nullptr;
  s = js_AddConsumer(&ci, js, kStream, &cfg_js, nullptr, &jerr);
  if (s != NATS_OK && !(s == NATS_ERR && jerr == JSConsumerNameExistErr)) {
    spdlog::warn("AddConsumer: {} (jerr={}) – continuing",
                 natsStatus_GetText(s), static_cast<int>(jerr));
  }
  if (ci) jsConsumerInfo_Destroy(ci);

  jsSubOptions so;
  jsSubOptions_Init(&so);
  so.Stream = const_cast<char*>(kStream);
  so.Consumer = const_cast<char*>(kConsumer);
  so.ManualAck = true;

  natsSubscription* sub = nullptr;
  s = js_PullSubscribe(&sub, js, kFilter, kConsumer, &jsOpts, &so, &jerr);
  if (s != NATS_OK || !sub) {
    spdlog::error("PullSubscribe: {} (jerr={})", natsStatus_GetText(s),
                  static_cast<int>(jerr));
    jsCtx_Destroy(js);
    natsConnection_Destroy(nc);
    return 1;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  spdlog::info(
      "crop-preparator ready — filter={} → enriched cv.object.* "
      "(side_channel={} republish={})",
      kFilter, emit_side_channel, republish_envelope);

  while (g_running) {
    natsMsgList list{};
    s = natsSubscription_Fetch(&list, sub, 8, 1000, &jerr);
    if (s == NATS_TIMEOUT) continue;
    if (s != NATS_OK) {
      spdlog::warn("Fetch: {} (jerr={})", natsStatus_GetText(s),
                   static_cast<int>(jerr));
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }

    for (int i = 0; i < list.Count; ++i) {
      natsMsg* msg = list.Msgs[i];

      detection::v1::ObjectEnvelope obj;
      if (!obj.ParseFromArray(natsMsg_GetData(msg),
                              natsMsg_GetDataLength(msg))) {
        spdlog::warn("failed to parse ObjectEnvelope ({} bytes)",
                     natsMsg_GetDataLength(msg));
        natsMsg_Ack(msg, nullptr);
        continue;
      }

      // Idempotency: skip our own re-publishes.
      if (already_prepared(obj)) {
        natsMsg_Ack(msg, nullptr);
        continue;
      }

      cv::Mat mat = decode_crop(obj);
      if (mat.empty()) {
        // No crop_jpeg — cannot prepare without a full-frame bus yet.
        // Pass through silently; specialists still see the original envelope.
        spdlog::debug(
            "frame={} track={} class={}: no crop_jpeg, skip prepare",
            obj.frame_id(), obj.track_id(), obj.class_name());
        natsMsg_Ack(msg, nullptr);
        continue;
      }

      // Crop is already the object region; treat image bounds as the box so
      // pad / plate_roi heuristics apply relative to this crop.
      const float bx1 = 0.f;
      const float by1 = 0.f;
      const float bx2 = static_cast<float>(mat.cols);
      const float by2 = static_cast<float>(mat.rows);

      auto prepared =
          crop::prepare(mat, bx1, by1, bx2, by2, obj.class_name(), cfg);
      if (prepared.empty()) {
        natsMsg_Ack(msg, nullptr);
        continue;
      }

      // Side-channel named ROIs (cv.crop.<kind>) — core NATS, not durable.
      if (emit_side_channel) {
        for (const auto& c : prepared) {
          auto pc = to_proto_crop(obj, c, cfg);
          std::string payload;
          if (pc.SerializeToString(&payload)) {
            const std::string subj = "cv.crop." + c.role;
            if (publish_core(nc, subj, payload)) {
              spdlog::debug("side-channel {} frame={} track={}", subj,
                            obj.frame_id(), obj.track_id());
            }
          }
        }
      }

      // Enriched ObjectEnvelope on the stable subject specialists already
      // consume. Loop-safe via crop_prepared label.
      if (republish_envelope) {
        auto enriched = enrich_envelope(obj, prepared, cfg);
        std::string payload;
        if (enriched.SerializeToString(&payload)) {
          const std::string subj = "cv.object." + obj.class_name();
          if (publish_js(js, subj, payload)) {
            spdlog::info(
                "enriched frame={} track={} class={} crops={} jpeg={}B",
                obj.frame_id(), obj.track_id(), obj.class_name(),
                prepared.size(), enriched.crop_jpeg().size());
          }
        }
      }

      natsMsg_Ack(msg, nullptr);
    }
    natsMsgList_Destroy(&list);
  }

  natsSubscription_Destroy(sub);
  jsCtx_Destroy(js);
  natsConnection_Destroy(nc);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  edge::otel::init("crop-preparator", "0.2.0");
  spdlog::set_level(spdlog::level::info);

  // Demo mode: positional JPEG + bbox (offline, no NATS).
  if (argc >= 6) {
    int rc = run_demo(argc, argv);
    edge::otel::shutdown();
    return rc;
  }

  // Production: no args → NATS JetStream path.
  if (argc == 1 ||
      (argc == 2 && std::string(argv[1]) == "--nats")) {
    int rc = run_nats();
    edge::otel::shutdown();
    return rc;
  }

  std::cerr
      << "Usage:\n"
      << "  Production (NATS):  " << argv[0] << " [--nats]\n"
      << "  Demo (file+bbox):   " << argv[0]
      << " <image.jpg> <x1> <y1> <x2> <y2> [class_name]\n"
      << "\n"
      << "Env:\n"
      << "  NATS_URL                  nats://localhost:4222\n"
      << "  CROP_REPUBLISH_ENVELOPE   1  (enriched ObjectEnvelope → cv.object.*)\n"
      << "  CROP_SIDE_CHANNEL         1  (PreparedCrop → cv.crop.<kind>)\n"
      << "  CROP_PAD_PX               12\n"
      << "  CROP_JPEG_QUALITY         85\n"
      << "  CROP_MAKE_PLATE_ROI       1\n"
      << "  CROP_TARGET_SIZE          0\n";
  edge::otel::shutdown();
  return 1;
}
