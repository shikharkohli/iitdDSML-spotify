"""
ESP32 PM Monitor — Backend
FastAPI + SQLite

Endpoints
─────────
  POST /api/data          ESP32 posts sensor readings   (X-API-Key)
  GET  /api/data          Historical readings            (API key or Basic Auth)
  GET  /api/config        Current config                 (API key or Basic Auth)
  PUT  /api/config        Update config from dashboard   (API key or Basic Auth)
  GET  /                  Dashboard HTML                 (HTTP Basic Auth)

Environment variables
─────────────────────
  API_KEY      Secret key header used by ESP32     (default: changeme)
  DASH_USER    Dashboard Basic Auth username        (default: admin)
  DASH_PASS    Dashboard Basic Auth password        (default: changeme)
  DB_PATH      SQLite file path                     (default: pm_data.db)
  MAX_RECORDS  Max rows kept in DB                  (default: 10000)
"""

import os
import secrets
import sqlite3
import time
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

from fastapi import Depends, FastAPI, HTTPException, Request, status
from fastapi.responses import HTMLResponse
from fastapi.security import HTTPBasic, HTTPBasicCredentials
from pydantic import BaseModel, Field

# ─── Config ───────────────────────────────────────────────────────────────────
API_KEY     = os.getenv("API_KEY",     "changeme")
DASH_USER   = os.getenv("DASH_USER",   "admin")
DASH_PASS   = os.getenv("DASH_PASS",   "changeme")
DB_PATH     = os.getenv("DB_PATH",     "pm_data.db")
MAX_RECORDS = int(os.getenv("MAX_RECORDS", "10000"))

# ─── Database ─────────────────────────────────────────────────────────────────
def get_db() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    conn = get_db()
    conn.executescript("""
        CREATE TABLE IF NOT EXISTS readings (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            ts          REAL    NOT NULL,
            pm1_0_std   INTEGER NOT NULL,
            pm2_5_std   INTEGER NOT NULL,
            pm10_std    INTEGER NOT NULL,
            pm1_0_atm   INTEGER NOT NULL,
            pm2_5_atm   INTEGER NOT NULL,
            pm10_atm    INTEGER NOT NULL,
            cnt_0_3um   INTEGER NOT NULL,
            cnt_0_5um   INTEGER NOT NULL,
            cnt_1_0um   INTEGER NOT NULL,
            cnt_2_5um   INTEGER NOT NULL,
            cnt_5_0um   INTEGER NOT NULL,
            cnt_10um    INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS config (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
        INSERT OR IGNORE INTO config (key, value) VALUES ('interval_sec', '1800');
    """)
    conn.commit()
    conn.close()


# ─── FastAPI ──────────────────────────────────────────────────────────────────
@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db()
    yield

app     = FastAPI(title="PM Monitor", lifespan=lifespan)
_basic  = HTTPBasic(auto_error=False)

# ─── Auth ─────────────────────────────────────────────────────────────────────
def _api_key_ok(request: Request) -> bool:
    return secrets.compare_digest(
        request.headers.get("X-API-Key", ""), API_KEY
    )


def _basic_ok(credentials: Optional[HTTPBasicCredentials]) -> bool:
    if credentials is None:
        return False
    return secrets.compare_digest(credentials.username, DASH_USER) and \
           secrets.compare_digest(credentials.password, DASH_PASS)


def require_api_key(request: Request):
    if not _api_key_ok(request):
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "Invalid API key")


def require_basic_auth(
    credentials: Optional[HTTPBasicCredentials] = Depends(_basic),
):
    if not _basic_ok(credentials):
        raise HTTPException(
            status.HTTP_401_UNAUTHORIZED,
            "Authentication required",
            headers={"WWW-Authenticate": "Basic"},
        )
    return credentials.username  # type: ignore[union-attr]


def require_any_auth(
    request: Request,
    credentials: Optional[HTTPBasicCredentials] = Depends(_basic),
):
    if _api_key_ok(request) or _basic_ok(credentials):
        return
    raise HTTPException(
        status.HTTP_401_UNAUTHORIZED,
        "Authentication required",
        headers={"WWW-Authenticate": "Basic"},
    )


# ─── Models ───────────────────────────────────────────────────────────────────
class Reading(BaseModel):
    pm1_0_std: int = Field(..., ge=0)
    pm2_5_std: int = Field(..., ge=0)
    pm10_std:  int = Field(..., ge=0)
    pm1_0_atm: int = Field(..., ge=0)
    pm2_5_atm: int = Field(..., ge=0)
    pm10_atm:  int = Field(..., ge=0)
    cnt_0_3um: int = Field(..., ge=0)
    cnt_0_5um: int = Field(..., ge=0)
    cnt_1_0um: int = Field(..., ge=0)
    cnt_2_5um: int = Field(..., ge=0)
    cnt_5_0um: int = Field(..., ge=0)
    # PMS5003 omits this field (reserved bytes); PMS7003 provides it.
    cnt_10um:  Optional[int] = Field(None, ge=0)


class ConfigUpdate(BaseModel):
    interval_sec: int = Field(..., ge=10, le=86400)


# ─── Routes ───────────────────────────────────────────────────────────────────

@app.post("/api/data", status_code=201, dependencies=[Depends(require_api_key)])
def post_data(reading: Reading):
    conn = get_db()
    try:
        conn.execute(
            """INSERT INTO readings
               (ts, pm1_0_std, pm2_5_std, pm10_std,
                pm1_0_atm, pm2_5_atm, pm10_atm,
                cnt_0_3um, cnt_0_5um, cnt_1_0um,
                cnt_2_5um, cnt_5_0um, cnt_10um)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (
                time.time(),
                reading.pm1_0_std, reading.pm2_5_std, reading.pm10_std,
                reading.pm1_0_atm, reading.pm2_5_atm, reading.pm10_atm,
                reading.cnt_0_3um, reading.cnt_0_5um, reading.cnt_1_0um,
                reading.cnt_2_5um, reading.cnt_5_0um, reading.cnt_10um,
            ),
        )
        # Prune oldest rows when over MAX_RECORDS
        conn.execute(
            """DELETE FROM readings WHERE id IN (
                   SELECT id FROM readings ORDER BY ts ASC
                   LIMIT MAX(0, (SELECT COUNT(*) FROM readings) - ?)
               )""",
            (MAX_RECORDS,),
        )
        conn.commit()
    finally:
        conn.close()
    return {"status": "ok"}


@app.get("/api/data", dependencies=[Depends(require_any_auth)])
def get_data(limit: int = 500, hours: Optional[float] = None):
    conn = get_db()
    try:
        if hours is not None:
            since = time.time() - hours * 3600
            rows = conn.execute(
                "SELECT * FROM readings WHERE ts >= ? ORDER BY ts DESC LIMIT ?",
                (since, limit),
            ).fetchall()
        else:
            rows = conn.execute(
                "SELECT * FROM readings ORDER BY ts DESC LIMIT ?", (limit,)
            ).fetchall()
    finally:
        conn.close()

    return [
        {
            "id":        r["id"],
            "ts":        r["ts"],
            "ts_iso":    datetime.fromtimestamp(r["ts"], tz=timezone.utc).isoformat(),
            "pm1_0_std": r["pm1_0_std"],
            "pm2_5_std": r["pm2_5_std"],
            "pm10_std":  r["pm10_std"],
            "pm1_0_atm": r["pm1_0_atm"],
            "pm2_5_atm": r["pm2_5_atm"],
            "pm10_atm":  r["pm10_atm"],
            "cnt_0_3um": r["cnt_0_3um"],
            "cnt_0_5um": r["cnt_0_5um"],
            "cnt_1_0um": r["cnt_1_0um"],
            "cnt_2_5um": r["cnt_2_5um"],
            "cnt_5_0um": r["cnt_5_0um"],
            "cnt_10um":  r["cnt_10um"],
        }
        for r in rows
    ]


@app.get("/api/config", dependencies=[Depends(require_any_auth)])
def get_config():
    conn = get_db()
    try:
        row = conn.execute(
            "SELECT value FROM config WHERE key='interval_sec'"
        ).fetchone()
    finally:
        conn.close()
    return {"interval_sec": int(row["value"]) if row else 1800}


@app.put("/api/config", dependencies=[Depends(require_any_auth)])
def put_config(update: ConfigUpdate):
    conn = get_db()
    try:
        conn.execute(
            "INSERT OR REPLACE INTO config (key, value) VALUES ('interval_sec', ?)",
            (str(update.interval_sec),),
        )
        conn.commit()
    finally:
        conn.close()
    return {"interval_sec": update.interval_sec}


@app.get("/", response_class=HTMLResponse)
def dashboard(username: str = Depends(require_basic_auth)):
    html_path = Path(__file__).parent.parent / "frontend" / "index.html"
    if html_path.exists():
        return HTMLResponse(content=html_path.read_text(encoding="utf-8"))
    raise HTTPException(404, "Dashboard not found")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run("app:app", host="0.0.0.0", port=8000, reload=True)
