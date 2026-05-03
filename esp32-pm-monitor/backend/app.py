"""
ESP32 PM Monitor — Backend
FastAPI + Neon (PostgreSQL) via psycopg2

Endpoints
─────────
  POST /api/data          ESP32 posts sensor readings   (X-API-Key required)
  GET  /api/data          Historical readings            (public)
  GET  /api/config        Current sampling interval      (public)
  PUT  /api/config        Update config from dashboard   (Basic Auth required)

Frontend
────────
  Served statically by Vercel at /  (frontend/index.html)

Environment variables
─────────────────────
  DATABASE_URL  Neon connection string (postgresql://user:pass@host/db?sslmode=require)
  API_KEY       Secret key header used by ESP32     (default: changeme)
  DASH_USER     Dashboard Basic Auth username        (default: admin)
  DASH_PASS     Dashboard Basic Auth password        (default: changeme)
  MAX_RECORDS   Max rows kept in DB                  (default: 10000)
"""

import os
import secrets
import ssl
import time
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from typing import Optional
from urllib.parse import urlparse

import pg8000.dbapi2

from fastapi import Depends, FastAPI, HTTPException, Request, status
from fastapi.security import HTTPBasic, HTTPBasicCredentials
from pydantic import BaseModel, Field

# ─── Config ───────────────────────────────────────────────────────────────────
# Vercel's Neon integration sets POSTGRES_URL; fall back to DATABASE_URL
DATABASE_URL = (
    os.getenv("POSTGRES_URL") or
    os.getenv("DATABASE_URL") or
    ""
)
API_KEY      = os.getenv("API_KEY",     "changeme")
DASH_USER    = os.getenv("DASH_USER",   "admin")
DASH_PASS    = os.getenv("DASH_PASS",   "changeme")
MAX_RECORDS  = int(os.getenv("MAX_RECORDS", "10000"))

# ─── Database ─────────────────────────────────────────────────────────────────
def _conn_params(url: str) -> dict:
    r = urlparse(url)
    params: dict = {
        "host":     r.hostname,
        "port":     r.port or 5432,
        "user":     r.username,
        "password": r.password,
        "database": r.path.lstrip("/"),
    }
    # Neon (and most managed Postgres) require SSL
    if r.hostname:
        ctx = ssl.create_default_context()
        params["ssl_context"] = ctx
    return params


def get_db() -> pg8000.dbapi2.Connection:
    return pg8000.dbapi2.connect(**_conn_params(DATABASE_URL))


def _as_dicts(cursor) -> list[dict]:
    cols = [d[0] for d in cursor.description]
    return [dict(zip(cols, row)) for row in cursor.fetchall()]


def init_db():
    conn = get_db()
    cur = conn.cursor()
    cur.execute("""
        CREATE TABLE IF NOT EXISTS readings (
            id          SERIAL PRIMARY KEY,
            ts          DOUBLE PRECISION NOT NULL,
            pm1_0_std   INTEGER,
            pm2_5_std   INTEGER,
            pm10_std    INTEGER,
            pm1_0_atm   INTEGER,
            pm2_5_atm   INTEGER,
            pm10_atm    INTEGER,
            cnt_0_3um   INTEGER,
            cnt_0_5um   INTEGER,
            cnt_1_0um   INTEGER,
            cnt_2_5um   INTEGER,
            cnt_5_0um   INTEGER,
            cnt_10um    INTEGER
        )
    """)
    cur.execute("""
        CREATE TABLE IF NOT EXISTS config (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        )
    """)
    cur.execute("""
        INSERT INTO config (key, value) VALUES ('interval_sec', '1800')
        ON CONFLICT (key) DO NOTHING
    """)
    conn.commit()
    cur.close()
    conn.close()


# ─── FastAPI ──────────────────────────────────────────────────────────────────
@asynccontextmanager
async def lifespan(app: FastAPI):
    try:
        init_db()
    except Exception as e:
        print(f"[DB] init_db failed: {e}")
    yield

app    = FastAPI(title="PM Monitor", lifespan=lifespan)
_basic = HTTPBasic(auto_error=False)

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
    cnt_10um:  Optional[int] = Field(None, ge=0)


class ConfigUpdate(BaseModel):
    interval_sec: int = Field(..., ge=10, le=86400)


# ─── Routes ───────────────────────────────────────────────────────────────────

@app.post("/api/data", status_code=201, dependencies=[Depends(require_api_key)])
def post_data(reading: Reading):
    conn = get_db()
    try:
        cur = conn.cursor()
        cur.execute(
            """INSERT INTO readings
               (ts, pm1_0_std, pm2_5_std, pm10_std,
                pm1_0_atm, pm2_5_atm, pm10_atm,
                cnt_0_3um, cnt_0_5um, cnt_1_0um,
                cnt_2_5um, cnt_5_0um, cnt_10um)
               VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)""",
            (
                time.time(),
                reading.pm1_0_std, reading.pm2_5_std, reading.pm10_std,
                reading.pm1_0_atm, reading.pm2_5_atm, reading.pm10_atm,
                reading.cnt_0_3um, reading.cnt_0_5um, reading.cnt_1_0um,
                reading.cnt_2_5um, reading.cnt_5_0um, reading.cnt_10um,
            ),
        )
        # Keep only the most recent MAX_RECORDS rows
        cur.execute(
            """DELETE FROM readings WHERE id NOT IN (
                   SELECT id FROM readings ORDER BY ts DESC LIMIT %s
               )""",
            (MAX_RECORDS,),
        )
        conn.commit()
        cur.close()
    finally:
        conn.close()
    return {"status": "ok"}


@app.get("/api/data")
def get_data(limit: int = 500, hours: Optional[float] = None):
    conn = get_db()
    try:
        cur = conn.cursor()
        if hours is not None:
            since = time.time() - hours * 3600
            cur.execute(
                "SELECT * FROM readings WHERE ts >= %s ORDER BY ts DESC LIMIT %s",
                (since, limit),
            )
        else:
            cur.execute(
                "SELECT * FROM readings ORDER BY ts DESC LIMIT %s", (limit,)
            )
        rows = _as_dicts(cur)
        cur.close()
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


@app.get("/api/config")
def get_config():
    conn = get_db()
    try:
        cur = conn.cursor()
        cur.execute("SELECT value FROM config WHERE key='interval_sec'")
        row = cur.fetchone()
        cur.close()
    finally:
        conn.close()
    return {"interval_sec": int(row[0]) if row else 1800}


@app.put("/api/config", dependencies=[Depends(require_basic_auth)])
def put_config(update: ConfigUpdate):
    conn = get_db()
    try:
        cur = conn.cursor()
        cur.execute(
            "INSERT INTO config (key, value) VALUES ('interval_sec', %s) ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value",
            (str(update.interval_sec),),
        )
        conn.commit()
        cur.close()
    finally:
        conn.close()
    return {"interval_sec": update.interval_sec}


if __name__ == "__main__":
    import uvicorn
    uvicorn.run("app:app", host="0.0.0.0", port=8000, reload=True)
