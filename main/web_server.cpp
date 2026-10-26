#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "config.h"
#include "shared_state.h"
#include "web_server.h"

static const char *TAG = "web";

static const char *content_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    static const struct { const char *ext, *type; } map[] = {
        {".html", "text/html"}, {".js", "text/javascript"}, {".css", "text/css"},
        {".json", "application/json"}, {".webmanifest", "application/manifest+json"},
        {".svg", "image/svg+xml"}, {".png", "image/png"}, {".ico", "image/x-icon"},
        {".glsl", "text/plain"}, {".txt", "text/plain"},
    };
    for (auto &m : map)
        if (!strcmp(dot, m.ext)) return m.type;
    return "application/octet-stream";
}

static esp_err_t static_handler(httpd_req_t *req)
{
    char uri[128];
    strlcpy(uri, req->uri, sizeof uri);
    if (char *q = strchr(uri, '?')) *q = 0;
    if (strstr(uri, "..")) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, nullptr);
    if (uri[strlen(uri) - 1] == '/') strlcat(uri, "index.html", sizeof uri);

    char path[200];
    bool gz = true;
    snprintf(path, sizeof path, NET_MOUNT "%s.gz", uri);
    struct stat st;
    if (stat(path, &st) != 0) {
        gz = false;
        snprintf(path, sizeof path, NET_MOUNT "%s", uri);
        if (stat(path, &st) != 0) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, nullptr);
    }

    FILE *f = fopen(path, "rb");
    if (!f) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, nullptr);

    httpd_resp_set_type(req, content_type(uri));
    if (gz) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    static char buf[4096]; // handlers run on the single httpd task
    size_t n;
    esp_err_t e = ESP_OK;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        e = httpd_resp_send_chunk(req, buf, n);
        if (e != ESP_OK) break;
    }
    fclose(f);
    if (e == ESP_OK) httpd_resp_send_chunk(req, nullptr, 0);
    return e;
}

static esp_err_t data_handler(httpd_req_t *req)
{
    Telemetry t = telemetry_get();
    char out[720];
    int n = snprintf(out, sizeof out,
        "{\"seq\":%u,\"t\":%u,"
        "\"pitot_v\":%.4f,\"static_v\":%.4f,\"pot_v\":%.4f,\"rail_v\":%.4f,\"fluke_v\":%.4f,"
        "\"pitot_kpa\":%.3f,\"static_kpa\":%.3f,\"qc_kpa\":%.3f,"
        "\"ias_kt\":%.1f,\"alt_ft\":%.0f,"
        "\"pwm_hz\":%.1f,\"pwm_duty\":%.2f,\"pwm_set\":%.2f,\"pwm_manual\":%s,"
        "\"adc_sensors_ok\":%s,\"adc_fluke_ok\":%s,\"sensors_sim\":%s,\"pwm_avg_sim\":%s}",
        (unsigned)t.seq, (unsigned)t.uptime_ms,
        t.pitot_v, t.static_v, t.pot_v, t.rail_v, t.fluke_v,
        t.pitot_kpa, t.static_kpa, t.qc_kpa, t.ias_kt, t.alt_ft,
        t.pwm_freq_hz, t.pwm_duty_pct, t.pwm_set_pct, t.pwm_manual ? "true" : "false",
        t.adc_sensors_ok ? "true" : "false", t.adc_fluke_ok ? "true" : "false",
        t.sensors_sim ? "true" : "false", t.pwm_avg_sim ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, n);
}

// GET /pwm?duty=37.5  (manual)   GET /pwm?src=pot  (follow the potentiometer)
static esp_err_t pwm_handler(httpd_req_t *req)
{
    char q[48], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "duty=0..100 or src=pot");
    if (httpd_query_key_value(q, "duty", v, sizeof v) == ESP_OK)
        pwm_set_manual(strtof(v, nullptr));
    else if (httpd_query_key_value(q, "src", v, sizeof v) == ESP_OK && !strcmp(v, "pot"))
        pwm_follow_pot();
    else
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "duty=0..100 or src=pot");
    return httpd_resp_sendstr(req, "ok");
}

// GET /zero  - both ports at ambient: store the pitot/static offset (persists in NVS)
static esp_err_t zero_handler(httpd_req_t *req)
{
    char out[48];
    snprintf(out, sizeof out, "{\"qc_zero_kpa\":%.4f}", hw_zero_qc());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

void web_server_start()
{
    esp_vfs_littlefs_conf_t fs = {};
    fs.base_path = NET_MOUNT;
    fs.partition_label = "storage";
    fs.format_if_mount_failed = false;
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&fs));

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id = 0;
    cfg.stack_size = 8192;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;

    httpd_handle_t srv;
    ESP_ERROR_CHECK(httpd_start(&srv, &cfg));

    httpd_uri_t data = {"/data", HTTP_GET, data_handler, nullptr};
    httpd_uri_t pwm = {"/pwm", HTTP_GET, pwm_handler, nullptr};
    httpd_uri_t zero = {"/zero", HTTP_GET, zero_handler, nullptr};
    httpd_uri_t any = {"/*", HTTP_GET, static_handler, nullptr}; // must be last
    httpd_register_uri_handler(srv, &data);
    httpd_register_uri_handler(srv, &pwm);
    httpd_register_uri_handler(srv, &zero);
    httpd_register_uri_handler(srv, &any);
    ESP_LOGI(TAG, "http server up");
}
