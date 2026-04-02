# ESP32 PM Monitor

Real-time air quality monitor using an **ESP32** and **PMS7003** sensor.
Data is pushed to a cloud backend and displayed on a password-protected,
publicly accessible dashboard you can view from anywhere.

---

## What it measures

| Value | Description |
|---|---|
| PM1.0 std / atm | 1.0 µm particles — standard & atmospheric (µg/m³) |
| PM2.5 std / atm | 2.5 µm particles — standard & atmospheric (µg/m³) |
| PM10 std / atm  | 10 µm particles — standard & atmospheric (µg/m³) |
| >0.3 µm count   | Particle count per 0.1 L of air |
| >0.5 / 1.0 / 2.5 / 5.0 / 10 µm | Same, for each size threshold |

---

## Architecture

```
PMS7003 ──UART──▶ ESP32 ──WiFi──▶ Backend (Render/Fly.io)
                                         │
                                   SQLite DB
                                         │
                              Web Dashboard (HTTPS)
                                   ↑ you browse from anywhere
```

---

## Setup — Step by step

### Step 1 — Wire the hardware

| PMS7003 pin | ESP32 pin |
|---|---|
| VCC | 5 V |
| GND | GND |
| TX  | GPIO 16 (configurable) |
| RX  | GPIO 17 (configurable) |
| SET | 3.3 V (keep HIGH = active mode) |
| RST | 3.3 V (keep HIGH) |

---

### Step 2 — Deploy the backend (free tier)

Choose **Option A (Render)** or **Option B (Fly.io)**.

#### Option A — Render.com (easiest, free)

1. Create a free account at <https://render.com>.
2. Click **New → Web Service** → **Connect a Git repository** → select this repo.
3. Set:
   - **Root Directory**: `esp32-pm-monitor/backend`
   - **Build Command**: `pip install -r requirements.txt`
   - **Start Command**: `uvicorn app:app --host 0.0.0.0 --port $PORT`
   - **Instance Type**: Free
4. Add a **Disk** (under Advanced):
   - Name: `pm-data` | Mount path: `/data` | Size: 1 GB
5. Add **Environment Variables**:
   | Key | Value |
   |---|---|
   | `DB_PATH` | `/data/pm_data.db` |
   | `API_KEY` | _generate a random string, e.g. `openssl rand -hex 16`_ |
   | `DASH_USER` | `admin` (or your choice) |
   | `DASH_PASS` | _strong password_ |
   | `MAX_RECORDS` | `10000` |
6. Click **Create Web Service**. Render builds and deploys.
7. Note your URL: `https://pm-monitor-xxxx.onrender.com`

> **Free tier note**: Render spins down idle services after 15 min.
> The ESP32's regular pings keep it awake.

---

#### Option B — Fly.io (more reliable free tier)

1. Install the Fly CLI: <https://fly.io/docs/hands-on/install-flyctl/>
2. Sign up / log in:
   ```bash
   fly auth signup   # or: fly auth login
   ```
3. From the `esp32-pm-monitor/` directory:
   ```bash
   fly launch --config fly.toml --no-deploy
   ```
   - Accept the generated app name or enter your own.
   - Choose region closest to you (`bom` = Mumbai, `sin` = Singapore, `lhr` = London).
4. Create a volume for persistent data:
   ```bash
   fly volumes create pm_data --region sin --size 1
   ```
5. Set secrets:
   ```bash
   fly secrets set \
     API_KEY="$(openssl rand -hex 16)" \
     DASH_USER="admin" \
     DASH_PASS="$(openssl rand -hex 8)"
   ```
   **Copy these values — you'll need `API_KEY` for the ESP32.**
6. Deploy:
   ```bash
   fly deploy
   ```
7. Your URL: `https://pm-monitor.fly.dev`

---

### Step 3 — Flash the ESP32

#### Using PlatformIO (recommended)

1. Install [VS Code](https://code.visualstudio.com) + the
   [PlatformIO extension](https://platformio.org/install/ide?install=vscode).
2. Open the `esp32-pm-monitor/esp32/` folder in VS Code.
3. Copy `pm_monitor/config.h.example` → `pm_monitor/config.h`.
4. Edit `config.h`:
   ```c
   #define WIFI_SSID     "YourWiFiName"
   #define WIFI_PASSWORD "YourWiFiPassword"
   #define SERVER_URL    "https://pm-monitor.fly.dev"   // your backend URL
   #define API_KEY       "the-api-key-from-step-2"
   ```
5. Connect the ESP32 via USB.
6. Click **Upload** (→ button) in PlatformIO.

#### Using Arduino IDE

1. Install [Arduino IDE 2](https://www.arduino.cc/en/software).
2. Add ESP32 board support:
   - File → Preferences → Additional boards URLs:
     `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   - Tools → Board → Boards Manager → search "esp32" → Install.
3. Install library: Sketch → Include Library → Manage → search **ArduinoJson** → Install.
4. Open `esp32/pm_monitor/pm_monitor.ino`.
5. Create `config.h` from the example (see above).
6. Select board: **ESP32 Dev Module** | Upload speed: **921600**.
7. Upload.

---

### Step 4 — View the dashboard

Open your backend URL in any browser (desktop or mobile):

```
https://pm-monitor.fly.dev
```

You'll be prompted for `DASH_USER` / `DASH_PASS`.  
The dashboard auto-refreshes every 30 seconds.

---

## Dashboard features

- **7 themes** — Midnight, Aurora, Sunset, Ocean, Rose Quartz, Light, Cyberpunk  
  (choice persists in `localStorage`)
- **All 12 PM + particle-count values** shown as live stat cards  
- **PM2.5 AQI colour coding** — green / amber / red based on US EPA thresholds
- **Dual-mode charts** — Standard (CF=1), Atmospheric, or Both overlaid
- **Particle count chart** for all 6 size bins
- **Time range selector** — 1 h, 6 h, 24 h, 3 d, 7 d, All
- **Sampling interval control** with presets (1 min → 1 hr) and custom input  
  ESP32 picks up changes within 60 seconds

---

## API reference

All endpoints accept `X-API-Key: <key>` header OR HTTP Basic Auth.

| Method | Path | Auth | Description |
|---|---|---|---|
| POST | `/api/data` | API key | ESP32 posts a reading |
| GET | `/api/data?hours=24&limit=500` | Any | Retrieve readings |
| GET | `/api/config` | Any | Get current interval |
| PUT | `/api/config` | Any | Set interval `{"interval_sec": 300}` |
| GET | `/` | Basic Auth | Dashboard |

---

## Local development

```bash
cd esp32-pm-monitor/backend
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt

export API_KEY=dev DASH_USER=admin DASH_PASS=dev
python app.py
# → http://localhost:8000
```

Test a fake reading:
```bash
curl -X POST http://localhost:8000/api/data \
  -H "X-API-Key: dev" \
  -H "Content-Type: application/json" \
  -d '{"pm1_0_std":5,"pm2_5_std":12,"pm10_std":18,
       "pm1_0_atm":4,"pm2_5_atm":11,"pm10_atm":17,
       "cnt_0_3um":1200,"cnt_0_5um":350,"cnt_1_0um":80,
       "cnt_2_5um":20,"cnt_5_0um":4,"cnt_10um":1}'
```

---

## Security notes

- The dashboard requires HTTP Basic Auth.  Credentials travel over HTTPS.
- The ESP32 authenticates with a per-deployment `API_KEY` header.
- **Change the defaults** (`changeme`) before deploying.
- `config.h` is listed in `.gitignore` — never commit it.
