"""Serves the cutoff comparison, rebuilding it every few seconds so the page is always current.

The static index only changed when someone ran the builder; this runs the builder on a timer and
serves the result, so a browser (or its own meta refresh) always shows the files finished so far.

usage: cutoff_server.py <batchDir> [<siteDir>] [<port>]
"""
import functools
import http.server
import os
import socketserver
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cutoff_report
import cutoff_site


def main():
    batch = os.path.abspath(sys.argv[1])
    site = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser('~/ae_cutoff'))
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 8099
    os.makedirs(site, exist_ok=True)

    def rebuild_loop():
        while True:
            try:
                # crops once per file, then the page itself
                for name in sorted(os.listdir(batch)):
                    d = os.path.join(batch, name)
                    if not os.path.isdir(d) or cutoff_report.has_crops(batch, name):
                        continue
                    baseline = next((v for v in ('const10', 'off')
                                     if os.path.exists(os.path.join(d, v, 'render_straight.png'))), None)
                    if baseline:
                        cutoff_report.report(batch, [name])
                cutoff_site.build(batch, site)
            except Exception as e:  # a half-written render must not kill the server
                print('rebuild failed: %s' % e, flush=True)
            time.sleep(10)

    threading.Thread(target=rebuild_loop, daemon=True).start()

    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=site)

    class Server(socketserver.ThreadingTCPServer):
        # SO_REUSEADDR must be on the CLASS: setting it after the constructor is too late, the bind
        # has already happened (that is how a restart after a kill fails with EADDRINUSE)
        allow_reuse_address = True
        daemon_threads = True

    with Server(('127.0.0.1', port), handler) as httpd:
        print('serving %s on http://127.0.0.1:%d/' % (site, port), flush=True)
        httpd.serve_forever()


if __name__ == '__main__':
    main()
