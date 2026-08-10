/*
 * main.c — entry point for the DRM/TTY direct-render fake update screen.
 *
 * Pure TTY mode: takes over the display (switch to an idle VT, grab DRM
 * master), renders the fake Windows update UI straight to the physical
 * screen, then restores the desktop after the timeout. No GTK, no WebKit,
 * no X11 — only libdrm + FreeType (+ fontconfig).
 *
 * Must be run as root.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ttydrm.h"

#define DEFAULT_TIMEOUT 60
#define VERSION "1.0.0"

static void print_version(void)
{
    printf("windows_update_in_linux %s\n", VERSION);
    printf("Fake Windows update screen rendered directly on the TTY via DRM/KMS.\n");
    printf("Requires root. NEVER reboots: restores the desktop after the timeout.\n");
}

static void print_usage(FILE *out)
{
    fprintf(out,
        "Usage: windows_update_in_linux [OPTIONS]\n"
        "\n"
        "Switches to an idle VT, grabs DRM master and renders a fake Windows\n"
        "update screen directly to the physical display. Progress climbs to\n"
        "35%% and freezes; after the timeout the original desktop is restored\n"
        "and the program exits. It NEVER reboots.\n"
        "\n"
        "Must be run as root:  sudo windows_update_in_linux\n"
        "\n"
        "Options:\n"
        "  --timeout=SEC    seconds until auto-restore (default: %d)\n"
        "  -t SEC           same as --timeout=SEC\n"
        "  --help, -h       show this help and exit\n"
        "  --version, -V    show version and exit\n"
        "\n"
        "Environment:\n"
        "  WINDOWS_UPDATE_TIMEOUT   default timeout in seconds\n",
        DEFAULT_TIMEOUT);
}

int main(int argc, char **argv)
{
    unsigned int timeout = DEFAULT_TIMEOUT;
    const char *env = getenv("WINDOWS_UPDATE_TIMEOUT");
    int i;

    if (env && *env) {
        unsigned int v = (unsigned int)strtoul(env, NULL, 10);
        if (v > 0)
            timeout = v;
    }

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_usage(stdout);
            return 0;
        } else if (strcmp(a, "--version") == 0 || strcmp(a, "-V") == 0) {
            print_version();
            return 0;
        } else if (strncmp(a, "--timeout=", 10) == 0) {
            unsigned int v = (unsigned int)strtoul(a + 10, NULL, 10);
            if (v > 0)
                timeout = v;
        } else if (strcmp(a, "-t") == 0 && i + 1 < argc) {
            unsigned int v = (unsigned int)strtoul(argv[++i], NULL, 10);
            if (v > 0)
                timeout = v;
        } else {
            fprintf(stderr, "windows_update_in_linux: unknown option '%s' (see --help)\n", a);
            return 2;
        }
    }

    return fake_update_ttydrm_run(timeout);
}
