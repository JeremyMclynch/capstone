"""
SQLite database layer for UWB distance measurements.
"""

import sqlite3
import os
import logging
from typing import Optional

logger = logging.getLogger(__name__)

DB_PATH = os.environ.get("UWB_DB_PATH", "uwb_measurements.db")

SCHEMA = """
CREATE TABLE IF NOT EXISTS measurements (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    anchor_id   INTEGER NOT NULL,
    tag_id      INTEGER NOT NULL,
    distance_mm REAL    NOT NULL,
    distance_m  REAL    GENERATED ALWAYS AS (distance_mm / 1000.0) VIRTUAL,
    node_uptime_s INTEGER,
    rssi_dbm    REAL,
    fp_power_dbm REAL,
    fp_index    REAL,
    peak_index  INTEGER,
    received_at TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_measurements_anchor_tag
    ON measurements (anchor_id, tag_id);

CREATE INDEX IF NOT EXISTS idx_measurements_received_at
    ON measurements (received_at);

CREATE VIEW IF NOT EXISTS latest_distances AS
    SELECT
        anchor_id,
        tag_id,
        distance_m,
        rssi_dbm,
        fp_power_dbm,
        fp_index,
        peak_index,
        received_at
    FROM measurements m1
    WHERE received_at = (
        SELECT MAX(received_at)
        FROM measurements m2
        WHERE m2.anchor_id = m1.anchor_id
          AND m2.tag_id = m1.tag_id
    );
"""


def get_connection() -> sqlite3.Connection:
    """Open and return a SQLite connection with WAL mode for concurrency."""
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")
    return conn


def init_db() -> None:
    """Create database tables and indexes if they do not exist."""
    with get_connection() as conn:
        conn.executescript(SCHEMA)
    logger.info("Database initialized at: %s", os.path.abspath(DB_PATH))


def insert_measurement(
    anchor_id: int,
    tag_id: int,
    distance_mm: float,
    node_uptime_s: Optional[int] = None,
    rssi_dbm: Optional[float] = None,
    fp_power_dbm: Optional[float] = None,
    fp_index: Optional[float] = None,
    peak_index: Optional[int] = None,
) -> int:
    """
    Insert a distance measurement record.

    Returns the rowid of the inserted record.
    """
    with get_connection() as conn:
        cursor = conn.execute(
            """
            INSERT INTO measurements (anchor_id, tag_id, distance_mm, node_uptime_s,
                                      rssi_dbm, fp_power_dbm, fp_index, peak_index)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (anchor_id, tag_id, distance_mm, node_uptime_s,
             rssi_dbm, fp_power_dbm, fp_index, peak_index),
        )
        return cursor.lastrowid


def get_recent_measurements(limit: int = 100):
    """Return the most recent distance measurements."""
    with get_connection() as conn:
        rows = conn.execute(
            """
            SELECT anchor_id, tag_id, distance_m,
                   rssi_dbm, fp_power_dbm, fp_index, peak_index,
                   received_at
            FROM measurements
            ORDER BY received_at DESC
            LIMIT ?
            """,
            (limit,),
        ).fetchall()
    return [dict(r) for r in rows]


def get_latest_per_pair():
    """Return the most recent measurement for each anchor-tag pair."""
    with get_connection() as conn:
        rows = conn.execute(
            """SELECT anchor_id, tag_id, distance_m,
                      rssi_dbm, fp_power_dbm, fp_index, peak_index,
                      received_at
               FROM latest_distances"""
        ).fetchall()
    return [dict(r) for r in rows]
