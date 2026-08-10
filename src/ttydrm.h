/*
 * ttydrm.h — DRM/TTY direct-render fake update screen.
 *
 * Renders the fake Windows update screen directly to the physical display via
 * DRM/KMS, bypassing the desktop entirely. Requires root. See ttydrm.c.
 */
#ifndef WINDOWS_UPDATE_IN_LINUX_TTYDRM_H
#define WINDOWS_UPDATE_IN_LINUX_TTYDRM_H

/*
 * Run the DRM/TTY direct-render fake update screen.
 *
 * Flow: switch to an idle VT -> take over DRM master -> draw the update UI
 * with FreeType -> animate a 50/50 outcome:
 *   - success: progress climbs fast then slow to 99%, waits for a real
 *     background apt update plus at least timeout_sec, hits 100% and
 *     reboots (unless no_reboot, which restores the desktop instead)
 *   - failure: progress freezes at a random 35%..42%, then after timeout_sec
 *     the embedded bsod binary (heyManNice/bsod) is exec'd to take over the
 *     screen with an error message
 *
 * A detached "apt-get update && apt-get upgrade" is forked only on the
 * success path (never on the failure/BSOD path), logging to
 * windows-update-real.log in the current directory; the success path waits
 * for it to finish before hitting 100%.
 * no_reboot: never call reboot; restore the desktop and exit instead
 *   (the BSOD hand-off then uses --restore so it also does not reboot).
 *
 * Returns 0 on a clean release, 2 if not run as root, 1 on error.
 */
int fake_update_ttydrm_run(unsigned int timeout_sec, int no_reboot);

#endif /* WINDOWS_UPDATE_IN_LINUX_TTYDRM_H */
