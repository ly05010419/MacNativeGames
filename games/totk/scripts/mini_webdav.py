#!/usr/bin/env python3
"""A small WebDAV server for trying the save sync (games/totk/tests/save_sync/probe.m) without an account anywhere:
PROPFIND (Depth 1), GET, PUT, MKCOL, DELETE, Basic auth. Standard library only.

    python3 games/totk/scripts/mini_webdav.py --root /tmp/dav [--port 8442] [--user me --password secret]
"""
import argparse, base64, os, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlsplit

HREF_ESCAPES = (("&", "&amp;"), ("<", "&lt;"), (">", "&gt;"), ('"', "&quot;"))


def escape(text):
    for a, b in HREF_ESCAPES:
        text = text.replace(a, b)
    return text


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):   # one line, like the game's own logs
        sys.stderr.write("[mini-webdav] %s %s\n" % (self.address_string(), fmt % args))

    def drain_body(self):
        """Read and discard the request body: with keep-alive, unread bytes would be parsed as the next request."""
        try:
            length = int(self.headers.get("Content-Length", "0") or 0)
        except ValueError:
            return
        while length > 0:
            block = self.rfile.read(min(1 << 16, length))
            if not block:
                return
            length -= len(block)

    def path_local(self):
        return os.path.realpath(os.path.join(self.root, unquote(urlsplit(self.path).path).lstrip("/")))

    def authorized(self):
        if not self.user:
            return True
        header = self.headers.get("Authorization", "")
        want = "Basic " + base64.b64encode(f"{self.user}:{self.password}".encode()).decode()
        return header == want

    def deny(self):
        self.send_response(401)
        self.send_header("WWW-Authenticate", 'Basic realm="mini-webdav"')
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Allow", "OPTIONS, GET, PUT, PROPFIND, MKCOL, DELETE")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_PROPFIND(self):
        if not self.authorized():
            return self.deny()
        self.drain_body()
        local = self.path_local()
        if not os.path.exists(local):
            return self.no_content(404)
        depth = self.headers.get("Depth", "1")
        entries = []
        if os.path.isdir(local):
            entries.append((local, True, 0))
            if depth == "1":
                for name in sorted(os.listdir(local)):
                    child = os.path.join(local, name)
                    entries.append((child, os.path.isdir(child), os.path.getsize(child) if os.path.isfile(child) else 0))
        else:
            entries.append((local, False, os.path.getsize(local)))
        root = os.path.realpath(self.root)
        body = ['<?xml version="1.0" encoding="utf-8"?><D:multistatus xmlns:D="DAV:">']
        for child, is_dir, size in entries:
            href = "/" + os.path.relpath(child, root).replace(os.sep, "/")
            if is_dir:
                href += "/"
            body.append(f'<D:response><D:href>{escape(href)}</D:href><D:propstat><D:prop>'
                        f'{"<D:resourcetype><D:collection/></D:resourcetype>" if is_dir else "<D:resourcetype/>"}'
                        f'<D:getcontentlength>{size}</D:getcontentlength>'
                        f'</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>')
        body.append("</D:multistatus>")
        payload = "".join(body).encode()
        self.send_response(207)
        self.send_header("Content-Type", "application/xml; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def no_content(self, status):
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        if not self.authorized():
            return self.deny()
        local = self.path_local()
        if not os.path.isfile(local):
            return self.no_content(404)
        with open(local, "rb") as stream:
            payload = stream.read()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_PUT(self):
        if not self.authorized():
            return self.deny()
        local = self.path_local()
        os.makedirs(os.path.dirname(local), exist_ok=True)
        length = int(self.headers.get("Content-Length", "0"))
        with open(local, "wb") as stream:
            remaining = length
            while remaining > 0:
                block = self.rfile.read(min(1 << 16, remaining))
                if not block:
                    break
                stream.write(block)
                remaining -= len(block)
        self.no_content(201)

    def do_MKCOL(self):
        if not self.authorized():
            return self.deny()
        self.drain_body()
        local = self.path_local()
        if os.path.isdir(local):
            return self.no_content(405)
        os.makedirs(local, exist_ok=True)
        self.no_content(201)

    def do_DELETE(self):
        if not self.authorized():
            return self.deny()
        self.drain_body()
        local = self.path_local()
        inside = os.path.realpath(local).startswith(os.path.realpath(self.root) + os.sep)
        if not inside or not os.path.exists(local) or os.path.realpath(local) == os.path.realpath(self.root):
            return self.no_content(404)
        import shutil
        shutil.rmtree(local) if os.path.isdir(local) else os.unlink(local)
        self.no_content(204)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, help="the folder served")
    parser.add_argument("--port", type=int, default=8442)
    parser.add_argument("--user", default="")
    parser.add_argument("--password", default="")
    args = parser.parse_args()
    os.makedirs(args.root, exist_ok=True)
    Handler.root, Handler.user, Handler.password = os.path.realpath(args.root), args.user, args.password
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"mini WebDAV on http://127.0.0.1:{args.port}/ serving {Handler.root}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
