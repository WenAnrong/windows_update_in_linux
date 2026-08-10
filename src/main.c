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

#define DEFAULT_TIMEOUT 20
#define VERSION "1.1.0"

static void print_version(void)
{
    printf("windows_update_in_linux %s\n", VERSION);
    printf("Fake Windows update screen rendered directly on the TTY via DRM/KMS.\n");
    printf("Requires root. 50%% update-success -> reboot, 50%% failure -> BSOD.\n");
    printf("Use --no-reboot to restore the desktop instead of rebooting.\n");
}

static void print_usage(FILE *out)
{
    fprintf(out,
        "Usage: windows_update_in_linux [OPTIONS]\n"
        "\n"
        "Switches to an idle VT, grabs DRM master and renders a fake Windows\n"
        "update screen directly to the physical display. 50%% of the time the\n"
        "update succeeds (progress to 100%%, then reboot), 50%% it fails\n"
        "(progress freezes at 35%%..42%%, then the built-in BSOD takes over).\n"
        "Use --no-reboot to never reboot.\n"
        "\n"
        "Must be run as root:  sudo windows_update_in_linux\n"
        "\n"
        "Options:\n"
        "  --timeout=SEC    minimum runtime / time before the BSOD hand-off\n"
        "                   (default: %d)\n"
        "  -t SEC           same as --timeout=SEC\n"
        "  --no-reboot      never reboot: restore the desktop and exit even\n"
        "                   after a successful update; the BSOD also restores\n"
        "                   instead of rebooting\n"
        "  --help, -h       show this help and exit\n"
        "  --version, -V    show version and exit\n"
        "\n"
        "Environment:\n"
        "  WINDOWS_UPDATE_TIMEOUT   default timeout in seconds\n"
        "  WINDOWS_UPDATE_MODE      'success' or 'failure' to force an outcome\n",
        DEFAULT_TIMEOUT);
}

int main(int argc, char **argv)
{
    unsigned int timeout = DEFAULT_TIMEOUT;
    int no_reboot = 0;
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
        } else if (strcmp(a, "--no-reboot") == 0) {
            no_reboot = 1;
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

    return fake_update_ttydrm_run(timeout, no_reboot);
}
