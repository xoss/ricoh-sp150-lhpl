/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sp150ctl - talk to a Ricoh SP 150 series printer over USB using the
 * vendor's "VOP" settings protocol (what the RICOH Printer utility uses),
 * natively via IOKit. No drivers, no Rosetta.
 *
 * Transport: USB vendor control transfers on the printer interface
 *   OUT: bmRequestType 0x41, bRequest 0x4d, wValue 0x3c2b, wIndex 0x100
 *   IN : bmRequestType 0xc1, bRequest 0x00, wValue 0x0000, wIndex 0x100
 * Frame: 4d 3c 2b 1a | cmd u16 LE | len u16 LE | 13 01 | sub | payload
 *   len = payload bytes + 3. Reply uses the same header; byte 10 = status
 *   for set commands, payload follows at byte 11 for get commands.
 *
 * Commands used:
 *   0x106/0  get Wi-Fi block (180 bytes)   0x106/1  set Wi-Fi block
 *   0x106/7  access point list (10 x 34)    0x106/8  Wi-Fi status (1 byte)
 *   0x105/0  get IPv4 (14 bytes used)
 * Wi-Fi block: [0] enable, [4] security, [8..39] SSID, [40..103] passphrase
 * IPv4 block : [0] mode, [2..5] IP, [6..9] netmask, [10..13] gateway
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <ctype.h>
#include <readpassphrase.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RICOH_VID 0x05ca
#define VOP_WINDEX 0x100
#define WIFI_LEN 0xb4
#define AP_LEN 0x154

static IOUSBDeviceInterface650 **dev;
static int dev_opened;
static int verbose;

static void hexdump(const char *title, const uint8_t *p, size_t n)
{
    printf("%s (%zu bytes)\n", title, n);
    for (size_t i = 0; i < n; i += 16) {
        printf("  %04zx: ", i);
        for (size_t j = 0; j < 16; j++)
            if (i + j < n) printf("%02x ", p[i + j]); else printf("   ");
        printf(" ");
        for (size_t j = 0; j < 16 && i + j < n; j++)
            putchar(isprint(p[i + j]) ? p[i + j] : '.');
        putchar('\n');
    }
}

/* hexdump a VOP frame, masking the Wi-Fi passphrase (block offset 0x28..0x67) */
static void dump_frame(const char *title, const uint8_t *p, size_t n)
{
    uint8_t m[11 + 512];
    if (n > sizeof m) n = sizeof m;
    memcpy(m, p, n);
    if (n > 11 && m[4] == 0x06 && m[5] == 0x01 && (m[10] == 0 || m[10] == 1))
        for (size_t i = 11 + 0x28; i < 11 + 0x68 && i < n; i++)
            if (m[i]) m[i] = '*';
    hexdump(title, m, n);
}

static int find_printer(void)
{
    /* USB matching needs vendor+product together, so filter by hand */
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, m, &it) != KERN_SUCCESS) return -1;
    io_service_t s, found = 0;
    while ((s = IOIteratorNext(it))) {
        char name[128] = "";
        int vid = -1;
        CFTypeRef pn = IORegistryEntryCreateCFProperty(s, CFSTR("USB Product Name"), NULL, 0);
        if (pn && CFGetTypeID(pn) == CFStringGetTypeID())
            CFStringGetCString(pn, name, sizeof name, kCFStringEncodingUTF8);
        if (pn) CFRelease(pn);
        CFTypeRef v = IORegistryEntryCreateCFProperty(s, CFSTR("idVendor"), NULL, 0);
        if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberIntType, &vid);
        if (v) CFRelease(v);
        if (!found && vid == RICOH_VID && strncmp(name, "SP 150", 6) == 0) {
            found = s;
            fprintf(stderr, "Found RICOH %s on USB\n", name);
        } else {
            IOObjectRelease(s);
        }
    }
    IOObjectRelease(it);
    if (!found) { fprintf(stderr, "No Ricoh SP 150 found on USB.\n"); return -1; }

    IOCFPlugInInterface **plug = NULL;
    SInt32 score;
    kern_return_t kr = IOCreatePlugInInterfaceForService(found, kIOUSBDeviceUserClientTypeID,
                                                         kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(found);
    if (kr || !plug) { fprintf(stderr, "IOCreatePlugInInterfaceForService: 0x%x\n", kr); return -1; }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650), (LPVOID *)&dev);
    (*plug)->Release(plug);
    if (!dev) { fprintf(stderr, "QueryInterface failed\n"); return -1; }
    return 0;
}

static IOReturn ctrl(uint8_t type, uint8_t req, uint16_t val, void *buf, uint16_t len, uint32_t *done)
{
    IOUSBDevRequestTO r = {
        .bmRequestType = type, .bRequest = req, .wValue = val, .wIndex = VOP_WINDEX,
        .wLength = len, .pData = buf, .noDataTimeout = 5000, .completionTimeout = 5000,
    };
    IOReturn kr = (*dev)->DeviceRequestTO(dev, &r);
    if (kr == kIOReturnNotOpen && !dev_opened) {
        if ((kr = (*dev)->USBDeviceOpen(dev)) != kIOReturnSuccess) {
            fprintf(stderr, "USBDeviceOpen: 0x%x (printer busy?)\n", kr);
            return kr;
        }
        dev_opened = 1;
        kr = (*dev)->DeviceRequestTO(dev, &r);
    }
    if (done) *done = r.wLenDone;
    return kr;
}

/* Read and discard a reply the printer still has queued (e.g. after a
   timed-out request); it refuses new commands until that is collected. */
static size_t drain_pending(uint8_t *keep, size_t keepmax, int max_wait_tenths)
{
    size_t n = 0;
    int idle = 0;
    while (idle < max_wait_tenths) {
        uint8_t b;
        uint32_t done = 0;
        if (ctrl(0xc1, 0, 0, &b, 1, &done) == kIOReturnSuccess && done == 1) {
            if (keep && n < keepmax) keep[n] = b;
            n++;
            idle = 0;
            continue;
        }
        if (n && idle >= 5) break;
        idle++;
        usleep(100000);
    }
    return n;
}

/* Send one VOP command and read the reply. Returns reply length or -1. */
static int vop(uint16_t cmd, uint8_t sub, const void *payload, uint16_t plen,
               uint8_t *reply, uint16_t rlen)
{
    uint8_t pkt[11 + 512];
    if (plen > 512) return -1;
    pkt[0] = 0x4d; pkt[1] = 0x3c; pkt[2] = 0x2b; pkt[3] = 0x1a;
    pkt[4] = cmd & 0xff; pkt[5] = cmd >> 8;
    pkt[6] = (plen + 3) & 0xff; pkt[7] = (plen + 3) >> 8;
    pkt[8] = 0x13; pkt[9] = 0x01; pkt[10] = sub;
    if (plen) memcpy(pkt + 11, payload, plen);
    if (verbose) dump_frame(">> request", pkt, 11 + plen);
    uint32_t done = 0;
    IOReturn kr = ctrl(0x41, 0x4d, 0x3c2b, pkt, 11 + plen, &done);
    if (kr) {
        size_t n = drain_pending(NULL, 0, 30);
        if (verbose) fprintf(stderr, "write refused (0x%x), drained %zu stale bytes, retrying\n", kr, n);
        kr = ctrl(0x41, 0x4d, 0x3c2b, pkt, 11 + plen, &done);
        if (kr) { fprintf(stderr, "write failed: 0x%x\n", kr); return -1; }
    }
    usleep(200000);
    /* resync on the 4d 3c header like the vendor tool does; the AP scan
       takes the printer several seconds */
    int max_tries = (cmd == 0x106 && sub == 7) ? 1200 : 60;
    for (int tries = 0; tries < max_tries; tries++) {
        uint8_t b = 0;
        if (ctrl(0xc1, 0, 0, &b, 1, &done) || done != 1 || b != 0x4d) { usleep(100000); continue; }
        if (ctrl(0xc1, 0, 0, &b, 1, &done) || done != 1 || b != 0x3c) { usleep(100000); continue; }
        reply[0] = 0x4d; reply[1] = 0x3c;
        size_t got = 2;
        /* the rest may arrive in pieces */
        for (int w = 0; got < rlen && w < 50; w++) {
            if (ctrl(0xc1, 0, 0, reply + got, rlen - got, &done) == kIOReturnSuccess && done) {
                got += done;
                w = 0;
            } else {
                usleep(100000);
            }
        }
        if (verbose) dump_frame("<< reply", reply, got);
        return (int)got;
    }
    fprintf(stderr, "no reply from printer (cmd 0x%x sub %d)\n", cmd, sub);
    return -1;
}

static int vop_get(uint16_t cmd, uint8_t sub, uint8_t *out, uint16_t n)
{
    uint8_t r[11 + 512];
    int got = vop(cmd, sub, NULL, 0, r, 11 + n);
    if (got < 11) return -1;
    if (r[2] != 0x2b || r[3] != 0x1a) { fprintf(stderr, "bad reply header\n"); return -1; }
    memcpy(out, r + 11, n);
    return got - 11;
}

static int vop_set(uint16_t cmd, uint8_t sub, const uint8_t *in, uint16_t n)
{
    uint8_t r[11];
    int got = vop(cmd, sub, in, n, r, 11);
    if (got < 11 || r[2] != 0x2b || r[3] != 0x1a) return -1;
    return r[10];   /* status byte, 0 = OK */
}

static void print_str(const char *label, const uint8_t *p, size_t n)
{
    printf("%s\"", label);
    for (size_t i = 0; i < n && p[i]; i++) putchar(isprint(p[i]) ? p[i] : '?');
    printf("\"\n");
}

static int get_net(uint8_t net[0x80])
{
    return vop_get(0x105, 0, net, 0x80) >= 14 ? 0 : -1;
}

static void print_net(const uint8_t *n)
{
    printf("IPv4: mode %u  address %u.%u.%u.%u  netmask %u.%u.%u.%u  gateway %u.%u.%u.%u\n",
           n[0], n[2], n[3], n[4], n[5], n[6], n[7], n[8], n[9], n[10], n[11], n[12], n[13]);
}

static int cmd_info(void)
{
    uint8_t st = 0, wifi[WIFI_LEN], net[0x80];
    if (vop_get(0x106, 8, &st, 1) >= 0) printf("Wi-Fi status: %u\n", st);
    if (vop_get(0x106, 0, wifi, sizeof wifi) >= 0) {
        print_str("Wi-Fi SSID: ", wifi + 0x08, 32);
        int plen = 0;
        while (plen < 64 && wifi[0x28 + plen]) plen++;
        printf("Wi-Fi enabled: %u  security: %u  passphrase: %d characters stored\n",
               wifi[0], wifi[4], plen);
        if (verbose) {
            uint8_t masked[WIFI_LEN];
            memcpy(masked, wifi, sizeof masked);
            for (int i = 0x28; i < 0x68; i++) if (masked[i]) masked[i] = '*';
            hexdump("Wi-Fi block (passphrase masked)", masked, sizeof masked);
        }
    }
    if (get_net(net) == 0) print_net(net);
    return 0;
}

static int cmd_scan(void)
{
    uint8_t ap[AP_LEN];
    fprintf(stderr, "Scanning (takes ~10-20 s)...\n");
    if (vop_get(0x106, 7, ap, sizeof ap) < 0) return 1;
    int any = 0;
    for (int i = 0; i < AP_LEN / 34; i++) {
        const uint8_t *e = ap + i * 34;
        if (!e[0]) continue;
        any = 1;
        char ssid[33];
        memcpy(ssid, e, 32);
        ssid[32] = 0;
        printf("  %-32s  flags 0x%02x 0x%02x\n", ssid, e[32], e[33]);
    }
    if (!any) printf("  (no networks found)\n");
    return 0;
}

/* poll Wi-Fi status + IPv4 every 5 s, print whenever something changes */
static void watch_net(int seconds)
{
    uint8_t net[0x80], last_st = 0xff, last[12];
    memset(last, 0xff, sizeof last);
    for (int t = 5; t <= seconds; t += 5) {
        sleep(5);
        uint8_t s = 0xff;
        vop_get(0x106, 8, &s, 1);
        if (get_net(net) == 0 && (s != last_st || memcmp(net + 2, last, 12))) {
            printf("  t+%2ds  Wi-Fi status %u  ", t, s);
            print_net(net);
            fflush(stdout);
            last_st = s;
            memcpy(last, net + 2, 12);
        }
    }
}

static int cmd_wifi_set(const char *ssid)
{
    uint8_t blk[WIFI_LEN];
    size_t sl = strlen(ssid);
    if (sl == 0 || sl > 32) { fprintf(stderr, "SSID must be 1..32 characters\n"); return 2; }
    if (vop_get(0x106, 0, blk, sizeof blk) < 0) return 1;

    char pw1[128], pw2[128];
    if (!readpassphrase("Wi-Fi password: ", pw1, sizeof pw1, RPP_REQUIRE_TTY) ||
        !readpassphrase("Repeat password: ", pw2, sizeof pw2, RPP_REQUIRE_TTY)) {
        fprintf(stderr, "Run this in a terminal (needs a TTY for the password prompt).\n");
        return 2;
    }
    size_t pl = strlen(pw1);
    int ok = strcmp(pw1, pw2) == 0 && pl >= 8 && pl <= 64;
    memset(pw2, 0, sizeof pw2);
    if (!ok) {
        memset(pw1, 0, sizeof pw1);
        fprintf(stderr, "Passwords differ or not 8..64 characters.\n");
        return 2;
    }
    blk[0] = 1;                                    /* Wi-Fi on */
    memset(blk + 0x08, 0, 32); memcpy(blk + 0x08, ssid, sl);
    memset(blk + 0x28, 0, 64); memcpy(blk + 0x28, pw1, pl);
    memset(pw1, 0, sizeof pw1);

    printf("Sending Wi-Fi settings for \"%s\" (security %u)...\n", ssid, blk[4]);
    int st = vop_set(0x106, 1, blk, sizeof blk);
    memset(blk + 0x28, 0, 64);
    if (st < 0) { fprintf(stderr, "No confirmation from printer.\n"); return 1; }
    printf("Printer answered status %d (%s)\n", st, st == 0 ? "accepted" : "check");

    printf("Waiting for the printer to join the network (up to 90 s)...\n");
    watch_net(90);
    return 0;
}

/* IPv4 mode: 0 = DHCP/AutoIP, 1 = static (order of the vendor UI popup) */
static int cmd_ip(int argc, char **argv)
{
    uint8_t net[0x80];
    if (get_net(net)) return 1;
    printf("Current  ");
    print_net(net);
    if (argc == 1 && !strcmp(argv[0], "dhcp")) {
        net[0] = 0;
    } else if (argc == 4 && !strcmp(argv[0], "static")) {
        unsigned v[12];
        if (sscanf(argv[1], "%u.%u.%u.%u", &v[0], &v[1], &v[2], &v[3]) != 4 ||
            sscanf(argv[2], "%u.%u.%u.%u", &v[4], &v[5], &v[6], &v[7]) != 4 ||
            sscanf(argv[3], "%u.%u.%u.%u", &v[8], &v[9], &v[10], &v[11]) != 4) {
            fprintf(stderr, "usage: ip static <addr> <netmask> <gateway>\n");
            return 2;
        }
        net[0] = 1;
        for (int i = 0; i < 12; i++) net[2 + i] = (uint8_t)v[i];
    } else {
        fprintf(stderr, "usage: sp150ctl ip dhcp | ip static <addr> <netmask> <gateway>\n");
        return 2;
    }
    printf("Sending  ");
    print_net(net);
    int st = vop_set(0x105, 1, net, sizeof net);
    if (st < 0) { fprintf(stderr, "No confirmation from printer.\n"); return 1; }
    printf("Printer answered status %d (%s)\n", st, st == 0 ? "accepted" : "check");
    printf("Watching the address (up to 90 s)...\n");
    watch_net(90);
    return 0;
}

static int cmd_drain(void)
{
    uint8_t buf[512];
    size_t n = drain_pending(buf, sizeof buf, 100);
    dump_frame("drained", buf, n < sizeof buf ? n : sizeof buf);
    return 0;
}

int main(int argc, char **argv)
{
    int a = 1;
    if (a < argc && strcmp(argv[a], "-v") == 0) { verbose = 1; a++; }
    if (a >= argc) {
        fprintf(stderr,
                "usage: sp150ctl [-v] info              show Wi-Fi + IPv4 settings\n"
                "       sp150ctl [-v] scan              list networks the printer sees\n"
                "       sp150ctl [-v] wifi-set <SSID>   set network + password (prompted)\n"
                "       sp150ctl [-v] ip dhcp           switch IPv4 to DHCP\n"
                "       sp150ctl [-v] ip static <a> <m> <g>  fixed IPv4 address\n"
                "       sp150ctl [-v] drain             collect a stuck reply\n");
        return 2;
    }
    if (find_printer()) return 1;
    int rc = 2;
    if (!strcmp(argv[a], "info")) rc = cmd_info();
    else if (!strcmp(argv[a], "scan")) rc = cmd_scan();
    else if (!strcmp(argv[a], "wifi-set") && a + 1 < argc) rc = cmd_wifi_set(argv[a + 1]);
    else if (!strcmp(argv[a], "drain")) rc = cmd_drain();
    else if (!strcmp(argv[a], "ip")) rc = cmd_ip(argc - a - 1, argv + a + 1);
    else fprintf(stderr, "unknown command %s\n", argv[a]);
    if (dev_opened) (*dev)->USBDeviceClose(dev);
    (*dev)->Release(dev);
    return rc;
}
