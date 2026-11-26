/*
 * titlecrash.c: common/crashguard.c for the browser title. Fatal signals
 * (in WebKit too, with pages in the browser process) are reported with
 * their pc and a stack scan, to the log and to /data/ps5webkit/crash-title.txt
 * (ps5browser.c main), then the title SIGKILLs itself (PS5WK_CRASHGUARD_TITLE:
 * _exit left an aborted title alive for minutes), with a watchdog thread
 * that does the same 8 s after the handler started if it hasn't ended yet.
 *
 * Without crashguard's pthread_create wrapper: titlemem.c has the title's.
 *
 * Addresses: "elf +0x..." in the unstripped title ELF that package.sh keeps
 * under titles/debug/<build id>/ (the build id is in the log at start).
 */
#define PS5WK_CRASHGUARD_NO_THREAD_WRAP 1
#define PS5WK_CRASHGUARD_TITLE 1
#include "../common/crashguard.c"
