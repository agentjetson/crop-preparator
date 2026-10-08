# crop-preparator

Lightweight OpenCV crop / ROI preparation stage for AgentJetson.

**Input**: `ObjectEnvelope` (bbox + optional existing `crop_jpeg`) or raw frame + bbox  
**Output**: one or more prepared crops (`full`, `plate_roi`, …) ready for secondary specialists

This sits between the primary [`object-classifier`](https://github.com/agentjetson/object-classifier) and secondary consumers ([`alpr-consumer`](https://github.com/agentjetson/alpr-consumer), attributes, …).  
Later the implementation can be swapped for an avplumber (or GStreamer/DeepStream) graph without changing the `ObjectEnvelope` contract.

Architecture map: [`agentjetson/core` ARCHITECTURE.md](https://github.com/agentjetson/core/blob/main/ARCHITECTURE.md).

---

## Role in the stack

```text
object-classifier
        │  ObjectEnvelope  (cv.object.<class>)
        ▼
crop-preparator   ◀── durable consumer crop-prep-worker on CV_EVENTS
        │
        ├─▶ enriched ObjectEnvelope  →  cv.object.<class>
        │     (labels.crop_prepared=1, improved crop_jpeg)
        └─▶ optional PreparedCrop    →  cv.crop.<kind>
              (full | plate_roi | …  — hot-path, not durable)
        │
        ▼
specialists (alpr-consumer, …)  — still pure consumers of ObjectEnvelope
```

- **Loop-safe**: re-published envelopes carry `labels["crop_prepared"]="1"`; the consumer skips them.
- **No full-frame bus required**: works from `ObjectEnvelope.crop_jpeg` when present (typical object-classifier path).
- **Specialists unchanged**: they keep consuming the stable `detection.v1.ObjectEnvelope` contract.

Subjects (owned by [contract `domain/nats-subjects.yaml`](https://github.com/agentjetson/core/blob/main/domain/nats-subjects.yaml)):

| Direction | Message            | Subject              |
|-----------|--------------------|----------------------|
| In        | `ObjectEnvelope`   | `cv.object.*`        |
| Out       | `ObjectEnvelope`   | `cv.object.<class>`  |
| Out (opt) | `PreparedCrop`     | `cv.crop.<crop_kind>`|

---

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Binary: `build/crop_preparator`.

---

## Production (NATS)

Requires the contract stack (NATS JetStream stream `CV_EVENTS`):

```bash
# contract repo
make up

# this binary
NATS_URL=nats://localhost:4222 ./build/crop_preparator
# or explicitly:
NATS_URL=nats://localhost:4222 ./build/crop_preparator --nats
```

### Environment

| Variable | Default | Meaning |
|----------|---------|---------|
| `NATS_URL` | `nats://localhost:4222` | JetStream |
| `CROP_REPUBLISH_ENVELOPE` | `1` | Re-publish enriched `ObjectEnvelope` on `cv.object.<class>` |
| `CROP_SIDE_CHANNEL` | `1` | Also emit `PreparedCrop` on `cv.crop.<kind>` |
| `CROP_PAD_PX` | `12` | Padding around object bbox |
| `CROP_JPEG_QUALITY` | `85` | JPEG quality for emitted crops |
| `CROP_MAKE_PLATE_ROI` | `1` | Heuristic plate ROI for vehicle classes |
| `CROP_TARGET_SIZE` | `0` | 0 = keep size; else square resize |

Durable consumer name: **`crop-prep-worker`** on stream **`CV_EVENTS`**, filter **`cv.object.*`**.

---

## Demo (offline, no NATS)

```bash
./build/crop_preparator vehicle.jpg 100 200 400 500 car
```

---

## Design notes

1. **ObjectEnvelope remains the cornerstone.** Specialists never need to know whether crop-preparator ran.
2. **Side-channel is optional.** Prefer enriched envelopes for ALPR/attributes; use `cv.crop.*` only when a specialist needs a *named* ROI without replacing the primary crop.
3. **Idempotent.** The `crop_prepared` label prevents infinite re-enrichment on the same subject.
4. **Edge-first.** Pure OpenCV today; swap the body of `crop::prepare` for DeepStream/avplumber later without touching contracts.
