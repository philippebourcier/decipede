// Non-blocking status web server on port 80 (core 0).
//   /             HTML status page (auto-refresh 45 s), same as base.py
//   /status.json  the same data as JSON
#pragma once

void http_server_poll(void);
