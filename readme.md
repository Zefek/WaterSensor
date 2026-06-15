# WaterSensor

Automatic reading of a mechanical water meter using an ESP32-CAM and machine learning.

A cheap camera (ESP32-CAM) periodically photographs the meter dial, the image
is sent to a backend, and a neural network reads the counter value from the
photo (both the digit wheels and the needle gauges for the decimal places).
The result is stored, validated, and pushed onward — into a database,
Home Assistant, Grafana dashboards, or an MQTT topic — so you can track water
consumption continuously and detect leaks.

> This document covers three repositories that together form the system:
> - **[Zefek/WaterSensor](https://github.com/Zefek/WaterSensor)** — the ESP32-CAM firmware and the Python recognition worker.
> - **[Zefek/OdectyMVC](https://github.com/Zefek/OdectyMVC)** — the .NET gateway / web app that receives the image upload from the ESP32 (Basic-auth `POST`), stores the JPEG, and publishes the message that triggers recognition. Also serves the web UI.
> - **[Zefek/Odecty](https://github.com/Zefek/Odecty)** — the .NET backend microservice ("OdectyStat") that consumes recognition results, validates them, stores measurements, and integrates with Home Assistant / Grafana / MQTT. Only the water-meter-relevant parts are described here; the project also handles other sensor types (heater, garage, LS sensor).

---

## What it is for, and why

Ordinary household water meters have no digital output. To track consumption
continuously — instead of reading it once a year — you would normally have to
replace the meter with a "smart" one or read it somehow from the outside.

This project takes the second route: **non-invasive optical reading**.

- Nothing on the water meter itself is modified (no tampering with a certified measuring device).
- A camera costing a few dollars is enough.
- Data flows automatically every 5 minutes.
- Enables leak detection (continuous consumption at night), consumption graphs,
  and smart-home integration.

---

## How it works (end-to-end architecture)

```
┌──────────────┐  POST         ┌──────────────────┐  AMQP            ┌──────────────────┐
│  ESP32-CAM   │  api/gauge/   │   OdectyMVC      │  gaugemvc.gauge  │   Recognition    │
│  firmware    │  {id}         │   gateway (.NET) │  .fileuploaded   │  worker (Python) │
│  (Arduino)   │ ────────────► │  Basic-auth,     │ ───────────────► │   CNN + OpenCV   │
│              │  octet-stream │  saves JPEG,     │  {GaugeId,       │                  │
│ photo /5 min │  Basic auth   │  publishes msg   │   Datetime,      │  reads the dial  │
└──────────────┘               └──────────────────┘   FileName}      └────────┬─────────┘
                                                       → recognize queue       │
                                            recognize.success / recognize.fail │
                                                                               │
                                                                               ▼
                                                          ┌──────────────────────────┐
                                                          │   OdectyStat backend     │
                                                          │   (.NET, Zefek/Odecty)   │
                                                          │                          │
                                                          │  validates the value,    │
                                                          │  stores measurements,    │
                                                          │  files the image,        │
                                                          │  publishes onward        │
                                                          └────────────┬─────────────┘
                                                                       │
                                       SQL Server · PostgreSQL (Home Assistant) ·
                                       MQTT (watermeter.state) · Grafana dashboards
```

The pipeline has four stages:

1. **ESP32-CAM firmware** — every 5 minutes captures a JPEG and sends it via
   HTTP POST to the configured endpoint.
2. **OdectyMVC gateway** (.NET) — Basic-auth `POST api/gauge/{id}` receives the
   image, stores it as `{gaugeType}_{guid}.jpg`, and publishes a
   `gaugemvc.gauge.fileuploaded` message `{GaugeId, Datetime, FileName}` to
   RabbitMQ (bound to the `recognize` queue).
3. **Recognition worker** (Python) — consumes the request, reads the value from
   the photo, and publishes the result (`recognize.success` / `recognize.fail`).
4. **OdectyStat backend** (.NET) — consumes the result, validates it, persists
   the measurement, and forwards it to Home Assistant / MQTT / Grafana.

> **On read vs. write:** the gateway (OdectyMVC) accepts writes only as the image
> upload and turns them into RabbitMQ messages. Reads (gauge overview, last photo)
> are served by the OdectyStat microservice on loopback (`127.0.0.1:5080`); the
> gateway handles authorization in front of it. See `OdectyMVC/API.md`.

---

## Components

### 1. ESP32-CAM firmware — `WaterSensor/src/WaterSensor/`

Arduino sketch for the **ESP32-CAM** board (profile `esp32:esp32:esp32cam`, see `sketch.yaml`).

| File | Purpose |
|------|---------|
| `WaterSensor.ino` | Main loop: Wi-Fi connect, capture every 5 min, send JPEG via HTTP POST (in 512 B chunks, up to 5 retries). |
| `camera.cpp` / `camera.h` | Camera init (SVGA, JPEG quality 5, framebuffer in PSRAM), capture, LED flash control. |
| `pin_config.h` | GPIO pin maps for various camera modules (AI_THINKER, M5STACK, XIAO_ESP32S3, …). |
| `config_default.h` | **Template** config — Wi-Fi, server address, endpoint, port, authorization header. |
| `sketch.yaml` | Arduino CLI profile (FQBN, ESP32 platform 3.2.0, baud 115200). |

Behavior (`WaterSensor.ino`):

- Connects to Wi-Fi on boot (and reconnects on drop).
- Calls `captureAndSend()` on a `5 minute` interval.
- Sends the image in 512 B blocks with small delays (gentle on weak Wi-Fi),
  logging transfer speed, RSSI, and free heap.
- Retries the connection up to 5 times on failure.
- Deinitializes the camera after sending (saves power / memory).

### 2. Recognition worker — `WaterSensor/src/Recognition/`

Python, built on **TensorFlow/Keras** (CNN for digits) and **OpenCV** (needle
detection), communicating over **RabbitMQ** (`pika`).

| File | Purpose |
|------|---------|
| `prediction.py` | Production worker — consumes the queue, reads the photo, returns the value. |
| `training.py` | Trains the CNN digit classifier (0–9). |
| `build_dataset.py` | Builds the training set from already-confirmed photos. |
| `watersensor.keras` | The trained model (included). |
| `config_example.py` | **Template** config — paths, calibration coordinates, HSV range, RabbitMQ topology. |
| `config_secret_example.py` | **Template** RabbitMQ credentials. |

**How `prediction.py` reads the meter:**

1. Crops 5 cells of the mechanical counter (`DIGIT_COORDS`) and feeds each to the
   CNN → the integer part (m³).
2. Crops 4 needle gauges for the decimal places (`GAUGE_AREAS`), uses an HSV mask
   to find the colored needle, computes its angle → one decimal digit each.
3. `get_value()` corrects needle transitions (when the highest order wraps 9→0,
   an adjacent needle may not have fully turned yet / has overshot).
4. Checks JPEG integrity (`detect_jpeg_glitch` — looks for the corrupted rows
   typical of an incomplete ESP32 transfer).
5. Publishes the result (e.g. `00563.3100`) to `recognize.success`, or a failure
   to `recognize.fail`.

**Training set (`build_dataset.py`):** training data comes from photos that were
already read successfully and saved with a filename encoding the correct value
(`vodomer_<hash>_<counter>,<dials>.jpg`). The script derives the correct digits
from the name, crops the cells exactly as `prediction.py` does (train/inference
consistency), and does a deterministic 80/10/10 hash-based split — a whole image
always lands in a single split (no data leak).

**Model (`training.py`):** grayscale digit crop **48 × 37 px** → 3 conv blocks
(32 → 64 → 128 filters, BatchNorm, MaxPool/GAP, Dropout) → softmax over 10
classes. Uses vertical-shift augmentation (digits scroll on the drum counter),
`EarlyStopping` + `ModelCheckpoint`, and verifies that the saved `.keras` model
matches the in-memory one.

### 3. Gateway / image intake — `OdectyMVC/` (.NET / ASP.NET Core MVC)

The public-facing app. It authenticates the ESP32, receives the photo, stores
it, and kicks off recognition. It also hosts a web UI (`Views/`, `wwwroot/`).

**Image upload** (`Controllers/GaugeController.cs`):

```csharp
[HttpPost("{id}")]
[Consumes("application/octet-stream")]
public async Task<IActionResult> GaugeByImage(int id, CancellationToken ct)
{
    using var ms = new MemoryStream();
    await Request.Body.CopyToAsync(ms, ct);
    await gaugeService.SaveFileForGauge(id, ms, ct);   // → save + publish
    return Ok();
}
```

- Route `POST api/gauge/{id}` (the `{id}` is the gauge ID), content type
  `application/octet-stream` — this is exactly what the firmware POSTs.
- **Basic authentication** (`BasicAuthenticationHandler.cs`) guards the endpoint
  in non-DEBUG builds; the firmware sends the matching `Authorization` header.

**Save + trigger** (`Application/GaugeService.cs → SaveFileForGauge`):

- Saves the JPEG as `{gauge.Type}_{Guid:N}.jpg` into the per-gauge folder
  (`options.Value.Path`, formatted with the gauge ID).
- Publishes a RabbitMQ message with routing key
  `gaugemvc.gauge.fileuploaded` and payload `{ GaugeId, Datetime (UTC), FileName }`.
  On the broker this is bound to the `recognize` queue that the Python worker
  consumes — that message **is** the recognition request.

> The `OdectyMVC/API.md` documents the read API (`GET /api/gauges`,
> `GET /api/gauges/{id}/lastphoto`, health checks). Writes are never HTTP — they
> go through RabbitMQ. The actual read data is served by OdectyStat (below).

### 4. Backend — `Odecty/OdectyStat/` (.NET / ASP.NET Core)

The backend ("OdectyStat") runs as a Windows Service / ASP.NET Core app. It is
the destination for recognition results and the system of record for
measurements. Water-meter-relevant pieces:

**RabbitMQ consumers** (background services, see `DataLayer/Consumers/`):

| Consumer | Queue | Action |
|----------|-------|--------|
| `RecognizedSuccess` | `gauge_recognize_success` | parses `{gaugeId, file, state, datetime}` → `GaugeService.GaugeRecognizedSucceeded(...)` |
| `RecognizedFailed` | `gauge_recognize_fail` | parses `{gaugeId, file}` → `GaugeService.GaugeRecognizedFailed(...)` |
| `MQClient` | `Odecty` | parses `NewValue {GaugeId, Value, Datetime}` → `GaugeService.AddNewValue(...)` (generic value ingestion) |

**Value validation** (`Application/GaugeService.cs` → `GaugeRecognizedSucceeded`) —
the core "is this reading believable?" logic:

- Looks up the gauge; if the new value equals the last one → valid (just touch the timestamp).
- Otherwise the increase must be non-negative **and** within
  `MaxValuePerHour × elapsed hours`. If it exceeds that, it tries to repair an
  integer-carry misread: for `inc` in 0..2, test `candidate = (prevInt + inc) + newDecimal`
  and accept the first candidate whose delta fits the allowed increment.
- With no prior measurement, a value below `LastValue` is rejected.
- **On valid:** stores the increment as a `GaugeMeasurement` (SQL Server),
  then publishes the value to MQTT (`amq.topic`, routing key `watermeter.state`)
  and to the exchange with `odecty.gauge.lastvaluechanged`; updates Home Assistant
  daily statistics (PostgreSQL) via `ComputeService3`; and moves the image into a
  per-gauge/per-date **success** folder, renaming it with the recognized value.
- **On invalid/fail:** moves the image into the **failed** folder for later inspection.

**REST API** (`Controllers/GaugesController.cs`):

| Method | Route | Purpose |
|--------|-------|---------|
| `GET` | `/api/gauges` | overview of all gauges |
| `GET` | `/api/gauges/{id}/lastphoto` | the most recent photo for a gauge |

**Storage & integrations:**

- **SQL Server** — main `Odecty` DB (`Gauge`, `GaugeMeasurement`).
- **PostgreSQL** — Home Assistant statistics (long-term + short-term) and diagnostics.
- **MQTT** (`amq.topic`) — pushes the current reading on `watermeter.state` (Home Assistant).
- **Grafana** — dashboards under `Odecty/grafana/`; `Odecty/db/grafana_grants.sql` grants read access.
- **Image folders** (`Dto/GaugeImageLocation.cs`) — `Path` (incoming per gauge),
  `RecognizedSuccessFolder`, `RecognizedFailedFolder` (organized by gauge ID + date).
- **Observability** — OpenTelemetry (OTLP export), liveness/readiness health checks.

---

## RabbitMQ message contract (the glue)

| Direction | Routing key | Queue (consumer) | Payload |
|-----------|-------------|------------------|---------|
| gateway → worker | `gaugemvc.gauge.fileuploaded` | `recognize` (Python) | `{ GaugeId, Datetime, FileName }` |
| worker → backend | `recognize.success` | `gauge_recognize_success` (.NET) | `{ file, gaugeId, state, datetime, value, value_0..3 }` |
| worker → backend | `recognize.fail` | `gauge_recognize_fail` (.NET) | `{ file, gaugeId, result: "Fail", … }` |
| backend → consumers | `odecty.gauge.lastvaluechanged` | — | `{ gaugeId, value }` |
| backend → MQTT | `watermeter.state` (`amq.topic`) | — | `"<value>"` (string) |

> Routing-key constants live in each app's `MessageQueueRoutingKeys` (e.g.
> `GaugeMVC_Gauge_Fileuploaded = "gaugemvc.gauge.fileuploaded"`). Queue names on
> the Python side come from `config.py` (`EXCHANGE`, `QUEUE_NAME`,
> `ROUTING_KEY_SUCCESS/FAIL`); on the .NET side from `QueuesToConsume` and the
> configured `QueueMappings`. The gateway's `fileuploaded` key must be bound to
> the worker's `recognize` queue on the broker.

---

## Getting it running

### Firmware (ESP32-CAM)

1. Copy `config_default.h` to `config.h` and fill it in:
   ```c
   #define WifiSSID     "your-wifi"
   #define WifiPassword "password"
   #define Server       "server.address"
   #define endpoint     "/api/upload"
   #define Port         80
   #define auth         "Bearer ..."   // value of the Authorization header
   ```
2. Select your camera model in `pin_config.h` (e.g. `#define CAMERA_MODEL_AI_THINKER`).
3. Flash via Arduino CLI / IDE (profile `WaterSensor_ESP32` from `sketch.yaml`):
   ```
   arduino-cli compile --profile WaterSensor_ESP32
   arduino-cli upload  --profile WaterSensor_ESP32 -p COMx
   ```
4. The serial monitor at **115200 baud** shows progress (log messages are in Czech).

### Recognition worker (Python)

1. Install dependencies:
   ```
   pip install tensorflow opencv-python pillow numpy pika
   ```
2. Copy the templates and fill in real values:
   ```
   copy config_example.py config.py
   copy config_secret_example.py config_secret.py
   ```
   - `config.py` — folder/model paths, **calibration coordinates**
     (`DIGIT_COORDS`, `GAUGE_AREAS`), needle HSV range, RabbitMQ topology.
   - `config_secret.py` — RabbitMQ host/port/credentials.
   - Both belong in `.gitignore` (never commit them).
3. **Calibrate the crops** — the `(x, y, w, h)` coordinates must match the camera's
   position relative to the dial. Without calibration the system reads incorrectly.
4. Run the worker:
   ```
   python prediction.py
   ```

### Gateway (OdectyMVC / .NET)

1. Open `OdectyMVC/OdectyMVC.sln` (.NET / ASP.NET Core MVC).
2. Configure:
   - `BasicAuthentication` (`Username`, `Password`) — must match the firmware's
     `Authorization` header.
   - the image folder path option used by `SaveFileForGauge` (per-gauge `Path`).
   - RabbitMQ connection + the `gaugemvc.gauge.fileuploaded` binding to the
     `recognize` queue.
3. Run it; point the firmware's `Server` / `Port` / `endpoint` (`api/gauge/{id}`)
   and `auth` header at this app.

### Backend (OdectyStat / .NET)

1. Open `Odecty/OdectyStat.sln` (.NET / ASP.NET Core).
2. Configure (typically `appsettings.json` / environment / user-secrets):
   - `OdectySettings` — RabbitMQ host/credentials/vhost, exchange name, and
     `QueueMappings` (queue ↔ exchange ↔ routing key bindings).
   - `GaugeImageLocation` — `Path`, `RecognizedSuccessFolder`, `RecognizedFailedFolder`.
   - SQL Server + PostgreSQL connection strings.
3. Apply the database schema / migrations, then run the service. It will start the
   RabbitMQ background consumers and expose the REST API and health endpoints.
4. Import the dashboards from `Odecty/grafana/` and apply `Odecty/db/grafana_grants.sql`.

### (Re)training the model — optional

```
python build_dataset.py     # builds train/val/test from confirmed photos
python training.py          # trains and saves watersensor.keras
```

---

## Key configuration reference

| Where | Key | Meaning |
|-------|-----|---------|
| `config.h` (firmware) | `Server`, `endpoint`, `Port`, `auth` | where the ESP32 uploads photos (`endpoint` = `api/gauge/{id}`) |
| `BasicAuthentication` (OdectyMVC) | `Username`, `Password` | credentials the firmware's Basic-auth header must match |
| `config.py` | `DIGIT_COORDS` | crops for the 5 mechanical counter digits |
| `config.py` | `GAUGE_AREAS` | crops for the 4 needle gauges (decimal places) |
| `config.py` | `HSV_LOWER/UPPER` | needle color range (default orange-red) |
| `config.py` | `EXCHANGE`, `QUEUE_NAME`, `ROUTING_KEY_*` | RabbitMQ topology (Python side) |
| `config_secret.py` | `RABBITMQ_*` | broker connection (Python side) |
| `OdectySettings` (.NET) | `RabbitMQ*`, `ExchangeName`, `QueueMappings` | broker connection + bindings (backend) |
| `GaugeImageLocation` (.NET) | `Path`, `Recognized*Folder` | image storage layout |
| `Gauge.MaxValuePerHour` (.NET / DB) | — | plausibility ceiling used during validation |

---

## Technology stack

- **Hardware:** ESP32-CAM (with PSRAM)
- **Firmware:** Arduino / C++ (`esp_camera`, `WiFi`)
- **Recognition:** Python, TensorFlow / Keras (CNN), OpenCV, Pillow, NumPy
- **Messaging:** RabbitMQ (AMQP; `pika` in Python, `RabbitMQ.Client` in .NET), MQTT (`amq.topic`)
- **Gateway / web:** .NET / ASP.NET Core MVC (Basic auth, image intake, web UI)
- **Backend:** .NET / ASP.NET Core (Windows Service), Entity Framework
- **Storage:** SQL Server (measurements), PostgreSQL (Home Assistant + diagnostics)
- **Dashboards / observability:** Grafana, OpenTelemetry (OTLP)

---

## Status & notes

- Comments and log messages across the projects are in Czech.
- The included `watersensor.keras` model works, but crop coordinates and HSV
  ranges must be calibrated for each individual camera installation.
- Secrets are kept out of git (`config.py`, `config_secret.py`, `config.h`,
  backend `appsettings`/user-secrets).
- Reads and writes are split: the **OdectyMVC** gateway takes the image upload
  (write, via RabbitMQ) and serves the web UI; the **OdectyStat** microservice
  serves the read API on loopback and runs the result-consuming backend logic.
- `Odecty` is a multi-sensor platform; only the water-meter (gauge) parts are
  documented here. Heater / garage / LS-sensor diagnostics are out of scope.
