/*
 * titlecrash.c: common/crashguard.c for the browser title. Fatal signals
 * (in WebKit too, with pages in the browser process) are reported with
 * their pc and a stack scan, to the log and to /data/ps5webkit/crash-title.txt
 * (ps5browser.c main), then the title ends with _exit(70).
 *
 * Without crashguard's pthread_create wrapper: titlemem.c has the title's.
 *
 * Addresses: "elf +0x..." in the unstripped title ELF that package.sh keeps
 * under titles/debug/<build id>/ (the build id is in the log at start).
 */
#define PS5WK_CRASHGUARD_NO_THREAD_WRAP 1
#include "../common/crashguard.c"
