/* fstab(5) API: declared by <fstab.h>, exported by no SDK library. GIO's
 * unix mount code (g_unix_mount_points_get) calls it; a payload has no
 * fstab, so report an empty table. */
#include <fstab.h>
#include <stddef.h>

int setfsent(void) { return 0; }
struct fstab *getfsent(void) { return NULL; }
void endfsent(void) { }
