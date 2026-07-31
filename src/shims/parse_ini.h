/* parse_ini.h — freestanding parser for the SD wifi-onboarding file.
 *
 * Shared verbatim between the on-device shim (wifi_sd.c) and the host unit
 * test (test_parser.c). NO libc is used: only pointer/byte arithmetic, so it
 * links cleanly into the -nostdlib shim AND compiles on the host for testing.
 *
 * File format (see README.md), tolerant of CRLF / whitespace / comments:
 *     SSID=MyNetwork
 *     PASSWORD=secretpass
 * Keys are case-insensitive; '=' or ':' separates key and value; lines
 * starting with '#' or ';' are comments; blank lines ignored. SSID may
 * contain spaces and '=' (only the first separator is honored).
 *
 * Return codes from wsd_parse():
 *   WSD_OK       (0)  ssid + password (WPA/WPA2) present and valid
 *   WSD_OK_OPEN  (1)  ssid present, no password -> open network
 *   negative          malformed / out-of-range -> caller must NOT connect
 */
#ifndef PARSE_INI_H
#define PARSE_INI_H

#define WSD_OK          0
#define WSD_OK_OPEN     1
#define WSD_E_NOSSID   (-1)   /* no SSID line found                         */
#define WSD_E_SSIDLEN  (-2)   /* SSID empty or > 32 chars                   */
#define WSD_E_PWDLEN   (-3)   /* password present but not 8..63 chars       */

#define WSD_SSID_MAX   32     /* IEEE 802.11 SSID max                       */
#define WSD_PWD_MIN    8      /* WPA/WPA2-PSK min (enforced by vp_project)  */
#define WSD_PWD_MAX    63     /* WPA/WPA2-PSK max ASCII passphrase          */

static int wsd_is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static char wsd_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* case-insensitive compare of key span [ks,ke) against nul-terminated lit */
static int wsd_key_eq(const char *ks, const char *ke, const char *lit)
{
    while (ks < ke && *lit) {
        if (wsd_lc(*ks) != wsd_lc(*lit)) return 0;
        ks++; lit++;
    }
    return ks == ke && *lit == 0;
}

/* copy [s,e) into dst (cap incl. nul); returns copied length */
static int wsd_copy(char *dst, int cap, const char *s, const char *e)
{
    int n = 0;
    while (s < e && n < cap - 1) dst[n++] = *s++;
    dst[n] = 0;
    return n;
}

/*
 * Parse buf[0..len). Writes ssid (cap>=WSD_SSID_MAX+1) and pass
 * (cap>=WSD_PWD_MAX+1). Both are always nul-terminated (empty on error).
 * Last matching line wins. Returns a WSD_* code.
 */
static int wsd_parse(const char *buf, int len,
                     char *ssid, int ssid_cap,
                     char *pass, int pass_cap)
{
    const char *ss = 0, *se = 0;   /* trimmed SSID value span     */
    const char *ps = 0, *pe = 0;   /* trimmed password value span */
    int i = 0;
    int ssid_len, pass_len;

    ssid[0] = 0;
    pass[0] = 0;
    if (!buf || len <= 0) return WSD_E_NOSSID;

    while (i < len) {
        const char *ls = buf + i, *le;
        const char *ks, *ke, *sep, *vs, *ve;
        /* find end of line */
        int j = i;
        while (j < len && buf[j] != '\n') j++;
        le = buf + j;
        i = j + 1;                     /* next line */

        /* trim leading ws */
        while (ls < le && wsd_is_ws(*ls)) ls++;
        if (ls >= le) continue;
        if (*ls == '#' || *ls == ';') continue;   /* comment */

        /* first '=' or ':' is the separator */
        sep = 0;
        { const char *p = ls;
          while (p < le) { if (*p == '=' || *p == ':') { sep = p; break; } p++; } }
        if (!sep) continue;

        ks = ls; ke = sep;
        while (ke > ks && wsd_is_ws(*(ke - 1))) ke--;   /* trim key trailing ws */

        vs = sep + 1; ve = le;
        while (vs < ve && wsd_is_ws(*vs)) vs++;          /* trim value leading ws */
        while (ve > vs && wsd_is_ws(*(ve - 1))) ve--;    /* trim value trailing ws (kills CR) */

        if (wsd_key_eq(ks, ke, "ssid") || wsd_key_eq(ks, ke, "wifi_ssid")) {
            ss = vs; se = ve;
        } else if (wsd_key_eq(ks, ke, "password") || wsd_key_eq(ks, ke, "pass") ||
                   wsd_key_eq(ks, ke, "psk") || wsd_key_eq(ks, ke, "key") ||
                   wsd_key_eq(ks, ke, "wifikey") || wsd_key_eq(ks, ke, "wifi_wpa_psk")) {
            ps = vs; pe = ve;
        }
    }

    if (!ss) return WSD_E_NOSSID;
    ssid_len = (int)(se - ss);
    if (ssid_len < 1 || ssid_len > WSD_SSID_MAX) return WSD_E_SSIDLEN;

    pass_len = ps ? (int)(pe - ps) : 0;
    if (pass_len == 0) {
        wsd_copy(ssid, ssid_cap, ss, se);
        pass[0] = 0;
        return WSD_OK_OPEN;
    }
    if (pass_len < WSD_PWD_MIN || pass_len > WSD_PWD_MAX) return WSD_E_PWDLEN;

    wsd_copy(ssid, ssid_cap, ss, se);
    wsd_copy(pass, pass_cap, ps, pe);
    return WSD_OK;
}

#endif /* PARSE_INI_H */
