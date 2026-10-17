import os
import sys
import re
import json
import urllib.parse
import urllib.request
import concurrent.futures
from http.server import HTTPServer, ThreadingHTTPServer, BaseHTTPRequestHandler
import socket

# Окружение и пути
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
CACHE_DIR = os.path.join(BASE_DIR, "yandex_cache")
COVERS_DIR = os.path.join(BASE_DIR, "covers_cache")
TOKEN_FILE = os.path.join(BASE_DIR, "ym_token.txt")
os.makedirs(CACHE_DIR, exist_ok=True)
os.makedirs(COVERS_DIR, exist_ok=True)

try:
    from yandex_music import Client
    import mutagen
    from mutagen.mp3 import MP3
    from mutagen.id3 import ID3, TIT2, TPE1, TALB, APIC, ID3NoHeaderError
except ImportError as e:
    print(f"Error importing dependencies: {e}", file=sys.stderr)
    sys.exit(1)

g_client = None
g_is_authenticated = False
g_user_name = ""

def find_saved_token() -> str:
    candidates = [
        TOKEN_FILE,
        os.path.join(os.path.dirname(BASE_DIR), "ym_token.txt"),
        os.path.join(BASE_DIR, "..", "cmake-build-release", "ym_token.txt"),
        os.path.join(BASE_DIR, "..", "cmake-build-debug", "ym_token.txt"),
    ]
    for p in candidates:
        if os.path.exists(p):
            try:
                with open(p, "r", encoding="utf-8") as f:
                    t = f.read().strip()
                    if t:
                        return t
            except Exception:
                pass
    return ""

def init_yandex_client():
    global g_client, g_is_authenticated, g_user_name
    token = find_saved_token()

    if token:
        try:
            c = Client(token).init()
            g_client = c
            g_is_authenticated = True
            name = ""
            if hasattr(c, 'me') and c.me and hasattr(c.me, 'account'):
                name = c.me.account.full_name or c.me.account.login or ""
            g_user_name = name or "Yandex User"
            print(f"[ym_bridge] Authenticated as: {g_user_name}")
            return
        except Exception as e:
            print(f"[ym_bridge] Saved token failed validation: {e}. Falling back to guest mode.")

    try:
        g_client = Client().init()
        g_is_authenticated = False
        g_user_name = ""
        print("[ym_bridge] Initialized in guest mode.")
    except Exception as e:
        print(f"[ym_bridge] Failed to initialize guest client: {e}")

init_yandex_client()

def get_track_cover_bytes(track, size='400x400'):
    # 1. Обложка самого трека через клиент
    try:
        if hasattr(track, 'download_cover_bytes'):
            b = track.download_cover_bytes(size)
            if b and len(b) > 300:
                return b
    except Exception:
        pass

    # 2. Обложка первого альбома трека
    if getattr(track, 'albums', None):
        for a in track.albums:
            try:
                if hasattr(a, 'download_cover_bytes'):
                    b = a.download_cover_bytes(size)
                    if b and len(b) > 300:
                        return b
            except Exception:
                pass

    # 3. Прямой URL через Yandex CDN
    uri = getattr(track, 'cover_uri', None)
    if not uri and getattr(track, 'albums', None) and len(track.albums) > 0:
        uri = getattr(track.albums[0], 'cover_uri', None)

    if uri:
        try:
            url = "https://" + uri.replace("%%", size)
            req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
            b = urllib.request.urlopen(req, timeout=5.0).read()
            if b and len(b) > 300:
                return b
        except Exception:
            pass

    return None

def download_and_tag_track(track_id: str) -> dict:
    global g_client
    if not g_client:
        init_yandex_client()

    final_path = os.path.join(CACHE_DIR, f"ym_{track_id}.mp3")
    cover_path = os.path.join(COVERS_DIR, f"ym_cover_{track_id}.jpg")

    tracks = g_client.tracks([track_id])
    if not tracks:
        raise RuntimeError(f"Трек с ID {track_id} не найден в Яндекс Музыке")
    track = tracks[0]

    artists = [a.name for a in track.artists if a.name] if track.artists else []
    artist_str = ", ".join(artists) if artists else "Неизвестный исполнитель"
    album_str = track.albums[0].title if track.albums and track.albums[0].title else ""
    title_str = track.title or "Без названия"

    dur_sec = int(round((getattr(track, 'duration_ms', 0) or 0) / 1000.0))

    # Если трек уже скачан и корректно тэгирован, возвращаем кэш
    if os.path.exists(final_path) and os.path.getsize(final_path) > 20000:
        try:
            check_id3 = ID3(final_path)
            if 'TIT2' in check_id3 and any(k.startswith('APIC') for k in check_id3):
                return {
                    "path": os.path.abspath(final_path),
                    "cover_path": os.path.abspath(cover_path) if os.path.exists(cover_path) else "",
                    "title": title_str,
                    "artist": artist_str,
                    "album": album_str,
                    "duration": dur_sec
                }
        except Exception:
            pass

    # Ищем наилучший доступный поток (приоритет MP3 с максимальным битрейтом)
    infos = track.get_download_info()
    if not infos:
        raise RuntimeError("Нет доступных ссылок для загрузки трека")

    sorted_infos = sorted(infos, key=lambda x: (x.codec == 'mp3', x.bitrate_in_kbps or 0), reverse=True)
    best = sorted_infos[0]

    tmp_path = os.path.join(CACHE_DIR, f"ym_{track_id}.tmp")
    if os.path.exists(tmp_path):
        try: os.remove(tmp_path)
        except Exception: pass

    # Скачиваем аудиофайл с правильными параметрами битрейта
    track.download(tmp_path, codec=best.codec, bitrate_in_kbps=best.bitrate_in_kbps)

    # Скачиваем обложку (400x400)
    cover_bytes = get_track_cover_bytes(track, '400x400')

    # Вшиваем ID3-теги и APIC обложку через mutagen
    try:
        tags = ID3()
        tags.add(TIT2(encoding=3, text=title_str))
        tags.add(TPE1(encoding=3, text=artist_str))
        if album_str:
            tags.add(TALB(encoding=3, text=album_str))
        if cover_bytes:
            tags.add(APIC(
                encoding=3,
                mime='image/jpeg',
                type=3,  # Front cover
                desc='Cover',
                data=cover_bytes
            ))
        tags.save(tmp_path, v2_version=3)
    except Exception as e:
        print(f"[ym_bridge] Warning: ID3 tagging failed: {e}")

    # Сохраняем обложку на диск для прямого доступа C++ OpenGL и SMTC
    saved_cover_path = ""
    try:
        if cover_bytes:
            # 1. Прямой файл по ID трека
            with open(cover_path, "wb") as cf:
                cf.write(cover_bytes)
            saved_cover_path = os.path.abspath(cover_path)

            # 2. Также сохраняем по FNV-1a (64-bit) для CoverFetcher кэша
            s = f"{artist_str}__{title_str}".lower()
            h = 14695981039346656037
            for ch in s:
                if ch.isalnum():
                    h ^= ord(ch)
                    h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
            hash_str = f"{h:016x}"
            fnv_path = os.path.join(COVERS_DIR, f"{hash_str}.jpg")
            with open(fnv_path, "wb") as cf:
                cf.write(cover_bytes)
    except Exception as e:
        print(f"[ym_bridge] Warning: saving cover cache failed: {e}")

    # Атомарно переименовываем во фронтальный mp3
    if os.path.exists(final_path):
        try: os.remove(final_path)
        except Exception: pass
    os.rename(tmp_path, final_path)

    return {
        "path": os.path.abspath(final_path),
        "cover_path": saved_cover_path,
        "title": title_str,
        "artist": artist_str,
        "album": album_str,
        "duration": dur_sec
    }

class YmBridgeHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def send_json(self, data, status_code=200):
        body = json.dumps(data, ensure_ascii=False).encode('utf-8')
        self.send_response(status_code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        global g_client, g_is_authenticated, g_user_name

        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        if path == "/status":
            self.send_json({
                "status": "ok",
                "authenticated": g_is_authenticated,
                "user": g_user_name,
                "token_exists": os.path.exists(TOKEN_FILE)
            })
            return

        elif path == "/auth":
            token_val = query.get("token", [None])[0] or query.get("code", [None])[0]
            if not token_val:
                self.send_json({"status": "error", "message": "Токен не передан"}, 400)
                return

            match = re.search(r'access_token=([^&]+)', token_val)
            if match:
                token_val = match.group(1)

            token_val = token_val.strip()

            try:
                c = Client(token_val).init()
                g_client = c
                g_is_authenticated = True
                name = ""
                if hasattr(c, 'me') and c.me and hasattr(c.me, 'account'):
                    name = c.me.account.full_name or c.me.account.login or ""
                g_user_name = name or "Yandex User"

                save_paths = {
                    TOKEN_FILE,
                    os.path.join(os.path.dirname(BASE_DIR), "ym_token.txt"),
                    os.path.join(BASE_DIR, "..", "cmake-build-release", "ym_token.txt"),
                    os.path.join(BASE_DIR, "..", "cmake-build-debug", "ym_token.txt")
                }
                for sp in save_paths:
                    try:
                        with open(sp, "w", encoding="utf-8") as f:
                            f.write(token_val)
                    except Exception:
                        pass

                print(f"[ym_bridge] New login successful: {g_user_name}")
                self.send_json({
                    "status": "ok",
                    "authenticated": True,
                    "user": g_user_name
                })
            except Exception as e:
                self.send_json({
                    "status": "error",
                    "message": f"Ошибка авторизации: {e}"
                }, 400)
            return

        elif path == "/logout":
            for sp in {
                TOKEN_FILE,
                os.path.join(os.path.dirname(BASE_DIR), "ym_token.txt"),
                os.path.join(BASE_DIR, "..", "cmake-build-release", "ym_token.txt"),
                os.path.join(BASE_DIR, "..", "cmake-build-debug", "ym_token.txt")
            }:
                try:
                    if os.path.exists(sp):
                        os.remove(sp)
                except Exception:
                    pass
            init_yandex_client()
            self.send_json({"status": "ok", "authenticated": False})
            return

        elif path == "/search":
            q = query.get("q", [""])[0].strip()
            if not q:
                self.send_json([])
                return

            try:
                if not g_client:
                    init_yandex_client()

                res = g_client.search(q, type_='track')
                results = []
                if res and res.tracks and res.tracks.results:
                    raw_tracks = res.tracks.results[:30]

                    def get_thumb_for_track(t):
                        tid = str(t.id)
                        t_path = os.path.join(COVERS_DIR, f"ym_thumb_{tid}.jpg")
                        if os.path.exists(t_path) and os.path.getsize(t_path) > 300:
                            return (tid, os.path.abspath(t_path))
                        u = None
                        if getattr(t, 'cover_uri', None):
                            u = t.get_cover_url('100x100')
                        elif getattr(t, 'albums', None) and len(t.albums) > 0 and getattr(t.albums[0], 'cover_uri', None):
                            u = t.albums[0].get_cover_url('100x100')
                        if u:
                            try:
                                full_u = 'https://' + u if not u.startswith('http') else u
                                req = urllib.request.Request(full_u, headers={'User-Agent': 'Mozilla/5.0'})
                                data = urllib.request.urlopen(req, timeout=3.0).read()
                                if data and len(data) > 300:
                                    with open(t_path, "wb") as f:
                                        f.write(data)
                                    return (tid, os.path.abspath(t_path))
                            except Exception:
                                pass
                        return (tid, "")

                    thumbs_map = {}
                    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
                        for tid, tp in ex.map(get_thumb_for_track, raw_tracks):
                            thumbs_map[tid] = tp

                    for t in raw_tracks:
                        tid = str(t.id)
                        artists = [a.name for a in t.artists if a.name] if t.artists else []
                        artist_str = ", ".join(artists) if artists else "Неизвестный исполнитель"
                        album_str = t.albums[0].title if t.albums and t.albums[0].title else ""
                        duration_sec = int((t.duration_ms or 0) / 1000)

                        results.append({
                            "id": tid,
                            "title": t.title or "Без названия",
                            "artist": artist_str,
                            "album": album_str,
                            "duration": duration_sec,
                            "thumb_path": thumbs_map.get(tid, "")
                        })

                self.send_json(results)
            except Exception as e:
                self.send_json({"status": "error", "message": str(e)}, 500)
            return

        elif path == "/download":
            track_id = query.get("id", [""])[0].strip()
            if not track_id:
                self.send_json({"status": "error", "message": "ID трека не передан"}, 400)
                return

            try:
                info = download_and_tag_track(track_id)
                self.send_json({
                    "status": "ok",
                    "path": info["path"],
                    "cover_path": info.get("cover_path", ""),
                    "title": info.get("title", ""),
                    "artist": info.get("artist", ""),
                    "album": info.get("album", ""),
                    "duration": info.get("duration", 0)
                })
            except Exception as e:
                self.send_json({"status": "error", "message": str(e)}, 500)
            return

        else:
            self.send_json({"status": "error", "message": "Неизвестный эндпоинт"}, 404)

def is_already_running(port=19840) -> bool:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(0.5)
        s.connect(('127.0.0.1', port))
        s.close()
        return True
    except Exception:
        return False

def main():
    if is_already_running(19840):
        print("[ym_bridge] Another instance is already running on port 19840.")
        sys.exit(0)

    server = ThreadingHTTPServer(('127.0.0.1', 19840), YmBridgeHandler)
    print("[ym_bridge] Server listening on http://127.0.0.1:19840")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()

if __name__ == "__main__":
    main()
