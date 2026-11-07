// Simulated ESP-01 (UNAPI ESP8266 firmware) for the host test and the openMSX
// device: it reads the engine's requests, answers them after a configurable
// delay through a UART of limited speed, and serves HTTP streams made from a
// local MP3 file.
//
// Hosts / URLs it knows:
//   radio.test (10.0.0.1)
//     /stream.mp3     ICY stream, metaint 8192, icy-name "Test FM", titles
//                     "Artist One - Song A", "Artist Two - Song B", ... changing
//     /plain          stream without metadata
//     /redirect       302 to http://radio2.test:8000/live
//     /relative       302 to /stream.mp3
//     /list.pls       audio/x-scpls body pointing at /stream.mp3
//     /aac            content-type audio/aacp
//     /missing        404
//   radio2.test (10.0.0.2) port 8000  /live  plain stream
//   secure.test (10.0.0.3)            TLS only, /stream.mp3
//   all.api.radio-browser.info, de1.api.radio-browser.info (10.0.0.4) and
//   nl1.api.radio-browser.info (10.0.0.5): a small Radio Browser API
//     /json/countries           a few countries (UTF-8 escapes, one empty)
//     /json/stations/search     stations by country / tag / order, with AAC,
//                               HLS, broken and over-long ones mixed in;
//                               tag=many gives 150 stations
//     /json/url/<uuid>          counts a click
//   ip-api.com (10.0.0.6)                     /json/?fields=...  Amsterdam by IP
//   geocoding-api.open-meteo.com (10.0.0.7)   /v1/search?name=   places by name
//   api.open-meteo.com (10.0.0.8)             /v1/forecast?...   offset and weather now
//   (these three and the Radio Browser send a Date header: host_esp_date_base + time)
//   anything else: DNS error

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../pva_internal.h"

// ---------------- configuration (tests change these) ----------------
bool     host_esp_present = true;
uint8_t  host_esp_wifi_state = 2;           // NET_STATE answer, 2 = connected
uint32_t host_esp_uart_bpms = 86;           // 859372 baud / 10 bits / 1000
uint32_t host_esp_server_bps = 24000;       // server sends this many bytes/s (128 kbit/s = 16000)
uint32_t host_esp_latency_ms = 2;           // answer delay of the ESP
uint32_t host_esp_dns_ms = 40;
uint32_t host_esp_open_ms = 60;
uint32_t host_esp_tls_ms = 1800;
uint32_t host_esp_rcv_limit = 1024;         // most data bytes in one TCP_RCV answer
uint32_t host_esp_drop_after = 0;           // close each stream after this many body bytes (0 = never)
uint32_t host_esp_stall_at = 0;             // pause the server at this body byte ...
uint32_t host_esp_stall_ms = 0;             // ... for this long
size_t   host_stream_ram_size = 128u * 1024u;
uint32_t host_esp_rtt_ms = 0;               // > 0: TCP window limit (2144 bytes per round trip)
bool     host_esp_close_silent = true;      // as the real ESP: TCP_RCV of a closed connection
                                            // answers "no data", only TCP_STATE tells the close
uint32_t host_esp_state_asks;
uint32_t host_esp_delay_next_rcv;           // > 0: the next TCP_RCV answer comes this late (once)
bool     host_esp_api_http_silent = false;  // the API takes HTTP connections and never answers (HTTPS works)
bool     host_esp_api_down = false;         // 10.0.0.4 refuses connections (a mirror must be used)
bool     host_esp_api_all_down = false;     // all API servers refuse
uint32_t host_esp_api_bps = 20000;          // API reply speed
uint32_t host_esp_clicks;
char     host_esp_last_click[48];
char     host_esp_last_api[300];
long long host_esp_date_base = 1793303100LL;    // Date headers: 2026-10-29 19:45:00 UTC + the time
bool     host_esp_web_down = false;         // 10.0.0.6..8 refuse connections
bool     host_esp_range = true;             // news.test/big answers Range with 206
uint32_t host_esp_ranges;                   // Range requests news.test had
uint32_t host_esp_redirs;                   // requests for secure.test/go (a redirect)
uint32_t host_esp_web_requests;
char     host_esp_last_web[600];

// statistics for the checks
uint32_t host_esp_opens, host_esp_closes, host_esp_tls_opens, host_esp_requests, host_esp_rcvs;
char     host_esp_last_sni[64];
char     host_esp_last_get[300];
uint32_t host_esp_rx_dropped;               // bytes the engine never read (stray)

// ---------------- audio source ----------------
static uint8_t *src;
static size_t   src_len;

int host_esp_load_audio(const char *mp3_path)
{
    FILE *f = fopen(mp3_path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    free(src);
    src = malloc((size_t)n);
    src_len = fread(src, 1, (size_t)n, f) ;
    fclose(f);
    // skip an ID3v2 tag: a stream starts with frames
    if (src_len > 10 && !memcmp(src, "ID3", 3))
    {
        size_t t = 10u + (((size_t)src[6] & 0x7F) << 21 | ((size_t)src[7] & 0x7F) << 14 | ((size_t)src[8] & 0x7F) << 7 | (src[9] & 0x7F));
        if (t < src_len) { memmove(src, src + t, src_len - t); src_len -= t; }
    }
    return 0;
}

// ---------------- byte queues ----------------
typedef struct { uint8_t *b; size_t len, cap; } bq_t;
static void bq_put(bq_t *q, const void *p, size_t n)
{
    if (q->len + n > q->cap) { q->cap = (q->len + n) * 2 + 256; q->b = realloc(q->b, q->cap); }
    memcpy(q->b + q->len, p, n);
    q->len += n;
}
static void bq_drop(bq_t *q, size_t n) { if (n > q->len) n = q->len; memmove(q->b, q->b + n, q->len - n); q->len -= n; }

// pending answers: released to the UART at 'at'
typedef struct ans { uint32_t at; bq_t data; struct ans *next; } ans_t;
static ans_t *answers;
static bq_t uart_rx;            // bytes on their way to the engine (already "on the wire")
static bq_t tx;                 // request bytes from the engine
static bool owned;
static uint32_t now_ms;

static void answer(uint32_t delay, uint8_t fn, uint8_t rc, const void *p, size_t n)
{
    ans_t *a = calloc(1, sizeof *a);
    a->at = now_ms + delay;
    uint8_t h[4] = { fn, rc, (uint8_t)(n >> 8), (uint8_t)n };
    bq_put(&a->data, h, 4);
    if (rc == 0 && n) bq_put(&a->data, p, n);
    ans_t **pp = &answers;
    while (*pp) pp = &(*pp)->next;
    *pp = a;
}

// ---------------- connections / HTTP servers ----------------
typedef struct
{
    bool     used, tls;
    uint8_t  ip[4];
    uint16_t port;
    bq_t     reqbuf;
    bool     got_request;
    bq_t     out;               // bytes the server has sent (TCP receive buffer of the ESP)
    // stream generator
    int      kind;              // 0 none, 1 icy, 2 plain, 3 fixed body (headers+body in out already)
    uint32_t metaint, until_meta, title_no, meta_count;
    size_t   src_pos;
    uint32_t body_sent;
    double   credit;
    bool     closed;            // server closed (after the data in 'out')
    uint32_t stall_until;
    bq_t     pend;              // kind 4: reply that trickles out at host_esp_api_bps
} conn_t;
static conn_t conns[5];         // 1..4

static const char *titles[] = { "Artist One - Song A", "Artist Two - Song B", "Caf\xc3\xa9 Trio - Song C" };

static void http_reply(conn_t *c, const char *head, const char *body)
{
    bq_put(&c->out, head, strlen(head));
    if (body) bq_put(&c->out, body, strlen(body));
    c->kind = 3;
    c->closed = true;
}

// ---------------- the Radio Browser API ----------------
typedef struct { const char *name, *url, *codec, *country, *tags; int bitrate, hls, ok; } st_t;
static const st_t stations[] = {
    { "Test FM", "http://radio.test/stream.mp3", "MP3", "The Netherlands", "pop,80s,rock", 128, 0, 1 },
    { "Radio Two Live", "http://radio2.test:8000/live", "MP3", "Germany", "jazz,smooth jazz", 128, 0, 1 },
    { "  Plain \\\"Quoted\\\" Radio", "http://radio.test/plain", "mp3", "The Netherlands", "jazz,90s", 128, 0, 1 },
    { "Caf\\u00e9 AAC", "http://radio.test/aac", "AAC+", "Germany", "jazz", 64, 0, 1 },
    { "HLS One", "http://radio.test/hls.m3u8", "MP3", "Germany", "pop", 128, 1, 1 },
    { "Broken FM", "http://radio.test/stream.mp3", "MP3", "Germany", "pop,jazz", 128, 0, 0 },
    { "Secure Jazz", "https://secure.test/stream.mp3", "MP3", "C\\u00f4te D'ivoire", "jazz,80s", 192, 0, 1 },
    { "NC Secure", "https://secure.test/stream.mp3", "MP3", "New Caledonia", "pacific", 128, 0, 1 },
    { "NC Plain", "http://radio.test/plain", "MP3", "New Caledonia", "pacific", 128, 0, 1 },
    { "NC Live", "http://radio2.test:8000/live", "MP3", "New Caledonia", "pacific", 128, 0, 1 },
    { "Long URL", "http://radio.test/stream.mp3?xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "MP3", "Germany", "pop", 128, 0, 1 },
};

static void station_json(bq_t *b, const st_t *t, int i)
{
    char j[1200];
    int n = snprintf(j, sizeof j,
        "{\"changeuuid\":\"c-%d\",\"stationuuid\":\"9617a958-0601-11e8-ae97-52543be04c%02d\",\"serveruuid\":null,"
        "\"name\":\"%s\",\"url\":\"%s\",\"url_resolved\":\"%s\",\"homepage\":\"http://x.test/\",\"favicon\":\"\","
        "\"tags\":\"%s\",\"country\":\"%s\",\"countrycode\":\"XX\",\"iso_3166_2\":null,\"state\":\"\","
        "\"language\":\"dutch,english\",\"languagecodes\":\"nl\",\"votes\":%d,\"lastchangetime\":\"2024-01-01 10:00:00\","
        "\"codec\":\"%s\",\"bitrate\":%d,\"hls\":%d,\"lastcheckok\":%d,\"clickcount\":%d,\"clicktrend\":-1,"
        "\"ssl_error\":0,\"geo_lat\":52.1,\"geo_long\":-4.5e0,\"has_extended_info\":false}",
        i, i % 100, t->name, t->url, t->url, t->tags, t->country, 100 - i, t->codec, t->bitrate, t->hls, t->ok, 1000 - i);
    bq_put(b, j, (size_t)n);
}

static void date_hdr(char *out, size_t size);
static void api(conn_t *c, const char *path)
{
    bq_t b = { 0 };
    snprintf(host_esp_last_api, sizeof host_esp_last_api, "%s", path);
    bool clen = c->ip[3] == 5;              // the second server always sends Content-Length
    if (!strncmp(path, "/json/countries", 15))
    {
        const char *j = "[{\"name\":\"C\\u00f4te D'ivoire\",\"iso_3166_1\":\"CI\",\"stationcount\":12},\n"
                        " {\"name\":\"Germany\",\"iso_3166_1\":\"DE\",\"stationcount\":3210},\n"
                        " {\"name\":\"New Caledonia\",\"iso_3166_1\":\"NC\",\"stationcount\":3},\n"
                        " {\"name\":\"Nowhere\",\"iso_3166_1\":\"NO\",\"stationcount\":0},\n"
                        " {\"name\":\"The Netherlands\",\"iso_3166_1\":\"NL\",\"stationcount\":987}]";
        bq_put(&b, j, strlen(j));
    }
    else if (!strncmp(path, "/json/stations/search?", 22))
    {
        const char *q = path + 22;
        char country[64] = "", tag[64] = "";
        const char *p;
        if ((p = strstr(q, "country=")) && (p == q || p[-1] == '&')) sscanf(p + 8, "%63[^&]", country);
        if ((p = strstr(q, "tag="))) sscanf(p + 4, "%63[^&]", tag);
        char name[64] = "";
        if ((p = strstr(q, "name=")) && (p == q || p[-1] == '&')) sscanf(p + 5, "%63[^&]", name);
        for (char *s2 = name, *d = name; ; )
        {
            if (*s2 == '%' && s2[1] && s2[2]) { char h[3] = { s2[1], s2[2], 0 }; *d++ = (char)strtol(h, NULL, 16); s2 += 3; }
            else if (!(*d++ = *s2++)) break;
        }
        // undo the URL encoding
        for (char *s2 = country, *d = country; ; )
        {
            if (*s2 == '%' && s2[1] && s2[2]) { char h[3] = { s2[1], s2[2], 0 }; *d++ = (char)strtol(h, NULL, 16); s2 += 3; }
            else if (!(*d++ = *s2++)) break;
        }
        for (char *s2 = tag, *d = tag; ; )
        {
            if (*s2 == '%' && s2[1] && s2[2]) { char h[3] = { s2[1], s2[2], 0 }; *d++ = (char)strtol(h, NULL, 16); s2 += 3; }
            else if (!(*d++ = *s2++)) break;
        }
        bq_put(&b, "[", 1);
        int n = 0;
        int limit = 100000, offset = 0;
        if ((p = strstr(q, "limit="))) limit = atoi(p + 6);
        if ((p = strstr(q, "offset="))) offset = atoi(p + 7);
        if (!strcmp(tag, "many") || !strcmp(tag, "rock"))
        {
            // 150 stations, in pages (limit / offset)
            for (int i = offset; i < 150 && i < offset + limit; i++)
            {
                char nm[40];
                snprintf(nm, sizeof nm, "%s Station %03d", tag[0] == 'r' ? "Rock" : "Many", i);
                char su[48];
                snprintf(su, sizeof su, "http://radio.test/plain?n=%03d", i);
                st_t t = { nm, su, "MP3", "Germany", tag, 128, 0, 1 };
                if (n++) bq_put(&b, ",", 1);
                station_json(&b, &t, i);
            }
        }
        else
            for (int i = 0; i < (int)(sizeof stations / sizeof stations[0]); i++)
            {
                const st_t *t = &stations[i];
                char cj[64];
                // the country in the table is JSON-escaped: compare the escape of 'country'
                snprintf(cj, sizeof cj, "%s", t->country);
                if (!strcmp(cj, "C\\u00f4te D'ivoire")) snprintf(cj, sizeof cj, "C\xc3\xb4te D'ivoire");
                if (country[0] && strcmp(country, cj)) continue;
                if (tag[0] && !strstr(t->tags, tag)) continue;
                if (name[0])                    // a part of the name, any case
                {
                    char a[200], b2[64];
                    size_t k;
                    for (k = 0; t->name[k] && k < sizeof a - 1; k++) a[k] = (char)tolower((unsigned char)t->name[k]);
                    a[k] = 0;
                    for (k = 0; name[k] && k < sizeof b2 - 1; k++) b2[k] = (char)tolower((unsigned char)name[k]);
                    b2[k] = 0;
                    if (!strstr(a, b2)) continue;
                }
                if (n++) bq_put(&b, ",", 1);
                station_json(&b, t, i);
            }
        bq_put(&b, "]", 1);
    }
    else if (!strncmp(path, "/json/url/", 10))
    {
        host_esp_clicks++;
        snprintf(host_esp_last_click, sizeof host_esp_last_click, "%s", path + 10);
        const char *j = "{\"ok\":true,\"message\":\"retrieved station url\",\"stationuuid\":\"x\",\"name\":\"x\",\"url\":\"http://radio.test/stream.mp3\"}";
        bq_put(&b, j, strlen(j));
    }
    else
    {
        http_reply(c, "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n", "<html>no</html>");
        free(b.b);
        return;
    }
    if (b.len < 2048) clen = true;          // nginx: small replies have a length, big ones end with the close
    char hd[260], dt[64];
    date_hdr(dt, sizeof dt);
    snprintf(hd, sizeof hd, "HTTP/1.1 200 OK\r\nServer: nginx\r\nContent-Type: application/json; charset=utf-8\r\n%s\r\n", dt);
    if (clen) { char l[40]; snprintf(l, sizeof l, "Content-Length: %u\r\n", (unsigned)b.len); snprintf(hd, sizeof hd, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n%s%s\r\n", dt, l); }
    bq_put(&c->out, hd, strlen(hd));
    bq_put(&c->pend, b.b, b.len);
    free(b.b);
    c->kind = 4;
    c->credit = 0;
}

static void date_hdr(char *out, size_t size)
{
    static const char *wd[] = { "Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed" };
    static const char *mo[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    long long t = host_esp_date_base + now_ms / 1000u;
    long long days = t / 86400; int rem = (int)(t % 86400);
    // civil from days
    long long z = days + 719468, era = z / 146097;
    unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    long long y = (long long)yoe + era * 400 + (m <= 2);
    snprintf(out, size, "Date: %s, %02u %s %lld %02d:%02d:%02d GMT\r\n", wd[days % 7], d, mo[m - 1], y, rem / 3600, rem / 60 % 60, rem % 60);
}

static void web_json(conn_t *c, const char *j)
{
    char hd[300], dt[64];
    date_hdr(dt, sizeof dt);
    snprintf(hd, sizeof hd, "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n%sContent-Length: %u\r\n\r\n",
             dt, (unsigned)strlen(j));
    bq_put(&c->out, hd, strlen(hd));
    bq_put(&c->pend, j, strlen(j));
    c->kind = 4;
    c->credit = 0;
}

static void web(conn_t *c, const char *path)
{
    host_esp_web_requests++;
    snprintf(host_esp_last_web, sizeof host_esp_last_web, "%s", path);
    if (c->ip[3] == 6 && !strncmp(path, "/json/", 6))
    {
        web_json(c, "{\"status\":\"success\",\"countryCode\":\"NL\",\"regionName\":\"North Holland\",\"city\":\"Amsterdam\","
                    "\"lat\":52.3759,\"lon\":4.8975,\"timezone\":\"Europe/Amsterdam\",\"offset\":3600}");
        return;
    }
    if (c->ip[3] == 7 && !strncmp(path, "/v1/search?", 11))
    {
        if (strstr(path, "name=zur"))
            web_json(c, "{\"results\":[{\"id\":2657896,\"name\":\"Z\\u00fcrich\",\"latitude\":47.36667,\"longitude\":8.55,"
                        "\"elevation\":429.0,\"feature_code\":\"PPLA\",\"country_code\":\"CH\",\"timezone\":\"Europe/Zurich\","
                        "\"population\":341730,\"postcodes\":[\"8000\",\"8001\"],\"country_id\":2658434,\"country\":\"Switzerland\","
                        "\"admin1\":\"Zurich\"},{\"id\":1,\"name\":\"Zurich\",\"latitude\":43.0367,\"longitude\":-77.5,"
                        "\"country_code\":\"US\",\"admin1\":\"New York\",\"timezone\":\"America/New_York\"}],\"generationtime_ms\":1.2}");
        else if (strstr(path, "name=sydney"))
            web_json(c, "{\"results\":[{\"name\":\"Sydney\",\"latitude\":-33.86785,\"longitude\":151.20732,\"country_code\":\"AU\","
                        "\"admin1\":\"New South Wales\",\"timezone\":\"Australia/Sydney\"}],\"generationtime_ms\":0.9}");
        else web_json(c, "{\"generationtime_ms\":0.4}");
        return;
    }
    if (c->ip[3] == 8 && !strncmp(path, "/v1/forecast?", 13))
    {
        char j[2000];
        bool syd = strstr(path, "latitude=-33") != NULL;
        bool daily = strstr(path, "&daily=") != NULL;
        int n = snprintf(j, sizeof j, "{\"latitude\":%s,\"longitude\":4.9,\"generationtime_ms\":0.03,\"utc_offset_seconds\":%d,"
                 "\"timezone\":\"%s\",\"timezone_abbreviation\":\"%s\",\"elevation\":13.0,"
                 "\"current_units\":{\"time\":\"iso8601\",\"interval\":\"seconds\",\"temperature_2m\":\"\\u00b0C\",\"weather_code\":\"wmo code\"},"
                 "\"current\":{\"time\":\"2026-10-29T20:45\",\"interval\":900,\"temperature_2m\":%s,\"weather_code\":%d,"
                 "\"apparent_temperature\":%s,\"relative_humidity_2m\":87,\"wind_speed_10m\":14.8,\"is_day\":0}",
                 syd ? "-33.87" : "52.37", syd ? 39600 : 3600, syd ? "Australia/Sydney" : "Europe/Amsterdam", syd ? "AEDT" : "CET",
                 syd ? "21.6" : "-0.4", syd ? 1 : 61, syd ? "20.9" : "-3.5");
        if (daily)
            n += snprintf(j + n, sizeof j - (size_t)n, ",\"daily_units\":{\"time\":\"iso8601\"},\"daily\":{"
                 "\"time\":[\"2026-10-29\",\"2026-10-30\",\"2026-10-31\",\"2026-11-01\",\"2026-11-02\",\"2026-11-03\"],"
                 "\"weather_code\":[61,3,0,71,95,null],"
                 "\"temperature_2m_max\":[8.4,10.1,12.0,2.5,15.2,null],"
                 "\"temperature_2m_min\":[-1.2,3.3,4.0,-5.5,9.9,null],"
                 "\"precipitation_probability_max\":[80,20,0,65,100,null],"
                 "\"wind_speed_10m_max\":[25.4,18.0,9.6,30.2,61.5,null]}");
        snprintf(j + n, sizeof j - (size_t)n, "}");
        web_json(c, j);
        return;
    }
    http_reply(c, "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n", "<html>no</html>");
}

// RSS feeds: news.test (10.0.0.9): /rss (RSS 2.0, CDATA, entities, HTML), /rdf (RSS 1.0),
// /atom (Atom), /many (60 stories), /content (the whole text in the feed), /1 (an article's page)
static void news(conn_t *c, const char *path)
{
    host_esp_web_requests++;
    snprintf(host_esp_last_web, sizeof host_esp_last_web, "%s", path);
    bq_t b = { 0 };
    char t[600];
    if (!strcmp(path, "/rss"))
    {
        const char *x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<rss version=\"2.0\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
            "<channel><title>Test News</title><link>http://news.test/</link><description>Channel text</description>"
            "<image><title>Test News</title><url>http://news.test/logo.png</url></image>"
            "<item><title><![CDATA[Caf\xc3\xa9 owners & \"friends\" meet]]></title><link>http://news.test/1</link>"
            "<description><![CDATA[<p>The first <b>story</b>, with   spaces\nand a line.</p><img src=\"x.jpg\"/>]]></description>"
            "<pubDate>Thu, 29 Oct 2026 20:15:00 GMT</pubDate><guid isPermaLink=\"false\">a1</guid></item>\n"
            "<item><title>Second &amp; &#8220;quoted&#8221; &lt;tag&gt; story</title>"
            "<description>&lt;p&gt;Escaped &amp;amp; HTML &lt;a href=&quot;x&quot;&gt;link&lt;/a&gt;.&lt;/p&gt;</description>"
            "<pubDate>Thu, 29 Oct 2026 19:00:00 +0100</pubDate><media:thumbnail url=\"t.jpg\" width=\"240\"/></item>\n"
            "<!-- a comment <item><title>no</title></item> -->"
            "<item><title>Third: no description</title><pubDate>Wed, 28 Oct 2026 08:00:00 GMT</pubDate></item>"
            "</channel></rss>";
        bq_put(&b, x, strlen(x));
    }
    else if (!strcmp(path, "/rdf"))
    {
        const char *x = "<?xml version=\"1.0\"?><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\" xmlns=\"http://purl.org/rss/1.0/\">"
            "<channel rdf:about=\"x\"><title>RDF</title></channel>"
            "<item rdf:about=\"y\"><title>RDF story</title><description>RDF text</description><dc:date>2026-10-29T18:30:00Z</dc:date></item>"
            "</rdf:RDF>";
        bq_put(&b, x, strlen(x));
    }
    else if (!strcmp(path, "/atom"))
    {
        const char *x = "<?xml version=\"1.0\"?><feed xmlns=\"http://www.w3.org/2005/Atom\"><title>Atom</title>"
            "<entry><title type=\"html\">Atom story</title><link href=\"x\"/><summary>Atom text</summary><updated>2026-10-29T17:00:00Z</updated></entry></feed>";
        bq_put(&b, x, strlen(x));
    }
    else if (!strcmp(path, "/content"))
    {
        const char *h = "<rss xmlns:content=\"http://purl.org/rss/1.0/modules/content/\"><channel><item><title>Long one</title>"
                        "<link>http://news.test/2</link><description>Short summary.</description><content:encoded><![CDATA[";
        bq_put(&b, h, strlen(h));
        for (int i = 0; i < 12; i++)
        {
            int n = snprintf(t, sizeof t, "<p>Paragraph %d of the whole story in the feed, with enough words to make a line or two.</p>", i + 1);
            bq_put(&b, t, (size_t)n);
        }
        const char *e = "]]></content:encoded><pubDate>Thu, 29 Oct 2026 20:00:00 GMT</pubDate></item></channel></rss>";
        bq_put(&b, e, strlen(e));
    }
    else if (!strcmp(path, "/1"))
    {
        const char *x = "<!DOCTYPE html><html><head><title>x</title><script>var a=\"<p>no script text</p>\";</script><style>p{}</style></head>"
            "<body><header><p>Site menu text that is long enough to be kept if not skipped.</p></header>"
            "<nav><p>Home News Sport Weather and more links in the navigation bar</p></nav>"
            "<article><h1>Cafe owners meet</h1><p class=\"x\">The first paragraph of the article, long enough to be kept as text.</p>"
            "<figure><img src=\"a.jpg\"><figcaption>A photo caption that should not appear in the text at all.</figcaption></figure>"
            "<p>Second paragraph with <a href=\"x\">a link</a> &amp; an entity, caf&eacute; and &#8220;quotes&#8221; &mdash; done.</p>"
            "<p>Share</p><!-- a comment <p>no comment text here at all, really not</p> -->"
            "<p>Third paragraph: the end of the article\ntext, again long enough \xe2\x80\x93 really.</p></article>"
            "<footer><p>Copyright footer text that is long enough but must be skipped.</p></footer></body></html>";
        bq_put(&b, x, strlen(x));
    }
    else if (!strcmp(path, "/bigfeed"))
    {
        const char *f = "<rss><channel><item><title>A long read</title><link>https://news.test/big</link>"
                        "<description>Its summary.</description><pubDate>Thu, 29 Oct 2026 20:00:00 GMT</pubDate></item></channel></rss>";
        bq_put(&b, f, strlen(f));
    }
    else if (!strcmp(path, "/big"))
    {
        // a page of about 150 KB: scripts first, then 40 paragraphs
        bq_put(&b, "<html><head><script>", 20);
        for (int i = 0; i < 1500; i++) bq_put(&b, "var x=1234567890+abcdefghij-klmnopqrstuvwxyz*ABCDEFGHIJKLMNOPQRSTUVWXY;\n", 72);
        bq_put(&b, "</script></head><body><article>", 31);
        for (int i = 0; i < 40; i++)
        {
            int n = snprintf(t, sizeof t, "<p>Paragraph %d of the long read, with enough words in it to be kept as text. "
                             "<span class=\"some-long-class-name another-class\">More</span> words follow here.</p>", i + 1);
            bq_put(&b, t, (size_t)n);
        }
        bq_put(&b, "</article></body></html>", 24);
        unsigned long from = 0;
        const char *rg = strstr((char *)c->reqbuf.b, "Range: bytes=");
        if (rg) { from = strtoul(rg + 13, NULL, 10); host_esp_ranges++; }
        char hd[300], dt[64];
        date_hdr(dt, sizeof dt);
        if (rg && host_esp_range && from < b.len)
            snprintf(hd, sizeof hd, "HTTP/1.1 206 Partial Content\r\nContent-Type: text/html\r\n%sContent-Range: bytes %lu-%lu/%lu\r\n"
                     "Content-Length: %lu\r\n\r\n", dt, from, (unsigned long)b.len - 1, (unsigned long)b.len, (unsigned long)b.len - from);
        else
        {
            from = 0;
            snprintf(hd, sizeof hd, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n%sContent-Length: %lu\r\n\r\n", dt, (unsigned long)b.len);
        }
        bq_put(&c->out, hd, strlen(hd));
        bq_put(&c->pend, b.b + from, b.len - from);
        free(b.b);
        c->kind = 4;
        c->credit = 0;
        return;
    }
    else if (!strcmp(path, "/many"))
    {
        bq_put(&b, "<rss><channel>", 14);
        for (int i = 0; i < 60; i++)
        {
            int n = snprintf(t, sizeof t, "<item><title>Story %02d</title><description>Text of story %02d, a bit longer to fill the pool "
                             "with some words that go on and on.</description><pubDate>Thu, 29 Oct 2026 12:%02d:00 GMT</pubDate></item>\n", i, i, i % 60);
            bq_put(&b, t, (size_t)n);
        }
        bq_put(&b, "</channel></rss>", 16);
    }
    else
    {
        http_reply(c, "HTTP/1.1 404 Not Found\r\n\r\n", "no");
        return;
    }
    char hd[200], dt[64];
    date_hdr(dt, sizeof dt);
    snprintf(hd, sizeof hd, "HTTP/1.1 200 OK\r\nContent-Type: application/rss+xml\r\n%s\r\n", dt);   // no length: it ends with the close
    bq_put(&c->out, hd, strlen(hd));
    bq_put(&c->pend, b.b, b.len);
    free(b.b);
    c->kind = 4;
    c->credit = 0;
}

static void serve(conn_t *c)
{
    char *r = (char *)c->reqbuf.b;
    char path[600] = "", hosth[128] = "";
    sscanf(r, "GET %599s", path);
    if (strncmp(path, "/json/", 6) && strncmp(path, "/v1/", 4)) { char *qm = strchr(path, '?'); if (qm) *qm = 0; }
   // a stream's ?query does not matter
    char *h = strstr(r, "\r\nHost: ");
    if (h) sscanf(h + 8, "%127[^\r]", hosth);
    snprintf(host_esp_last_get, sizeof host_esp_last_get, "%s", r);
    host_esp_requests++;
    bool icy_meta = strstr(r, "Icy-MetaData: 1") != NULL;
    bool r1 = c->ip[3] == 1, r2 = c->ip[3] == 2, sec = c->ip[3] == 3;
    if (c->ip[3] >= 6 && c->ip[3] <= 8) { web(c, path); return; }
    if (c->ip[3] == 9) { news(c, path); return; }
    if (c->ip[3] == 4 || c->ip[3] == 5)
    {
        if (host_esp_api_http_silent && !c->tls) return;
        if (!strstr(r, "User-Agent: ")) { http_reply(c, "HTTP/1.1 403 Forbidden\r\n\r\n", NULL); return; }
        api(c, path);
        return;
    }
    if (sec && !c->tls) { http_reply(c, "HTTP/1.1 400 Bad Request\r\n\r\n", NULL); return; }
    if (sec && !strcmp(path, "/go")) { host_esp_redirs++; http_reply(c, "HTTP/1.1 302 Found\r\nLocation: https://secure.test/stream.mp3\r\n\r\n", NULL); return; }
    if ((r1 && !strcmp(path, "/stream.mp3")) || (sec && !strcmp(path, "/stream.mp3")))
    {
        char hd[300];
        snprintf(hd, sizeof hd, "ICY 200 OK\r\nicy-name: %s\r\ncontent-type: audio/mpeg\r\n%s\r\n",
                 sec ? "Secure Radio" : "Test FM", icy_meta ? "icy-metaint: 8192\r\n" : "");
        bq_put(&c->out, hd, strlen(hd));
        c->kind = 1;
        c->metaint = icy_meta ? 8192 : 0;
        c->until_meta = c->metaint;
        return;
    }
    if ((r1 && !strcmp(path, "/plain")) || (r2 && c->port == 8000 && !strcmp(path, "/live")))
    {
        const char *hd = "HTTP/1.0 200 OK\r\nContent-Type: audio/mpeg\r\nServer: test\r\n\r\n";
        bq_put(&c->out, hd, strlen(hd));
        c->kind = 2;
        return;
    }
    if (r1 && !strcmp(path, "/redirect")) { http_reply(c, "HTTP/1.1 302 Found\r\nLocation: http://radio2.test:8000/live\r\n\r\n", NULL); return; }
    if (r1 && !strcmp(path, "/relative")) { http_reply(c, "HTTP/1.1 301 Moved\r\nLocation: /stream.mp3\r\n\r\n", NULL); return; }
    if (r1 && !strcmp(path, "/list.pls"))
    {
        http_reply(c, "HTTP/1.1 200 OK\r\nContent-Type: audio/x-scpls\r\n\r\n",
                   "[playlist]\r\nNumberOfEntries=1\r\nFile1=http://radio.test/stream.mp3\r\nTitle1=Test FM\r\nLength1=-1\r\n");
        return;
    }
    if (r1 && !strcmp(path, "/aac")) { http_reply(c, "HTTP/1.1 200 OK\r\nContent-Type: audio/aacp\r\n\r\n", "xxxx"); return; }
    http_reply(c, "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n", "<html>no</html>");
}

static void gen(conn_t *c, uint32_t ms)
{
    if (c->kind == 4)
    {
        // an API reply: at host_esp_api_bps, then the server closes
        c->credit += (double)host_esp_api_bps * ms / 1000.0;
        size_t k = (size_t)c->credit;
        if (k > c->pend.len) k = c->pend.len;
        if (c->out.len > 16384) k = 0;
        c->credit -= (double)k;
        bq_put(&c->out, c->pend.b, k);
        bq_drop(&c->pend, k);
        if (!c->pend.len) c->closed = true;
        return;
    }
    if (c->kind != 1 && c->kind != 2) return;
    if (c->closed) return;
    if (c->stall_until && now_ms < c->stall_until) return;
    double bps = host_esp_server_bps;
    if (host_esp_rtt_ms && 2144.0 * 1000.0 / host_esp_rtt_ms < bps) bps = 2144.0 * 1000.0 / host_esp_rtt_ms;
    c->credit += bps * ms / 1000.0;
    size_t want = (size_t)c->credit;
    if (c->out.len > 16384) want = 0;           // TCP window full: server waits
    c->credit -= (double)want;
    if (c->credit > 4096) c->credit = 4096;
    uint8_t tmp[4096];
    while (want)
    {
        size_t k = want > sizeof tmp ? sizeof tmp : want;
        size_t o = 0;
        while (o < k)
        {
            if (c->metaint && c->until_meta == 0)
            {
                // a title every 6th block (about 3 s at 128 kbit/s), else an empty block
                if (c->meta_count++ % 6u == 0u)
                {
                    char m[96] = { 0 };
                    int n = snprintf(m, sizeof m, "StreamTitle='%s';", titles[c->title_no++ % 3]);
                    int blocks = (n + 15) / 16;
                    tmp[o++] = (uint8_t)blocks;
                    bq_put(&c->out, tmp, o);
                    o = 0;
                    bq_put(&c->out, m, (size_t)blocks * 16u);
                }
                else tmp[o++] = 0;
                c->until_meta = c->metaint;
                continue;
            }
            if (!src_len) { c->closed = true; break; }      // (no sound loaded)
            tmp[o++] = src[c->src_pos];
            c->src_pos = (c->src_pos + 1) % src_len;
            c->body_sent++;
            if (c->metaint) c->until_meta--;
            if (host_esp_stall_at && c->body_sent == host_esp_stall_at) { c->stall_until = now_ms + host_esp_stall_ms; host_esp_stall_at = 0; break; }
            if (host_esp_drop_after && c->body_sent >= host_esp_drop_after) { c->closed = true; break; }
        }
        bq_put(&c->out, tmp, o);
        want -= k;
        if (c->closed || (c->stall_until && now_ms < c->stall_until)) break;
    }
}

static int dns(const char *name, uint8_t *ip)
{
    static const struct { const char *n; uint8_t ip[4]; } t[] = {
        { "radio.test", { 10, 0, 0, 1 } }, { "radio2.test", { 10, 0, 0, 2 } }, { "secure.test", { 10, 0, 0, 3 } },
        { "all.api.radio-browser.info", { 10, 0, 0, 4 } }, { "de1.api.radio-browser.info", { 10, 0, 0, 4 } },
        { "nl1.api.radio-browser.info", { 10, 0, 0, 5 } }, { "ip-api.com", { 10, 0, 0, 6 } },
        { "geocoding-api.open-meteo.com", { 10, 0, 0, 7 } }, { "api.open-meteo.com", { 10, 0, 0, 8 } },
        { "news.test", { 10, 0, 0, 9 } } };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(name, t[i].n)) { memcpy(ip, t[i].ip, 4); return 0; }
    return -1;
}

static void request(const uint8_t *p, size_t n)
{
    uint8_t fn = p[0];
    const uint8_t *a = p + 3;
    size_t len = n - 3;
    uint32_t L = host_esp_latency_ms;
    switch (fn)
    {
        case 3: answer(L, fn, 0, &host_esp_wifi_state, 1); break;
        case 206:
        {
            char name[80] = "";
            size_t k = len - 1 < sizeof name - 1 ? len - 1 : sizeof name - 1;
            memcpy(name, a + 1, k);
            name[k] = 0;
            uint8_t ip[4];
            if (dns(name, ip) == 0) answer(host_esp_dns_ms, fn, 0, ip, 4);
            else answer(host_esp_dns_ms, fn, 2, NULL, 0);
            break;
        }
        case 13:
        {
            int slot = 0;
            for (int i = 1; i <= 4; i++) if (!conns[i].used) { slot = i; break; }
            bool tls = (a[10] & 0x04) != 0;
            bool tls_busy = false;
            for (int i = 1; i <= 4; i++) if (conns[i].used && conns[i].tls) tls_busy = true;
            if (!slot || (tls && tls_busy)) { answer(host_esp_open_ms, fn, 9, NULL, 0); break; }   // ERR_NO_FREE_CONN
            uint16_t port = (uint16_t)(a[4] | (a[5] << 8));
            bool known = (a[0] == 10 && a[3] >= 1 && a[3] <= 9);
            if ((a[3] == 4 && host_esp_api_down) || ((a[3] == 4 || a[3] == 5) && host_esp_api_all_down) || (a[3] >= 6 && host_esp_web_down)) known = false;
            if (!slot || !known || (a[3] == 2 && port != 8000)) { answer(host_esp_open_ms, fn, 3, NULL, 0); break; }
            conn_t *c = &conns[slot];
            memset(c, 0, sizeof *c);
            c->used = true;
            c->tls = tls;
            memcpy(c->ip, a, 4);
            c->port = port;
            host_esp_opens++;
            if (tls)
            {
                host_esp_tls_opens++;
                size_t k = len > 11 ? len - 11 : 0;
                if (k > sizeof host_esp_last_sni - 1) k = sizeof host_esp_last_sni - 1;
                memcpy(host_esp_last_sni, a + 11, k);
                host_esp_last_sni[k] = 0;
            }
            uint8_t cn = (uint8_t)slot;
            answer(host_esp_open_ms + (tls ? host_esp_tls_ms : 0), fn, 0, &cn, 1);
            break;
        }
        case 14:
        case 15:
        {
            uint8_t cn = a[0];
            if (cn >= 1 && cn <= 4 && conns[cn].used)
            {
                free(conns[cn].reqbuf.b);
                free(conns[cn].out.b);
                free(conns[cn].pend.b);
                memset(&conns[cn], 0, sizeof conns[cn]);
                host_esp_closes++;
                answer(L, fn, 0, NULL, 0);
            }
            else answer(L, fn, 11, NULL, 0);
            break;
        }
        case 17:
        {
            uint8_t cn = a[0];
            if (cn < 1 || cn > 4 || !conns[cn].used) { answer(L, fn, 11, NULL, 0); break; }
            conn_t *c = &conns[cn];
            bq_put(&c->reqbuf, a + 2, len - 2);
            uint8_t z = 0;
            bq_put(&c->reqbuf, &z, 1);
            c->reqbuf.len--;
            if (!c->got_request && strstr((char *)c->reqbuf.b, "\r\n\r\n")) { c->got_request = true; serve(c); }
            answer(L, fn, 0, NULL, 0);
            break;
        }
        case 18:
        {
            host_esp_rcvs++;
            uint8_t cn = a[0];
            uint16_t want = (uint16_t)(a[1] | (a[2] << 8));
            if (cn < 1 || cn > 4 || !conns[cn].used) { answer(L, fn, 11, NULL, 0); break; }
            conn_t *c = &conns[cn];
            if (c->out.len == 0 && c->closed && !host_esp_close_silent) { answer(L, fn, 11, NULL, 0); break; }
            size_t k = c->out.len;
            if (k > want) k = want;
            if (k > host_esp_rcv_limit) k = host_esp_rcv_limit;
            uint8_t *buf = malloc(k + 2);
            buf[0] = 0; buf[1] = 0;
            memcpy(buf + 2, c->out.b, k);
            bq_drop(&c->out, k);
            answer(L + host_esp_delay_next_rcv, fn, 0, buf, k + 2);
            host_esp_delay_next_rcv = 0;
            free(buf);
            break;
        }
        case 16:
        {
            host_esp_state_asks++;
            uint8_t cn = a[0];
            if (cn < 1 || cn > 4 || !conns[cn].used) { answer(L, fn, 11, NULL, 0); break; }
            conn_t *c = &conns[cn];
            uint8_t r[16] = { 0 };
            r[1] = c->closed ? 7 : 4;               // CLOSE-WAIT / ESTABLISHED
            r[2] = (uint8_t)c->out.len;
            r[3] = (uint8_t)(c->out.len >> 8);
            r[6] = 0xFF; r[7] = 0xFF;
            answer(L, fn, 0, r, 16);
            break;
        }
        default:
            answer(L, fn, 1, NULL, 0);
    }
}

static void parse_tx(void)
{
    while (tx.len >= 3)
    {
        size_t len = ((size_t)tx.b[1] << 8) | tx.b[2];
        if (tx.len < 3 + len) return;
        request(tx.b, 3 + len);
        bq_drop(&tx, 3 + len);
    }
}

// called every simulated millisecond
void host_esp_tick(uint32_t now)
{
    uint32_t ms = now > now_ms ? now - now_ms : 0;
    now_ms = now;
    for (int i = 1; i <= 4; i++) if (conns[i].used) gen(&conns[i], ms ? ms : 0);
    // move released answers onto the wire, at most uart_bpms bytes per ms
    size_t budget = (size_t)host_esp_uart_bpms * (ms ? ms : 1);
    while (answers && answers->at <= now_ms && budget)
    {
        ans_t *a = answers;
        size_t k = a->data.len < budget ? a->data.len : budget;
        bq_put(&uart_rx, a->data.b, k);
        bq_drop(&a->data, k);
        budget -= k;
        if (!a->data.len) { answers = a->next; free(a->data.b); free(a); }
    }
}

void host_esp_reset(void)
{
    for (int i = 1; i <= 4; i++) { free(conns[i].reqbuf.b); free(conns[i].out.b); free(conns[i].pend.b); memset(&conns[i], 0, sizeof conns[i]); }
    while (answers) { ans_t *a = answers; answers = a->next; free(a->data.b); free(a); }
    uart_rx.len = 0;
    tx.len = 0;
}

// a connection some earlier program left open in the ESP (survives an MSX reset)
void host_esp_leftover(bool tls)
{
    for (int i = 1; i <= 4; i++)
        if (!conns[i].used) { memset(&conns[i], 0, sizeof conns[i]); conns[i].used = true; conns[i].tls = tls; conns[i].ip[0] = 192; host_esp_opens++; return; }
}

int host_esp_open_conns(void)
{
    int n = 0;
    for (int i = 1; i <= 4; i++) n += conns[i].used;
    return n;
}

bool host_esp_owned(void) { return owned; }

// ---------------- HAL ----------------
bool pva_hal_net_present(void) { return host_esp_present; }

bool pva_hal_net_open(void)
{
    if (!host_esp_present) return false;
    owned = true;
    return true;
}

void pva_hal_net_close(void)
{
    owned = false;
    host_esp_rx_dropped += (uint32_t)uart_rx.len;
    uart_rx.len = 0;
}

size_t pva_hal_net_write(const uint8_t *p, size_t n)
{
    if (!owned) return 0;
    bq_put(&tx, p, n);
    parse_tx();
    return n;
}

size_t pva_hal_net_read(uint8_t *p, size_t n)
{
    if (!owned) return 0;
    if (n > uart_rx.len) n = uart_rx.len;
    memcpy(p, uart_rx.b, n);
    bq_drop(&uart_rx, n);
    return n;
}

static uint8_t stream_ram[256u * 1024u];
uint8_t *pva_hal_stream_ram(size_t *size)
{
    *size = host_stream_ram_size < sizeof stream_ram ? host_stream_ram_size : sizeof stream_ram;
    return *size ? stream_ram : NULL;
}
