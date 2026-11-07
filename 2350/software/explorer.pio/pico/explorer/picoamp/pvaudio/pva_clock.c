// MSX PICOVERSE PROJECT
// pva_clock.c - PicoVerse Audio Engine: the time of day, the place, and the
// current weather, for PicoAmp's clock page.
//
// Time:     every web reply's Date header (Radio Browser, the services below)
//           gives UTC to the second; the RP2350 counts on from there.
// Place:    location.mode in PICOAMP.INI:
//             ip     ip-api.com tells the city, its coordinates and the time
//                    zone of the internet connection (free, no key)
//             place  a place found by name (Open-Meteo's geocoding: free, no
//                    key), kept as location.name / .lat / .lon
// Zone:     the UTC offset in force now (with summer time) comes with the
//           weather: Open-Meteo's forecast with timezone=auto, asked for
//           every 30 minutes; it also gives the current temperature and
//           weather code (WMO).
//
// The MSX reads all of it from the clock block (PVA_R_CLOCK), already as local
// date and time: no date arithmetic on the Z80.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pva_internal.h"

#define REFRESH_MS      (30u * 60u * 1000u)     // weather and zone
#define IP_REFRESH_MS   (6u * 60u * 60u * 1000u)
#define RETRY_MS        60000u
#define RETRY_MAX_MS    (10u * 60u * 1000u)
#define MAX_RESULTS     8u

static const char *const ipapi_url =
    "http://ip-api.com/json/?fields=status,message,countryCode,regionName,city,lat,lon,timezone,offset";

// time
static bool     have_utc;
static int64_t  utc_s;              // UTC seconds at utc_ms
static uint32_t utc_ms;
static uint8_t  syncs;
// zone and place
static bool     enabled, have_zone, have_ll, have_ip;
static int32_t  offset_s;
static char     zone[40], abbr[8], place[64];
static char     lat[16], lon[16];
static bool     mode_ip = true;
static uint32_t next_fetch, ip_at, retry_ms = RETRY_MS;
static bool     fetch_now, ip_now;
// weather now
static bool     have_wx;
static int16_t  temp10;
static uint8_t  wcode;
static uint8_t  gen;
// search
typedef struct { char name[48], lat[16], lon[16]; } loc_t;
static char     cur_admin[24], cur_cc[4];
static loc_t    res[MAX_RESULTS];
static uint8_t  nres, loc_state = PVA_LOCS_IDLE;
static char     search_q[64];
static bool     search_pending;
static loc_t    cur;                // the result being read
// the request running
enum { Q_NONE, Q_IP, Q_METEO, Q_SEARCH };
static uint8_t  q = Q_NONE;
static uint8_t  last_err;
static bool     q_ok;
static pva_json_t J;
// ip-api's fields
static char     ip_city[40], ip_region[40], ip_cc[4], ip_tz[40], ip_lat[16], ip_lon[16];
static bool     ip_success;
static int32_t  ip_offset;
static bool     ip_have_offset;
// the forecast's
static bool     m_have_off, m_have_t, m_have_c;
static int32_t  m_off;
static int16_t  m_t10;
static uint8_t  m_code;
static char     m_tz[40], m_abbr[8];
// the weather: now and the next days (Open-Meteo, with the zone)
#define WX_DAYS 6
typedef struct { uint8_t wday, mday, code, pp, wind; int16_t tmax, tmin; bool ok; } wxday_t;
static struct { int16_t t10, feel10; uint8_t hum, code, wind, is_day, ndays; bool have; wxday_t d[WX_DAYS]; } wx, mw;

// ---------------------------------------------------------------- time
// days since 1970-01-01 of a civil date (H. Hinnant's algorithm)
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = (int)(yy + (*m <= 2));
}

int64_t pva_days_from_civil(int y, unsigned m, unsigned d) { return days_from_civil(y, m, d); }
void pva_civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d) { civil_from_days(z, y, m, d); }

static int64_t now_utc(void) { return utc_s + (int64_t)((pva_hal_millis() - utc_ms) / 1000u); }

// "Thu, 29 Oct 2026 19:42:05 GMT" (RFC 7231 IMF-fixdate)
void pva_clock_http_date(const char *v)
{
    static const char mons[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *c = strchr(v, ',');
    if (!c) return;
    int d, y, hh, mm, ss;
    char mon[4] = "";
    if (sscanf(c + 1, " %d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6) return;
    const char *mp = strstr(mons, mon);
    if (!mon[0] || !mp || (mp - mons) % 3) return;
    unsigned m = (unsigned)((mp - mons) / 3 + 1);
    if (y < 2020 || y > 2200 || d < 1 || d > 31 || hh > 23 || mm > 59 || ss > 60) return;
    int64_t t = days_from_civil(y, m, (unsigned)d) * 86400 + hh * 3600 + mm * 60 + ss;
    // a second of difference is the Date header's resolution: keep counting then
    if (have_utc)
    {
        int64_t diff = t - now_utc();
        if (diff >= -1 && diff <= 1) return;
    }
    if (!have_utc) pva_log("clock: %04d-%02u-%02d %02d:%02d:%02d UTC", y, m, d, hh, mm, ss);
    utc_s = t;
    utc_ms = pva_hal_millis();
    have_utc = true;
    syncs++;
}

bool pva_clock_have_time(void) { return have_utc; }

bool pva_clock_utc(int64_t *now, int32_t *offset)
{
    *now = have_utc ? now_utc() : 0;
    *offset = have_zone ? offset_s : 0;
    return have_utc;
}

// the local time of the place (false: not known yet)
bool pva_clock_local(uint8_t *hour, uint8_t *min, uint8_t *wday)
{
    if (!have_utc || !have_zone) return false;
    int64_t t = now_utc() + offset_s;
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    int32_t rem = (int32_t)(t - days * 86400);
    *hour = (uint8_t)(rem / 3600);
    *min = (uint8_t)((rem / 60) % 60);
    *wday = (uint8_t)(((days % 7) + 7 + 3) % 7);
    return true;
}

// ---------------------------------------------------------------- helpers
static int16_t tenths(const char *s)
{
    // "12.3", "-0.5", "7" -> 123, -5, 70
    bool neg = *s == '-';
    if (neg || *s == '+') s++;
    long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    v *= 10;
    if (*s == '.' && s[1] >= '0' && s[1] <= '9') v += s[1] - '0';
    if (v > 9999) v = 9999;
    return (int16_t)(neg ? -v : v);
}

static void copy(char *dst, size_t size, const char *s)
{
    pva_copy_ascii(dst, size, s, strlen(s));
}

static void ip_place(void)
{
    snprintf(place, sizeof place, "%s%s%s%s%s", ip_city, ip_region[0] && strcmp(ip_region, ip_city) ? ", " : "",
             strcmp(ip_region, ip_city) ? ip_region : "", ip_cc[0] ? ", " : "", ip_cc);
}

static void apply_cfg(void)
{
    char m[16];
    pva_cfg_get("location.mode", m, sizeof m);
    bool ip = strcmp(m, "place") != 0;
    char a[16], o[16], n[64];
    pva_cfg_get("location.lat", a, sizeof a);
    pva_cfg_get("location.lon", o, sizeof o);
    pva_cfg_get("location.name", n, sizeof n);
    if (!ip && (!a[0] || !o[0])) ip = true;             // a place without coordinates: by IP
    bool changed = ip != mode_ip || (!ip && (strcmp(a, lat) || strcmp(o, lon)));
    mode_ip = ip;
    if (!ip)
    {
        snprintf(lat, sizeof lat, "%s", a);
        snprintf(lon, sizeof lon, "%s", o);
        copy(place, sizeof place, n[0] ? n : "?");
        have_ll = true;
    }
    else if (have_ip)
    {
        ip_place();
        snprintf(lat, sizeof lat, "%s", ip_lat);
        snprintf(lon, sizeof lon, "%s", ip_lon);
        have_ll = true;
    }
    else have_ll = false;
    if (changed)
    {
        have_wx = false;
        fetch_now = true;
        if (ip && !have_ip) ip_now = true;
        gen++;
    }
}

// ---------------------------------------------------------------- replies
static void json_cb(pva_json_t *j, int ev, const char *path, const char *v, uint16_t idx)
{
    (void)j; (void)idx;
    if (q == Q_IP && ev == PVA_JSON_VALUE)
    {
        if (!strcmp(path, "status")) ip_success = !strcmp(v, "success");
        else if (!strcmp(path, "city")) copy(ip_city, sizeof ip_city, v);
        else if (!strcmp(path, "regionName")) copy(ip_region, sizeof ip_region, v);
        else if (!strcmp(path, "countryCode")) copy(ip_cc, sizeof ip_cc, v);
        else if (!strcmp(path, "timezone")) copy(ip_tz, sizeof ip_tz, v);
        else if (!strcmp(path, "lat")) snprintf(ip_lat, sizeof ip_lat, "%.15s", v);
        else if (!strcmp(path, "lon")) snprintf(ip_lon, sizeof ip_lon, "%.15s", v);
        else if (!strcmp(path, "offset")) { ip_offset = (int32_t)atol(v); ip_have_offset = true; }
    }
    else if (q == Q_METEO && ev == PVA_JSON_VALUE)
    {
        if (!strcmp(path, "utc_offset_seconds")) { m_off = (int32_t)atol(v); m_have_off = true; }
        else if (!strcmp(path, "timezone")) copy(m_tz, sizeof m_tz, v);
        else if (!strcmp(path, "timezone_abbreviation")) copy(m_abbr, sizeof m_abbr, v);
        else if (!strcmp(path, "current.temperature_2m") && v[0] != 'n') { m_t10 = tenths(v); m_have_t = true; }
        else if (!strcmp(path, "current.weather_code") && v[0] != 'n') { m_code = (uint8_t)atoi(v); m_have_c = true; }
        else if (v[0] == 'n') { }
        else if (!strcmp(path, "current.apparent_temperature")) mw.feel10 = tenths(v);
        else if (!strcmp(path, "current.relative_humidity_2m")) mw.hum = (uint8_t)atoi(v);
        else if (!strcmp(path, "current.wind_speed_10m")) mw.wind = (uint8_t)((tenths(v) + 5) / 10);
        else if (!strcmp(path, "current.is_day")) mw.is_day = (uint8_t)atoi(v);
        else if (!strncmp(path, "daily.", 6) && idx < WX_DAYS)
        {
            wxday_t *d = &mw.d[idx];
            const char *f = path + 6;
            if (!strcmp(f, "time"))
            {
                int y, mo, dd;
                if (sscanf(v, "%d-%d-%d", &y, &mo, &dd) == 3 && mo >= 1 && mo <= 12 && dd >= 1 && dd <= 31)
                {
                    int64_t days = days_from_civil(y, (unsigned)mo, (unsigned)dd);
                    d->wday = (uint8_t)(((days % 7) + 7 + 3) % 7);
                    d->mday = (uint8_t)dd;
                }
            }
            else if (!strcmp(f, "weather_code")) { d->code = (uint8_t)atoi(v); d->ok = true; }
            else if (!strcmp(f, "temperature_2m_max")) d->tmax = tenths(v);
            else if (!strcmp(f, "temperature_2m_min")) d->tmin = tenths(v);
            else if (!strcmp(f, "precipitation_probability_max")) d->pp = (uint8_t)atoi(v);
            else if (!strcmp(f, "wind_speed_10m_max")) d->wind = (uint8_t)((tenths(v) + 5) / 10);
        }
    }
    else if (q == Q_SEARCH)
    {
        if (ev == PVA_JSON_VALUE)
        {
            if (!strcmp(path, "results.name")) copy(cur.name, 26, v);
            else if (!strcmp(path, "results.admin1")) copy(cur_admin, sizeof cur_admin, v);
            else if (!strcmp(path, "results.country_code")) copy(cur_cc, sizeof cur_cc, v);
            else if (!strcmp(path, "results.latitude")) snprintf(cur.lat, sizeof cur.lat, "%.15s", v);
            else if (!strcmp(path, "results.longitude")) snprintf(cur.lon, sizeof cur.lon, "%.15s", v);
        }
        else if (ev == PVA_JSON_END && !strcmp(path, "results"))
        {
            if (nres < MAX_RESULTS && cur.name[0] && cur.lat[0] && cur.lon[0])
            {
                loc_t *r = &res[nres++];
                *r = cur;
                // "Name, Region, CC" (the region left out when it is the name)
                snprintf(r->name, sizeof r->name, "%s%s%s%s%s", cur.name, cur_admin[0] && strcmp(cur_admin, cur.name) ? ", " : "",
                         strcmp(cur_admin, cur.name) ? cur_admin : "", cur_cc[0] ? ", " : "", cur_cc);
            }
            memset(&cur, 0, sizeof cur);
            cur_admin[0] = cur_cc[0] = 0;
        }
    }
}

static bool body(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n && !J.done; i++) pva_json_byte(&J, (char)p[i]);
    return J.done;
}

static void fetch_done(bool ok)
{
    uint8_t was = q;
    q = Q_NONE;
    q_ok = ok;
    uint32_t now = pva_hal_millis();
    if (!ok) last_err = pva_web_error() ? pva_web_error() : PVA_NERR_HTTP;
    if (was == Q_IP)
    {
        if (ok && ip_success && ip_lat[0] && ip_lon[0])
        {
            have_ip = true;
            ip_at = now;
            if (mode_ip)
            {
                ip_place();
                snprintf(lat, sizeof lat, "%s", ip_lat);
                snprintf(lon, sizeof lon, "%s", ip_lon);
                have_ll = true;
                if (ip_have_offset) { offset_s = ip_offset; have_zone = true; snprintf(zone, sizeof zone, "%s", ip_tz); }
                fetch_now = true;           // the weather (and the zone's name) next
                gen++;
            }
            retry_ms = RETRY_MS;
            pva_log("clock: by IP %.40s", place);
        }
        else
        {
            if (ok) last_err = PVA_NERR_HTTP;
            ip_now = false;
            next_fetch = now + retry_ms;
            if (retry_ms < RETRY_MAX_MS) retry_ms *= 2u;
        }
    }
    else if (was == Q_METEO)
    {
        if (ok && m_have_off)
        {
            offset_s = m_off;
            have_zone = true;
            snprintf(zone, sizeof zone, "%s", m_tz);
            snprintf(abbr, sizeof abbr, "%s", m_abbr);
            have_wx = m_have_t;
            temp10 = m_t10;
            wcode = m_have_c ? m_code : 0;
            mw.ndays = 0;               // the days with a forecast (a day without one ends them)
            while (mw.ndays < WX_DAYS && mw.d[mw.ndays].ok) mw.ndays++;
            mw.t10 = m_t10;
            mw.code = wcode;
            mw.have = m_have_t;
            wx = mw;
            next_fetch = now + REFRESH_MS;
            retry_ms = RETRY_MS;
            gen++;
        }
        else
        {
            if (ok) last_err = PVA_NERR_HTTP;
            next_fetch = now + retry_ms;
            if (retry_ms < RETRY_MAX_MS) retry_ms *= 2u;
        }
    }
    else if (was == Q_SEARCH)
    {
        loc_state = ok ? PVA_LOCS_DONE : PVA_LOCS_FAILED;
        pva_log("clock: '%.20s': %u places", search_q, (unsigned)nres);
    }
}

static bool start(uint8_t what, const char *url)
{
    J.cb = json_cb;
    pva_json_reset(&J);
    q = what;
    if (pva_web_get(url, body, fetch_done) != PVA_OK) { q = Q_NONE; return false; }
    return true;
}

// ---------------------------------------------------------------- polling
static uint8_t last_sec = 0xFF;
static uint8_t pub_gen = 0xFF, pub_flags = 0xFF, pub_loc = 0xFF, pub_n = 0xFF, pub_err = 0xFF;
static uint8_t pub_extra[PVA_CLOCK_SIZE - PVA_CK_EXTRA];

static void publish(void)
{
    uint8_t flags = (have_utc ? PVA_CKF_TIME : 0) | (have_zone ? PVA_CKF_ZONE : 0) | (have_wx ? PVA_CKF_WEATHER : 0) |
                    (q == Q_IP || q == Q_METEO ? PVA_CKF_BUSY : 0) | (last_err && !q_ok ? PVA_CKF_FAILED : 0);
    int64_t t = now_utc() + (have_zone ? offset_s : 0);
    uint8_t sec = (uint8_t)(have_utc ? (uint8_t)(((t % 60) + 60) % 60) : 0xFE);
    uint8_t extra[PVA_CLOCK_SIZE - PVA_CK_EXTRA];
    pva_engine_extra(extra);
    if (sec == last_sec && gen == pub_gen && flags == pub_flags && loc_state == pub_loc && nres == pub_n && last_err == pub_err &&
        !memcmp(extra, pub_extra, sizeof extra)) return;
    memcpy(pub_extra, extra, sizeof extra);
    pub_loc = loc_state;
    pub_n = nres;
    pub_err = last_err;
    last_sec = sec;
    pub_gen = gen;
    pub_flags = flags;
    volatile uint8_t *b = pva_win + PVA_R_CLOCK;
    uint8_t s = (uint8_t)(b[PVA_CK_SEQ] | 1u);
    b[PVA_CK_SEQ] = s;
    pva_barrier();
    b[PVA_CK_FLAGS] = flags;
    if (have_utc)
    {
        int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
        int32_t rem = (int32_t)(t - days * 86400);
        int y; unsigned m, d;
        civil_from_days(days, &y, &m, &d);
        b[PVA_CK_SEC] = (uint8_t)(rem % 60);
        b[PVA_CK_MIN] = (uint8_t)((rem / 60) % 60);
        b[PVA_CK_HOUR] = (uint8_t)(rem / 3600);
        b[PVA_CK_WDAY] = (uint8_t)(((days % 7) + 7 + 3) % 7);   // 1970-01-01 was a Thursday; 0 = Monday
        b[PVA_CK_DAY] = (uint8_t)d;
        b[PVA_CK_MONTH] = (uint8_t)m;
        b[PVA_CK_YEAR] = (uint8_t)y;
        b[PVA_CK_YEAR + 1] = (uint8_t)(y >> 8);
    }
    int16_t om = (int16_t)(offset_s / 60);
    b[PVA_CK_OFFSET] = (uint8_t)om;
    b[PVA_CK_OFFSET + 1] = (uint8_t)((uint16_t)om >> 8);
    b[PVA_CK_LOC_STATE] = loc_state;
    b[PVA_CK_LOC_COUNT] = nres;
    b[PVA_CK_GEN] = gen;
    b[PVA_CK_WCODE] = wcode;
    b[PVA_CK_TEMP] = (uint8_t)temp10;
    b[PVA_CK_TEMP + 1] = (uint8_t)((uint16_t)temp10 >> 8);
    for (unsigned i = 0; i < 8; i++) b[PVA_CK_ABBR + i] = i < 7 ? (uint8_t)abbr[i] : 0;
    b[PVA_CK_SYNCS] = syncs;
    b[PVA_CK_ERR] = last_err;
    for (unsigned i = 0; i < sizeof extra; i++) b[PVA_CK_EXTRA + i] = extra[i];
    pva_barrier();
    b[PVA_CK_SEQ] = (uint8_t)(s + 1u);
}

void pva_clock_poll(void)
{
    if (enabled && q == Q_NONE && !pva_web_busy() && pva_hal_net_present())
    {
        uint32_t now = pva_hal_millis();
        if (search_pending)
        {
            search_pending = false;
            char e[96], url[200];
            pva_dir_urlenc(e, sizeof e, search_q);
            snprintf(url, sizeof url, "http://geocoding-api.open-meteo.com/v1/search?name=%s&count=%u&language=en&format=json",
                     e, (unsigned)MAX_RESULTS);
            nres = 0;
            memset(&cur, 0, sizeof cur);
            if (!start(Q_SEARCH, url)) loc_state = PVA_LOCS_FAILED;
        }
        else if (mode_ip && (ip_now || !have_ip || now - ip_at >= IP_REFRESH_MS) && (ip_now || (int32_t)(now - next_fetch) >= 0))
        {
            ip_now = false;
            ip_success = ip_have_offset = false;
            ip_city[0] = ip_region[0] = ip_cc[0] = ip_tz[0] = ip_lat[0] = ip_lon[0] = 0;
            start(Q_IP, ipapi_url);
        }
        else if (have_ll && (fetch_now || (int32_t)(now - next_fetch) >= 0))
        {
            fetch_now = false;
            char url[400];
            snprintf(url, sizeof url, "http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
                     "&timezone=auto&forecast_days=%u&current=temperature_2m,weather_code,apparent_temperature,"
                     "relative_humidity_2m,wind_speed_10m,is_day&daily=weather_code,temperature_2m_max,"
                     "temperature_2m_min,precipitation_probability_max,wind_speed_10m_max", lat, lon, (unsigned)WX_DAYS);
            m_have_off = m_have_t = m_have_c = false;
            memset(&mw, 0, sizeof mw);
            for (unsigned i = 0; i < WX_DAYS; i++) mw.d[i].pp = 0xFF;
            m_tz[0] = m_abbr[0] = 0;
            start(Q_METEO, url);
        }
    }
    publish();
}

// ---------------------------------------------------------------- commands
// PicoAmp read the settings: the clock starts working
void pva_clock_enable(void)
{
    enabled = true;
    apply_cfg();
}

void pva_clock_cfg_changed(void) { if (enabled) apply_cfg(); }

int pva_clock_info(void)
{
    pva_reply_str(place);
    pva_reply_str(zone);
    pva_reply_str(abbr);
    return PVA_OK;
}

// WX_GET: the weather now and the next days (see PVA_WX_xxx)
int pva_clock_wx(void)
{
    if (!wx.have) return PVA_E_STATE;
    uint8_t b[PVA_WX_DAY0 + WX_DAYS * PVA_WX_DAYSIZE];
    memset(b, 0, sizeof b);
    b[PVA_WX_NDAYS] = wx.ndays;
    b[PVA_WX_IS_DAY] = wx.is_day;
    b[PVA_WX_TEMP] = (uint8_t)wx.t10; b[PVA_WX_TEMP + 1] = (uint8_t)((uint16_t)wx.t10 >> 8);
    b[PVA_WX_FEEL] = (uint8_t)wx.feel10; b[PVA_WX_FEEL + 1] = (uint8_t)((uint16_t)wx.feel10 >> 8);
    b[PVA_WX_HUM] = wx.hum;
    b[PVA_WX_CODE] = wx.code;
    b[PVA_WX_WIND] = wx.wind;
    for (unsigned i = 0; i < wx.ndays && i < WX_DAYS; i++)
    {
        uint8_t *d = b + PVA_WX_DAY0 + i * PVA_WX_DAYSIZE;
        d[PVA_WXD_WDAY] = wx.d[i].wday;
        d[PVA_WXD_MDAY] = wx.d[i].mday;
        d[PVA_WXD_CODE] = wx.d[i].code;
        d[PVA_WXD_PP] = wx.d[i].pp;
        d[PVA_WXD_WIND] = wx.d[i].wind;
        d[PVA_WXD_TMAX] = (uint8_t)wx.d[i].tmax; d[PVA_WXD_TMAX + 1] = (uint8_t)((uint16_t)wx.d[i].tmax >> 8);
        d[PVA_WXD_TMIN] = (uint8_t)wx.d[i].tmin; d[PVA_WXD_TMIN + 1] = (uint8_t)((uint16_t)wx.d[i].tmin >> 8);
    }
    for (size_t i = 0; i < sizeof b; i++) pva_reply_byte(b[i]);
    return PVA_OK;
}

int pva_clock_loc_ip(void)
{
    if (!pva_cfg_set("location.mode", "ip")) return PVA_E_FULL;
    enabled = true;
    ip_now = true;
    apply_cfg();
    gen++;
    return PVA_OK;
}

int pva_clock_search(const char *text)
{
    if (!text[0]) return PVA_E_ARGS;
    if (!pva_hal_net_present()) return PVA_E_NOWIFI;
    snprintf(search_q, sizeof search_q, "%s", text);
    search_pending = true;
    enabled = true;
    nres = 0;
    loc_state = PVA_LOCS_LOADING;
    return PVA_OK;
}

int pva_clock_list(void)
{
    if (loc_state != PVA_LOCS_DONE) return PVA_E_STATE;
    pva_reply_byte(nres);
    for (uint8_t i = 0; i < nres; i++) pva_reply_str(res[i].name);
    return PVA_OK;
}

int pva_clock_pick(uint8_t i)
{
    if (loc_state != PVA_LOCS_DONE || i >= nres) return PVA_E_RANGE;
    if (!pva_cfg_set("location.mode", "place") || !pva_cfg_set("location.name", res[i].name) ||
        !pva_cfg_set("location.lat", res[i].lat) || !pva_cfg_set("location.lon", res[i].lon))
        return PVA_E_FULL;
    enabled = true;
    apply_cfg();
    fetch_now = true;
    gen++;
    pva_log("clock: place %.40s", res[i].name);
    return PVA_OK;
}
