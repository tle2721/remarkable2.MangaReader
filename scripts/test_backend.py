#!/usr/bin/env python3
"""
End-to-end local integration test for the rm2manga backend.
Simulates both:
  1. The Aidoku HTTP-over-Unix-Domain-Socket server (/tmp/rakuyomi.sock)
  2. The rm-appload coordinator IPC socket
and verifies zero-hardcoded-sources bootstrap, user-provided source_lists JSON
configuration, source installation, per-source discover & search, library toggle,
manga details, CBZ extraction & natural page sort, bookmark persistence,
background next-chapter preload, mark-as-read, swipe settings, and clean exit.
"""

import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import zipfile
from pathlib import Path

# Minimal valid 1x1 grayscale PNG
MINI_PNG = (
    b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01"
    b"\x08\x00\x00\x00\x00:~\x9bU\x00\x00\x00\nIDATx\x9cc`\x00\x00\x00\x02"
    b"\x00\x01H\xaf\xa4q\x00\x00\x00\x00IEND\xaeB`\x82"
)

MSG_SYSTEM_TERMINATE = -1
MSG_SYSTEM_NEW_COORDINATOR = -2

REQ_SEARCH = 3
REQ_MANGA_DETAILS = 4
REQ_TOGGLE_LIBRARY = 5
REQ_OPEN_CHAPTER = 6
REQ_SAVE_PROGRESS = 7
REQ_INSTALL_SOURCE = 13
REQ_UPDATE_SETTINGS = 14
REQ_SAVE_SOURCE_CONFIG = 15

EVT_BOOTSTRAP = 100
EVT_LIBRARY_UPDATED = 101
EVT_SEARCH_RESULTS = 102
EVT_MANGA_DETAILS = 103
EVT_CHAPTER_PAGES = 105
EVT_SOURCES_LIST = 107


def make_cbz(path: Path, filenames: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as zf:
        for name in filenames:
            zf.writestr(name, MINI_PNG)


class MockAidokuServer:
    def __init__(self, sock_path: str, aidoku_home: Path, ch1_cbz: Path, ch2_cbz: Path):
        self.sock_path = sock_path
        self.aidoku_home = aidoku_home
        self.ch1_cbz = str(ch1_cbz)
        self.ch2_cbz = str(ch2_cbz)
        self.installed_sources: list[dict] = []
        self.library: list[dict] = []
        self.downloaded_jobs: dict[str, str] = {}
        self.download_requests: list[str] = []
        self.marked_read: list[str] = []
        self._stop = threading.Event()
        self._srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        if os.path.exists(sock_path):
            os.unlink(sock_path)
        self._srv.bind(sock_path)
        self._srv.listen(16)
        self._srv.settimeout(0.2)
        self._thread = threading.Thread(target=self._serve, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)
        self._srv.close()
        if os.path.exists(self.sock_path):
            os.unlink(self.sock_path)

    def _serve(self) -> None:
        while not self._stop.is_set():
            try:
                conn, _ = self._srv.accept()
            except TimeoutError:
                continue
            except OSError:
                break
            threading.Thread(target=self._handle_conn, args=(conn,), daemon=True).start()

    def _handle_conn(self, conn: socket.socket) -> None:
        with conn:
            conn.settimeout(2.0)
            raw = b""
            while b"\r\n\r\n" not in raw:
                chunk = conn.recv(4096)
                if not chunk:
                    return
                raw += chunk
            head, rest = raw.split(b"\r\n\r\n", 1)
            lines = head.decode("utf-8", errors="replace").split("\r\n")
            req_line = lines[0].split(" ")
            method = req_line[0]
            full_target = req_line[1]
            parsed = urllib.parse.urlsplit(full_target)
            path = parsed.path
            query_params = urllib.parse.parse_qs(parsed.query)

            content_len = 0
            for line in lines[1:]:
                if line.lower().startswith("content-length:"):
                    content_len = int(line.split(":", 1)[1].strip())
            while len(rest) < content_len:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                rest += chunk
            body_text = rest[:content_len].decode("utf-8", errors="replace")

            status, payload = self._route(method, path, query_params, body_text)
            body_bytes = json.dumps(payload).encode("utf-8")
            resp = (
                f"HTTP/1.1 {status} OK\r\n"
                f"Content-Type: application/json\r\n"
                f"Content-Length: {len(body_bytes)}\r\n"
                f"Connection: close\r\n\r\n"
            ).encode("utf-8") + body_bytes
            conn.sendall(resp)

    def _read_settings(self) -> dict:
        cfg_file = self.aidoku_home / "settings.json"
        if cfg_file.is_file():
            try:
                return json.loads(cfg_file.read_text())
            except Exception:
                return {}
        return {}

    def _route(self, method: str, path: str, query_params: dict, body_text: str):
        if path == "/health-check":
            return 200, {"ok": True}
        if path == "/installed-sources":
            return 200, list(self.installed_sources)
        if path == "/available-sources":
            cfg = self._read_settings()
            if not cfg.get("source_lists"):
                return 200, []
            return 200, [
                {"id": "ext.alpha", "name": "Alpha Catalog", "version": 3},
                {"id": "ext.beta", "name": "Beta Reader", "version": 1},
            ]
        if method == "POST" and path.startswith("/available-sources/") and path.endswith("/install"):
            sid = path.split("/available-sources/", 1)[1].rsplit("/install", 1)[0]
            if not any(s["id"] == sid for s in self.installed_sources):
                name = "Alpha Catalog" if sid == "ext.alpha" else "Beta Reader"
                self.installed_sources.append({"id": sid, "name": name, "version": 3})
            return 200, {"ok": True}
        if path == "/mangas":
            q = (query_params.get("q", [""])[0] or "").lower()
            out = []
            installed_ids = {s["id"] for s in self.installed_sources}
            if "ext.alpha" in installed_ids and (not q or "lone" in q):
                out.append(
                    {
                        "id": "series_01",
                        "title": "Lone Wanderer",
                        "source": {"id": "ext.alpha", "name": "Alpha Catalog", "version": 3},
                    }
                )
            if "ext.beta" in installed_ids and (not q or "clock" in q):
                out.append(
                    {
                        "id": "series_02",
                        "title": "Clockwork Tower",
                        "source": {"id": "ext.beta", "name": "Beta Reader", "version": 1},
                    }
                )
            return 200, out
        if path == "/library":
            return 200, list(self.library)
        if method == "POST" and path == "/mangas/ext.alpha/series_01/add-to-library":
            self.library = [
                {
                    "id": "series_01",
                    "title": "Lone Wanderer",
                    "source": {"id": "ext.alpha", "name": "Alpha Catalog"},
                }
            ]
            return 200, {"ok": True}
        if path == "/mangas/ext.alpha/series_01/chapters":
            return 200, [
                {
                    "id": "ch_2",
                    "title": "Second Horizon",
                    "chapter_num": 2.0,
                    "downloaded": False,
                    "read": False,
                },
                {
                    "id": "ch_1",
                    "title": "First Step",
                    "chapter_num": 1.0,
                    "downloaded": True,
                    "read": False,
                },
            ]
        if method == "POST" and path == "/jobs/download-chapter":
            req = json.loads(body_text or "{}")
            cid = req.get("chapter_id", "")
            self.download_requests.append(cid)
            job_id = f"job_{cid}"
            self.downloaded_jobs[job_id] = self.ch1_cbz if cid == "ch_1" else self.ch2_cbz
            return 200, job_id
        if method == "GET" and path.startswith("/jobs/"):
            job_id = path.split("/jobs/", 1)[1]
            cbz = self.downloaded_jobs.get(job_id, self.ch1_cbz)
            return 200, {"type": "COMPLETED", "data": cbz}
        if method == "POST" and path.endswith("/mark-as-read"):
            self.marked_read.append(path)
            return 200, {"ok": True}
        return 200, {}


def send_ipc(sock: socket.socket, msg_type: int, payload_obj=None) -> None:
    raw = b"" if payload_obj is None else json.dumps(payload_obj).encode("utf-8")
    hdr = struct.pack("<ii", msg_type, len(raw))
    sock.sendall(hdr + raw)


def recv_exact(sock: socket.socket, n: int) -> bytes:
    out = b""
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise EOFError("Backend closed IPC socket unexpectedly")
        out += chunk
    return out


def recv_ipc(sock: socket.socket):
    hdr = recv_exact(sock, 8)
    msg_type, length = struct.unpack("<ii", hdr)
    payload = recv_exact(sock, length) if length > 0 else b""
    data = json.loads(payload.decode("utf-8")) if payload else None
    return msg_type, data


def wait_for_event(sock: socket.socket, predicate, timeout: float = 8.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        sock.settimeout(max(0.1, deadline - time.monotonic()))
        msg_type, data = recv_ipc(sock)
        if predicate(msg_type, data):
            return msg_type, data
    raise TimeoutError("Timed out waiting for expected IPC event")


def main() -> int:
    backend_bin = sys.argv[1] if len(sys.argv) > 1 else "/tmp/rm2manga_backend_host"
    if not os.path.isfile(backend_bin):
        print(f"Error: host backend binary not found at {backend_bin}", file=sys.stderr)
        return 1

    tmp_root = Path(tempfile.mkdtemp(prefix="rm2m_", dir="/tmp"))
    try:
        data_root = tmp_root / "app"
        aidoku_home = data_root / "data" / "aidoku"
        page_cache = tmp_root / "pages"
        aidoku_sock = str(tmp_root / "aidoku.sock")
        appload_sock = str(tmp_root / "appload.sock")

        ch1_cbz = aidoku_home / "downloads" / "ch1.cbz"
        ch2_cbz = aidoku_home / "downloads" / "ch2.cbz"
        make_cbz(ch1_cbz, ["page_10.png", "page_2.png", "page_1.png", "page_9.png"])
        make_cbz(ch2_cbz, ["001.png", "002.png", "003.png"])

        aidoku_srv = MockAidokuServer(aidoku_sock, aidoku_home, ch1_cbz, ch2_cbz)
        aidoku_srv.start()

        ipc_srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        ipc_srv.bind(appload_sock)
        ipc_srv.listen(1)
        ipc_srv.settimeout(5.0)

        env = os.environ.copy()
        env["RM2MANGA_DATA_ROOT"] = str(data_root)
        env["RM2MANGA_AIDOKU_SOCK"] = aidoku_sock
        env["RM2MANGA_AIDOKU_HOME"] = str(aidoku_home)
        env["RM2MANGA_PAGE_CACHE_DIR"] = str(page_cache)
        env["RM2MANGA_DISABLE_COVERS"] = "1"

        proc = subprocess.Popen([backend_bin, appload_sock], env=env)
        conn, _ = ipc_srv.accept()

        with conn:
            # 1. Initial Bootstrap -> verify zero hardcoded sources and empty source_lists
            send_ipc(conn, MSG_SYSTEM_NEW_COORDINATOR)
            _, boot = wait_for_event(
                conn,
                lambda t, d: t == EVT_BOOTSTRAP and d.get("aidoku_online") is True,
            )
            assert boot["installed_sources"] == [], (
                f"Expected zero pre-installed sources, got {boot['installed_sources']}"
            )
            assert boot["source_config"]["source_lists"] == [], (
                f"Expected empty default source_lists, got {boot['source_config']}"
            )
            print("[PASS] Zero hardcoded sources & empty default source_lists verified")

            # 2. Save user-provided raw_json source_lists config -> verify settings.json & available sources
            user_json = json.dumps(
                {
                    "source_lists": ["https://example.invalid/sources/index.min.json"],
                    "languages": ["en"],
                },
                indent=2,
            )
            send_ipc(conn, REQ_SAVE_SOURCE_CONFIG, {"raw_json": user_json})
            _, src_ev = wait_for_event(
                conn,
                lambda t, d: t == EVT_SOURCES_LIST and d.get("loading") is False and len(d.get("available", [])) == 2,
            )
            disk_cfg = json.loads((aidoku_home / "settings.json").read_text())
            assert disk_cfg["source_lists"] == ["https://example.invalid/sources/index.min.json"]
            assert disk_cfg["languages"] == ["en"]
            assert [s["id"] for s in src_ev["available"]] == ["ext.alpha", "ext.beta"]
            print("[PASS] User raw_json source_lists saved to settings.json & available sources fetched")

            # 3. Install user-chosen sources (ext.alpha & ext.beta) and test per-source Discover
            send_ipc(conn, REQ_INSTALL_SOURCE, {"source_id": "ext.alpha", "install": True})
            wait_for_event(
                conn,
                lambda t, d: t == EVT_SOURCES_LIST and any(s["id"] == "ext.alpha" for s in d.get("installed", [])),
            )
            send_ipc(conn, REQ_INSTALL_SOURCE, {"source_id": "ext.beta", "install": True})
            wait_for_event(
                conn,
                lambda t, d: t == EVT_SOURCES_LIST and len(d.get("installed", [])) == 2,
            )

            # Discover filtered to ext.alpha only
            send_ipc(
                conn,
                REQ_SEARCH,
                {"query": "", "source_filter": "ext.alpha", "mode": "discover", "force_refresh": True},
            )
            _, disc_alpha = wait_for_event(
                conn,
                lambda t, d: t == EVT_SEARCH_RESULTS and d.get("loading") is False,
            )
            assert [r["id"] for r in disc_alpha["results"]] == ["series_01"], (
                f"Expected only ext.alpha results, got {disc_alpha['results']}"
            )
            print("[PASS] Source installation & per-source Discover filtering verified")

            # 4. Add series_01 to Library and fetch Manga Details
            send_ipc(
                conn,
                REQ_TOGGLE_LIBRARY,
                {
                    "source_id": "ext.alpha",
                    "source_name": "Alpha Catalog",
                    "manga_id": "series_01",
                    "manga_title": "Lone Wanderer",
                    "add": True,
                },
            )
            wait_for_event(
                conn,
                lambda t, d: t == EVT_LIBRARY_UPDATED and len(d.get("library", [])) == 1,
            )

            send_ipc(
                conn,
                REQ_MANGA_DETAILS,
                {
                    "source_id": "ext.alpha",
                    "source_name": "Alpha Catalog",
                    "manga_id": "series_01",
                    "manga_title": "Lone Wanderer",
                },
            )
            _, details = wait_for_event(
                conn,
                lambda t, d: t == EVT_MANGA_DETAILS and d.get("loading") is False,
            )
            assert len(details["chapters"]) == 2
            print("[PASS] Library toggle & manga chapter details verified")

            # 5. Open Chapter 1 -> verify CBZ extraction, natural page order, and next_chapter link
            send_ipc(
                conn,
                REQ_OPEN_CHAPTER,
                {
                    "source_id": "ext.alpha",
                    "source_name": "Alpha Catalog",
                    "manga_id": "series_01",
                    "manga_title": "Lone Wanderer",
                    "chapter_id": "ch_1",
                    "chapter_num": 1.0,
                    "chapter_title": "First Step",
                },
            )
            _, ch_pages = wait_for_event(conn, lambda t, d: t == EVT_CHAPTER_PAGES)
            pages = ch_pages["pages"]
            assert len(pages) == 4, f"Expected 4 pages, got {pages}"
            basenames = [os.path.basename(p.replace("file://", "")) for p in pages]
            assert basenames == ["page_1.png", "page_2.png", "page_9.png", "page_10.png"], (
                f"Natural sort failed: {basenames}"
            )
            assert ch_pages["next_chapter"]["id"] == "ch_2"
            print("[PASS] Chapter CBZ extraction, natural page sort & next_chapter link verified")

            # 6. Save progress at page index 2 of 4 -> triggers automatic next-chapter preload
            send_ipc(
                conn,
                REQ_SAVE_PROGRESS,
                {
                    "source_id": "ext.alpha",
                    "source_name": "Alpha Catalog",
                    "manga_id": "series_01",
                    "manga_title": "Lone Wanderer",
                    "chapter_id": "ch_1",
                    "chapter_num": 1.0,
                    "chapter_title": "First Step",
                    "page_index": 2,
                    "page_count": 4,
                    "completed": False,
                },
            )
            _, lib_upd = wait_for_event(
                conn,
                lambda t, d: (
                    t == EVT_LIBRARY_UPDATED
                    and d.get("continue_reading", {}).get("last_page_index") == 2
                ),
            )
            assert lib_upd["continue_reading"]["last_chapter_id"] == "ch_1"

            deadline = time.monotonic() + 4.0
            while "ch_2" not in aidoku_srv.download_requests and time.monotonic() < deadline:
                time.sleep(0.05)
            assert "ch_2" in aidoku_srv.download_requests, "Background preload of ch_2 did not trigger!"
            print("[PASS] Continue Reading bookmark & automatic next-chapter preload verified")

            # 7. Complete chapter & persist invertSwipe setting
            send_ipc(
                conn,
                REQ_SAVE_PROGRESS,
                {
                    "source_id": "ext.alpha",
                    "source_name": "Alpha Catalog",
                    "manga_id": "series_01",
                    "manga_title": "Lone Wanderer",
                    "chapter_id": "ch_1",
                    "chapter_num": 1.0,
                    "chapter_title": "First Step",
                    "page_index": 3,
                    "page_count": 4,
                    "completed": True,
                },
            )
            deadline = time.monotonic() + 3.0
            while not aidoku_srv.marked_read and time.monotonic() < deadline:
                time.sleep(0.05)
            assert len(aidoku_srv.marked_read) == 1

            send_ipc(
                conn,
                REQ_UPDATE_SETTINGS,
                {"readingMode": "rtl", "fitMode": "contain", "invertSwipe": True},
            )
            _, upd_boot = wait_for_event(
                conn,
                lambda t, d: t == EVT_BOOTSTRAP and d.get("settings", {}).get("invertSwipe") is True,
            )
            assert upd_boot["settings"]["invertSwipe"] is True
            print("[PASS] Chapter completion mark-as-read & invertSwipe setting persistence verified")

            # 8. Terminate cleanly
            send_ipc(conn, MSG_SYSTEM_TERMINATE)

        exit_code = proc.wait(timeout=5.0)
        assert exit_code == 0, f"Backend exited with non-zero status {exit_code}"

        state_file = data_root / "data" / "state.json"
        assert state_file.is_file(), "state.json was not persisted"
        saved_state = json.loads(state_file.read_text())
        manga_entry = saved_state["mangas"]["ext.alpha::series_01"]
        assert manga_entry["last_chapter_id"] == "ch_1"
        assert manga_entry["last_page_index"] == 3
        assert saved_state["settings"]["invertSwipe"] is True
        print("[PASS] Clean shutdown (exit 0) & atomic state.json persistence verified")

        aidoku_srv.stop()
        ipc_srv.close()
        return 0
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
