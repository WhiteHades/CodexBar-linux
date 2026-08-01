#pragma once

int codexbar_serve_run(const char *host,
                       unsigned int port,
                       double refresh_interval,
                       double request_timeout,
                       const char *dashboard_token);
