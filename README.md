# crop-preparator

Lightweight OpenCV crop / ROI preparation stage for AgentJetson.

**Input**: `ObjectEnvelope` (bbox + optional existing crop) or raw frame + bbox  
**Output**: one or more prepared crops (`full`, `plate_roi`, …) ready for secondary specialists

This sits between the primary `object-classifier` and secondary consumers (ALPR, attributes, …).  
Later the implementation can be swapped for an avplumber (or GStreamer/DeepStream) graph without changing the ObjectEnvelope contract.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Demo

```bash
./build/crop_preparator vehicle.jpg 100 200 400 500 car
```

## Production wiring (TODO)

- Subscribe to `cv.object.*` (NATS) or receive ObjectEnvelopes via gRPC
- Emit enriched ObjectEnvelopes (updated `crop_jpeg` + labels) or side-channel prepared-crop messages
- Specialists continue to consume the stable ObjectEnvelope contract
