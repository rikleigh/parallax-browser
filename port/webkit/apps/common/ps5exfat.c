/* ps5exfat.c: a folder as one exFAT image file; see ps5exfat.h.
 *
 * What the console's image mounters expect (looked up November 2026)
 * ------------------------------------------------------------------
 * The mounter is ShadowMountPlus (drakmor; 1.7beta12 of 2026-11-08, on
 * kstuff-lite 1.07+). The other tools that make PS5 exFAT images (ps5upload,
 * MkPFS, PS5 Image Studio, exFAT2AMPR) all aim at "an image ShadowMountPlus
 * mounts". etaHEN doesn't mount images itself: its /data/etaHEN/games
 * folders are among ShadowMountPlus's scan roots. The PS5Browser README
 * (Vibe-Code-Jesus) only says it converts game folders to exFAT images and
 * gives no layout. What ShadowMountPlus's README, its image scripts
 * (mkexfat.sh, mkexfat_macos.sh, New-OsfExfatImage.ps1) and its source
 * (src/sm_image.c, src/sm_scan.c, include/sm_mount_defs.h) say:
 *   - The extension is .exfat. (.ffpkg is UFS2, .ffpfs and .ffpfsc are PFS.)
 *     Images go in /data/homebrew/, /data/etaHEN/games/,
 *     /mnt/usb0..7/homebrew/ (and etaHEN/games/ there), /mnt/ext0..1/homebrew/,
 *     or are listed by path in /data/shadowmount/manual.lst. The fast mount
 *     profile (LVD type 5 with BFS "sdimg" batching) applies only to images
 *     under /data or /user. Elsewhere the 1.6 profile is used.
 *   - The image is a raw exFAT volume with no MBR or GPT: the scripts format
 *     the whole file (mkfs.exfat, newfs_exfat on the attached file, OSFMount
 *     "logical volume"), and the file is attached as an LVD or md device and
 *     nmount'ed as exfatfs straight away.
 *   - The game folder's contents go at the image root (eboot.bin and
 *     sce_sys/param.json at the root): "Do not add another enclosing game
 *     directory inside the image". The builders take the game root folder
 *     ("not its parent folder").
 *   - Clusters are 64 KiB: "create it with a 64 KB allocation unit", and
 *     "If creating images manually, use the same allocation unit". The LVD's
 *     secondary unit is 64 KiB, and the BFS fast path only batches 64 KiB
 *     requests at 64 KiB-aligned offsets of the backing file, so the cluster
 *     heap starts on a 64 KiB boundary too. The logical sector stays 512
 *     bytes for exFAT ("do not set image_sector=...:65536").
 *   - Size: the scripts take the files rounded up to clusters, plus FAT,
 *     bitmap and entries and 32 MiB of metadata, plus a spare of 0.5% of
 *     that clamped to 64..512 MiB (free space for read-write image_rw=
 *     mounts: saves, small patches), and round up to 1 MiB.
 *   - There's no label convention: mkfs's default is none, and the Windows
 *     script uses "OSFIMG".
 * The exFAT writer in ps5upload (phantomptr) was tested on a CFI-1115A on
 * 13.60 (this project's firmware) with ShadowMountPlus 1.7. It adds two
 * findings. First, the up-case table must be the specification's
 * recommended one (5836 bytes compressed, checksum E619D30D): a generated
 * table that macOS checked and mounted was refused by the PS5 at nmount
 * with EINVAL. Second, it writes a FAT chain for every allocation and
 * leaves NoFatChain clear, so drivers that follow chains and drivers that
 * trust contiguity read the same bytes. PS5 Dump Forge writes the chains
 * too, with NoFatChain set. The sources agree on 64 KiB clusters and
 * contents at the root, and none of them partitions the image.
 * Sources:
 *   https://github.com/drakmor/ShadowMountPlus (README.md, mkexfat.sh,
 *     mkexfat_macos.sh, New-OsfExfatImage.ps1, src/sm_image.c,
 *     src/sm_scan.c, include/sm_mount_defs.h)
 *   https://newreleases.io/project/github/drakmor/ShadowMountPlus/release/1.7beta1
 *   https://github.com/phantomptr/ps5upload
 *     (engine/crates/ps5upload-fpkg/src/exfat_write.rs, its doc comments)
 *   https://github.com/psbrew/mkpfs
 *   https://github.com/Vibe-Code-Jesus/PS5Browser-Releases (README)
 *   https://learn.microsoft.com/en-us/windows/win32/fileio/exfat-specification
 *
 * What this writes, then (from Microsoft's exFAT specification):
 *   sectors 0..11      main boot region: boot sector, 8 extended boot
 *                      sectors, OEM parameters, reserved, checksum sector
 *   sectors 12..23     the backup boot region, the same bytes
 *   sector 128 (64K)   the FAT (one), padded so the heap starts on a
 *                      64 KiB (or cluster, if bigger) boundary
 *   cluster 2..        allocation bitmap, up-case table, root directory,
 *                      the other directories (breadth first), then every
 *                      file's data in the same order; the free clusters
 *                      after them (the spare) are left sparse
 * 512-byte sectors and 64 KiB clusters by default. Everything is
 * contiguous and also chained in the FAT, with NoFatChain clear (ps5upload's
 * choice, the console-tested one). The image's size is what's needed plus
 * ShadowMountPlus's spare, rounded up to 1 MiB. The source folder's contents
 * are the image root. Timestamps come from the source in UTC. The up-case
 * table is the specification's.
 */
#include "ps5exfat.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define SECTOR 512u
#define FAT_OFFSET 128u /* sectors: 64 KiB, as newfs_exfat and mkfs.exfat put it */
#define HEAP_ALIGN (64u * 1024) /* the LVD secondary unit, the BFS fast path's block */
#define DEFAULT_CLUSTER (64u * 1024)
#define MAX_CLUSTERS 0xfffffff5u
#define MAX_DIRECTORY (256u * 1024 * 1024) /* a directory's largest DataLength */
#define SPARE_MIN (64ull << 20)
#define SPARE_MAX (512ull << 20)
#define IMAGE_ALIGN (1ull << 20)
#define BUFFER (8u * 1024 * 1024)
#define DEFAULT_MARGIN (64ull << 20)

/* The specification's recommended up-case table in its compressed form
 * (0xffff, n: n characters that map to themselves), 5836 bytes, checksum
 * 0xe619d30d (section 7.2.5.1). The PS5 refuses other tables. */
static const uint16_t upcaseCompressed[2918] = {
    0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0008, 0x0009, 0x000a, 0x000b,
    0x000c, 0x000d, 0x000e, 0x000f, 0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017,
    0x0018, 0x0019, 0x001a, 0x001b, 0x001c, 0x001d, 0x001e, 0x001f, 0x0020, 0x0021, 0x0022, 0x0023,
    0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002a, 0x002b, 0x002c, 0x002d, 0x002e, 0x002f,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037, 0x0038, 0x0039, 0x003a, 0x003b,
    0x003c, 0x003d, 0x003e, 0x003f, 0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004a, 0x004b, 0x004c, 0x004d, 0x004e, 0x004f, 0x0050, 0x0051, 0x0052, 0x0053,
    0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005a, 0x005b, 0x005c, 0x005d, 0x005e, 0x005f,
    0x0060, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004a, 0x004b,
    0x004c, 0x004d, 0x004e, 0x004f, 0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005a, 0x007b, 0x007c, 0x007d, 0x007e, 0x007f, 0x0080, 0x0081, 0x0082, 0x0083,
    0x0084, 0x0085, 0x0086, 0x0087, 0x0088, 0x0089, 0x008a, 0x008b, 0x008c, 0x008d, 0x008e, 0x008f,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097, 0x0098, 0x0099, 0x009a, 0x009b,
    0x009c, 0x009d, 0x009e, 0x009f, 0x00a0, 0x00a1, 0x00a2, 0x00a3, 0x00a4, 0x00a5, 0x00a6, 0x00a7,
    0x00a8, 0x00a9, 0x00aa, 0x00ab, 0x00ac, 0x00ad, 0x00ae, 0x00af, 0x00b0, 0x00b1, 0x00b2, 0x00b3,
    0x00b4, 0x00b5, 0x00b6, 0x00b7, 0x00b8, 0x00b9, 0x00ba, 0x00bb, 0x00bc, 0x00bd, 0x00be, 0x00bf,
    0x00c0, 0x00c1, 0x00c2, 0x00c3, 0x00c4, 0x00c5, 0x00c6, 0x00c7, 0x00c8, 0x00c9, 0x00ca, 0x00cb,
    0x00cc, 0x00cd, 0x00ce, 0x00cf, 0x00d0, 0x00d1, 0x00d2, 0x00d3, 0x00d4, 0x00d5, 0x00d6, 0x00d7,
    0x00d8, 0x00d9, 0x00da, 0x00db, 0x00dc, 0x00dd, 0x00de, 0x00df, 0x00c0, 0x00c1, 0x00c2, 0x00c3,
    0x00c4, 0x00c5, 0x00c6, 0x00c7, 0x00c8, 0x00c9, 0x00ca, 0x00cb, 0x00cc, 0x00cd, 0x00ce, 0x00cf,
    0x00d0, 0x00d1, 0x00d2, 0x00d3, 0x00d4, 0x00d5, 0x00d6, 0x00f7, 0x00d8, 0x00d9, 0x00da, 0x00db,
    0x00dc, 0x00dd, 0x00de, 0x0178, 0x0100, 0x0100, 0x0102, 0x0102, 0x0104, 0x0104, 0x0106, 0x0106,
    0x0108, 0x0108, 0x010a, 0x010a, 0x010c, 0x010c, 0x010e, 0x010e, 0x0110, 0x0110, 0x0112, 0x0112,
    0x0114, 0x0114, 0x0116, 0x0116, 0x0118, 0x0118, 0x011a, 0x011a, 0x011c, 0x011c, 0x011e, 0x011e,
    0x0120, 0x0120, 0x0122, 0x0122, 0x0124, 0x0124, 0x0126, 0x0126, 0x0128, 0x0128, 0x012a, 0x012a,
    0x012c, 0x012c, 0x012e, 0x012e, 0x0130, 0x0131, 0x0132, 0x0132, 0x0134, 0x0134, 0x0136, 0x0136,
    0x0138, 0x0139, 0x0139, 0x013b, 0x013b, 0x013d, 0x013d, 0x013f, 0x013f, 0x0141, 0x0141, 0x0143,
    0x0143, 0x0145, 0x0145, 0x0147, 0x0147, 0x0149, 0x014a, 0x014a, 0x014c, 0x014c, 0x014e, 0x014e,
    0x0150, 0x0150, 0x0152, 0x0152, 0x0154, 0x0154, 0x0156, 0x0156, 0x0158, 0x0158, 0x015a, 0x015a,
    0x015c, 0x015c, 0x015e, 0x015e, 0x0160, 0x0160, 0x0162, 0x0162, 0x0164, 0x0164, 0x0166, 0x0166,
    0x0168, 0x0168, 0x016a, 0x016a, 0x016c, 0x016c, 0x016e, 0x016e, 0x0170, 0x0170, 0x0172, 0x0172,
    0x0174, 0x0174, 0x0176, 0x0176, 0x0178, 0x0179, 0x0179, 0x017b, 0x017b, 0x017d, 0x017d, 0x017f,
    0x0243, 0x0181, 0x0182, 0x0182, 0x0184, 0x0184, 0x0186, 0x0187, 0x0187, 0x0189, 0x018a, 0x018b,
    0x018b, 0x018d, 0x018e, 0x018f, 0x0190, 0x0191, 0x0191, 0x0193, 0x0194, 0x01f6, 0x0196, 0x0197,
    0x0198, 0x0198, 0x023d, 0x019b, 0x019c, 0x019d, 0x0220, 0x019f, 0x01a0, 0x01a0, 0x01a2, 0x01a2,
    0x01a4, 0x01a4, 0x01a6, 0x01a7, 0x01a7, 0x01a9, 0x01aa, 0x01ab, 0x01ac, 0x01ac, 0x01ae, 0x01af,
    0x01af, 0x01b1, 0x01b2, 0x01b3, 0x01b3, 0x01b5, 0x01b5, 0x01b7, 0x01b8, 0x01b8, 0x01ba, 0x01bb,
    0x01bc, 0x01bc, 0x01be, 0x01f7, 0x01c0, 0x01c1, 0x01c2, 0x01c3, 0x01c4, 0x01c5, 0x01c4, 0x01c7,
    0x01c8, 0x01c7, 0x01ca, 0x01cb, 0x01ca, 0x01cd, 0x01cd, 0x01cf, 0x01cf, 0x01d1, 0x01d1, 0x01d3,
    0x01d3, 0x01d5, 0x01d5, 0x01d7, 0x01d7, 0x01d9, 0x01d9, 0x01db, 0x01db, 0x018e, 0x01de, 0x01de,
    0x01e0, 0x01e0, 0x01e2, 0x01e2, 0x01e4, 0x01e4, 0x01e6, 0x01e6, 0x01e8, 0x01e8, 0x01ea, 0x01ea,
    0x01ec, 0x01ec, 0x01ee, 0x01ee, 0x01f0, 0x01f1, 0x01f2, 0x01f1, 0x01f4, 0x01f4, 0x01f6, 0x01f7,
    0x01f8, 0x01f8, 0x01fa, 0x01fa, 0x01fc, 0x01fc, 0x01fe, 0x01fe, 0x0200, 0x0200, 0x0202, 0x0202,
    0x0204, 0x0204, 0x0206, 0x0206, 0x0208, 0x0208, 0x020a, 0x020a, 0x020c, 0x020c, 0x020e, 0x020e,
    0x0210, 0x0210, 0x0212, 0x0212, 0x0214, 0x0214, 0x0216, 0x0216, 0x0218, 0x0218, 0x021a, 0x021a,
    0x021c, 0x021c, 0x021e, 0x021e, 0x0220, 0x0221, 0x0222, 0x0222, 0x0224, 0x0224, 0x0226, 0x0226,
    0x0228, 0x0228, 0x022a, 0x022a, 0x022c, 0x022c, 0x022e, 0x022e, 0x0230, 0x0230, 0x0232, 0x0232,
    0x0234, 0x0235, 0x0236, 0x0237, 0x0238, 0x0239, 0x2c65, 0x023b, 0x023b, 0x023d, 0x2c66, 0x023f,
    0x0240, 0x0241, 0x0241, 0x0243, 0x0244, 0x0245, 0x0246, 0x0246, 0x0248, 0x0248, 0x024a, 0x024a,
    0x024c, 0x024c, 0x024e, 0x024e, 0x0250, 0x0251, 0x0252, 0x0181, 0x0186, 0x0255, 0x0189, 0x018a,
    0x0258, 0x018f, 0x025a, 0x0190, 0x025c, 0x025d, 0x025e, 0x025f, 0x0193, 0x0261, 0x0262, 0x0194,
    0x0264, 0x0265, 0x0266, 0x0267, 0x0197, 0x0196, 0x026a, 0x2c62, 0x026c, 0x026d, 0x026e, 0x019c,
    0x0270, 0x0271, 0x019d, 0x0273, 0x0274, 0x019f, 0x0276, 0x0277, 0x0278, 0x0279, 0x027a, 0x027b,
    0x027c, 0x2c64, 0x027e, 0x027f, 0x01a6, 0x0281, 0x0282, 0x01a9, 0x0284, 0x0285, 0x0286, 0x0287,
    0x01ae, 0x0244, 0x01b1, 0x01b2, 0x0245, 0x028d, 0x028e, 0x028f, 0x0290, 0x0291, 0x01b7, 0x0293,
    0x0294, 0x0295, 0x0296, 0x0297, 0x0298, 0x0299, 0x029a, 0x029b, 0x029c, 0x029d, 0x029e, 0x029f,
    0x02a0, 0x02a1, 0x02a2, 0x02a3, 0x02a4, 0x02a5, 0x02a6, 0x02a7, 0x02a8, 0x02a9, 0x02aa, 0x02ab,
    0x02ac, 0x02ad, 0x02ae, 0x02af, 0x02b0, 0x02b1, 0x02b2, 0x02b3, 0x02b4, 0x02b5, 0x02b6, 0x02b7,
    0x02b8, 0x02b9, 0x02ba, 0x02bb, 0x02bc, 0x02bd, 0x02be, 0x02bf, 0x02c0, 0x02c1, 0x02c2, 0x02c3,
    0x02c4, 0x02c5, 0x02c6, 0x02c7, 0x02c8, 0x02c9, 0x02ca, 0x02cb, 0x02cc, 0x02cd, 0x02ce, 0x02cf,
    0x02d0, 0x02d1, 0x02d2, 0x02d3, 0x02d4, 0x02d5, 0x02d6, 0x02d7, 0x02d8, 0x02d9, 0x02da, 0x02db,
    0x02dc, 0x02dd, 0x02de, 0x02df, 0x02e0, 0x02e1, 0x02e2, 0x02e3, 0x02e4, 0x02e5, 0x02e6, 0x02e7,
    0x02e8, 0x02e9, 0x02ea, 0x02eb, 0x02ec, 0x02ed, 0x02ee, 0x02ef, 0x02f0, 0x02f1, 0x02f2, 0x02f3,
    0x02f4, 0x02f5, 0x02f6, 0x02f7, 0x02f8, 0x02f9, 0x02fa, 0x02fb, 0x02fc, 0x02fd, 0x02fe, 0x02ff,
    0x0300, 0x0301, 0x0302, 0x0303, 0x0304, 0x0305, 0x0306, 0x0307, 0x0308, 0x0309, 0x030a, 0x030b,
    0x030c, 0x030d, 0x030e, 0x030f, 0x0310, 0x0311, 0x0312, 0x0313, 0x0314, 0x0315, 0x0316, 0x0317,
    0x0318, 0x0319, 0x031a, 0x031b, 0x031c, 0x031d, 0x031e, 0x031f, 0x0320, 0x0321, 0x0322, 0x0323,
    0x0324, 0x0325, 0x0326, 0x0327, 0x0328, 0x0329, 0x032a, 0x032b, 0x032c, 0x032d, 0x032e, 0x032f,
    0x0330, 0x0331, 0x0332, 0x0333, 0x0334, 0x0335, 0x0336, 0x0337, 0x0338, 0x0339, 0x033a, 0x033b,
    0x033c, 0x033d, 0x033e, 0x033f, 0x0340, 0x0341, 0x0342, 0x0343, 0x0344, 0x0345, 0x0346, 0x0347,
    0x0348, 0x0349, 0x034a, 0x034b, 0x034c, 0x034d, 0x034e, 0x034f, 0x0350, 0x0351, 0x0352, 0x0353,
    0x0354, 0x0355, 0x0356, 0x0357, 0x0358, 0x0359, 0x035a, 0x035b, 0x035c, 0x035d, 0x035e, 0x035f,
    0x0360, 0x0361, 0x0362, 0x0363, 0x0364, 0x0365, 0x0366, 0x0367, 0x0368, 0x0369, 0x036a, 0x036b,
    0x036c, 0x036d, 0x036e, 0x036f, 0x0370, 0x0371, 0x0372, 0x0373, 0x0374, 0x0375, 0x0376, 0x0377,
    0x0378, 0x0379, 0x037a, 0x03fd, 0x03fe, 0x03ff, 0x037e, 0x037f, 0x0380, 0x0381, 0x0382, 0x0383,
    0x0384, 0x0385, 0x0386, 0x0387, 0x0388, 0x0389, 0x038a, 0x038b, 0x038c, 0x038d, 0x038e, 0x038f,
    0x0390, 0x0391, 0x0392, 0x0393, 0x0394, 0x0395, 0x0396, 0x0397, 0x0398, 0x0399, 0x039a, 0x039b,
    0x039c, 0x039d, 0x039e, 0x039f, 0x03a0, 0x03a1, 0x03a2, 0x03a3, 0x03a4, 0x03a5, 0x03a6, 0x03a7,
    0x03a8, 0x03a9, 0x03aa, 0x03ab, 0x0386, 0x0388, 0x0389, 0x038a, 0x03b0, 0x0391, 0x0392, 0x0393,
    0x0394, 0x0395, 0x0396, 0x0397, 0x0398, 0x0399, 0x039a, 0x039b, 0x039c, 0x039d, 0x039e, 0x039f,
    0x03a0, 0x03a1, 0x03a3, 0x03a3, 0x03a4, 0x03a5, 0x03a6, 0x03a7, 0x03a8, 0x03a9, 0x03aa, 0x03ab,
    0x038c, 0x038e, 0x038f, 0x03cf, 0x03d0, 0x03d1, 0x03d2, 0x03d3, 0x03d4, 0x03d5, 0x03d6, 0x03d7,
    0x03d8, 0x03d8, 0x03da, 0x03da, 0x03dc, 0x03dc, 0x03de, 0x03de, 0x03e0, 0x03e0, 0x03e2, 0x03e2,
    0x03e4, 0x03e4, 0x03e6, 0x03e6, 0x03e8, 0x03e8, 0x03ea, 0x03ea, 0x03ec, 0x03ec, 0x03ee, 0x03ee,
    0x03f0, 0x03f1, 0x03f9, 0x03f3, 0x03f4, 0x03f5, 0x03f6, 0x03f7, 0x03f7, 0x03f9, 0x03fa, 0x03fa,
    0x03fc, 0x03fd, 0x03fe, 0x03ff, 0x0400, 0x0401, 0x0402, 0x0403, 0x0404, 0x0405, 0x0406, 0x0407,
    0x0408, 0x0409, 0x040a, 0x040b, 0x040c, 0x040d, 0x040e, 0x040f, 0x0410, 0x0411, 0x0412, 0x0413,
    0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041a, 0x041b, 0x041c, 0x041d, 0x041e, 0x041f,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042a, 0x042b,
    0x042c, 0x042d, 0x042e, 0x042f, 0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    0x0418, 0x0419, 0x041a, 0x041b, 0x041c, 0x041d, 0x041e, 0x041f, 0x0420, 0x0421, 0x0422, 0x0423,
    0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042a, 0x042b, 0x042c, 0x042d, 0x042e, 0x042f,
    0x0400, 0x0401, 0x0402, 0x0403, 0x0404, 0x0405, 0x0406, 0x0407, 0x0408, 0x0409, 0x040a, 0x040b,
    0x040c, 0x040d, 0x040e, 0x040f, 0x0460, 0x0460, 0x0462, 0x0462, 0x0464, 0x0464, 0x0466, 0x0466,
    0x0468, 0x0468, 0x046a, 0x046a, 0x046c, 0x046c, 0x046e, 0x046e, 0x0470, 0x0470, 0x0472, 0x0472,
    0x0474, 0x0474, 0x0476, 0x0476, 0x0478, 0x0478, 0x047a, 0x047a, 0x047c, 0x047c, 0x047e, 0x047e,
    0x0480, 0x0480, 0x0482, 0x0483, 0x0484, 0x0485, 0x0486, 0x0487, 0x0488, 0x0489, 0x048a, 0x048a,
    0x048c, 0x048c, 0x048e, 0x048e, 0x0490, 0x0490, 0x0492, 0x0492, 0x0494, 0x0494, 0x0496, 0x0496,
    0x0498, 0x0498, 0x049a, 0x049a, 0x049c, 0x049c, 0x049e, 0x049e, 0x04a0, 0x04a0, 0x04a2, 0x04a2,
    0x04a4, 0x04a4, 0x04a6, 0x04a6, 0x04a8, 0x04a8, 0x04aa, 0x04aa, 0x04ac, 0x04ac, 0x04ae, 0x04ae,
    0x04b0, 0x04b0, 0x04b2, 0x04b2, 0x04b4, 0x04b4, 0x04b6, 0x04b6, 0x04b8, 0x04b8, 0x04ba, 0x04ba,
    0x04bc, 0x04bc, 0x04be, 0x04be, 0x04c0, 0x04c1, 0x04c1, 0x04c3, 0x04c3, 0x04c5, 0x04c5, 0x04c7,
    0x04c7, 0x04c9, 0x04c9, 0x04cb, 0x04cb, 0x04cd, 0x04cd, 0x04c0, 0x04d0, 0x04d0, 0x04d2, 0x04d2,
    0x04d4, 0x04d4, 0x04d6, 0x04d6, 0x04d8, 0x04d8, 0x04da, 0x04da, 0x04dc, 0x04dc, 0x04de, 0x04de,
    0x04e0, 0x04e0, 0x04e2, 0x04e2, 0x04e4, 0x04e4, 0x04e6, 0x04e6, 0x04e8, 0x04e8, 0x04ea, 0x04ea,
    0x04ec, 0x04ec, 0x04ee, 0x04ee, 0x04f0, 0x04f0, 0x04f2, 0x04f2, 0x04f4, 0x04f4, 0x04f6, 0x04f6,
    0x04f8, 0x04f8, 0x04fa, 0x04fa, 0x04fc, 0x04fc, 0x04fe, 0x04fe, 0x0500, 0x0500, 0x0502, 0x0502,
    0x0504, 0x0504, 0x0506, 0x0506, 0x0508, 0x0508, 0x050a, 0x050a, 0x050c, 0x050c, 0x050e, 0x050e,
    0x0510, 0x0510, 0x0512, 0x0512, 0x0514, 0x0515, 0x0516, 0x0517, 0x0518, 0x0519, 0x051a, 0x051b,
    0x051c, 0x051d, 0x051e, 0x051f, 0x0520, 0x0521, 0x0522, 0x0523, 0x0524, 0x0525, 0x0526, 0x0527,
    0x0528, 0x0529, 0x052a, 0x052b, 0x052c, 0x052d, 0x052e, 0x052f, 0x0530, 0x0531, 0x0532, 0x0533,
    0x0534, 0x0535, 0x0536, 0x0537, 0x0538, 0x0539, 0x053a, 0x053b, 0x053c, 0x053d, 0x053e, 0x053f,
    0x0540, 0x0541, 0x0542, 0x0543, 0x0544, 0x0545, 0x0546, 0x0547, 0x0548, 0x0549, 0x054a, 0x054b,
    0x054c, 0x054d, 0x054e, 0x054f, 0x0550, 0x0551, 0x0552, 0x0553, 0x0554, 0x0555, 0x0556, 0x0557,
    0x0558, 0x0559, 0x055a, 0x055b, 0x055c, 0x055d, 0x055e, 0x055f, 0x0560, 0x0531, 0x0532, 0x0533,
    0x0534, 0x0535, 0x0536, 0x0537, 0x0538, 0x0539, 0x053a, 0x053b, 0x053c, 0x053d, 0x053e, 0x053f,
    0x0540, 0x0541, 0x0542, 0x0543, 0x0544, 0x0545, 0x0546, 0x0547, 0x0548, 0x0549, 0x054a, 0x054b,
    0x054c, 0x054d, 0x054e, 0x054f, 0x0550, 0x0551, 0x0552, 0x0553, 0x0554, 0x0555, 0x0556, 0xffff,
    0x17f6, 0x2c63, 0x1d7e, 0x1d7f, 0x1d80, 0x1d81, 0x1d82, 0x1d83, 0x1d84, 0x1d85, 0x1d86, 0x1d87,
    0x1d88, 0x1d89, 0x1d8a, 0x1d8b, 0x1d8c, 0x1d8d, 0x1d8e, 0x1d8f, 0x1d90, 0x1d91, 0x1d92, 0x1d93,
    0x1d94, 0x1d95, 0x1d96, 0x1d97, 0x1d98, 0x1d99, 0x1d9a, 0x1d9b, 0x1d9c, 0x1d9d, 0x1d9e, 0x1d9f,
    0x1da0, 0x1da1, 0x1da2, 0x1da3, 0x1da4, 0x1da5, 0x1da6, 0x1da7, 0x1da8, 0x1da9, 0x1daa, 0x1dab,
    0x1dac, 0x1dad, 0x1dae, 0x1daf, 0x1db0, 0x1db1, 0x1db2, 0x1db3, 0x1db4, 0x1db5, 0x1db6, 0x1db7,
    0x1db8, 0x1db9, 0x1dba, 0x1dbb, 0x1dbc, 0x1dbd, 0x1dbe, 0x1dbf, 0x1dc0, 0x1dc1, 0x1dc2, 0x1dc3,
    0x1dc4, 0x1dc5, 0x1dc6, 0x1dc7, 0x1dc8, 0x1dc9, 0x1dca, 0x1dcb, 0x1dcc, 0x1dcd, 0x1dce, 0x1dcf,
    0x1dd0, 0x1dd1, 0x1dd2, 0x1dd3, 0x1dd4, 0x1dd5, 0x1dd6, 0x1dd7, 0x1dd8, 0x1dd9, 0x1dda, 0x1ddb,
    0x1ddc, 0x1ddd, 0x1dde, 0x1ddf, 0x1de0, 0x1de1, 0x1de2, 0x1de3, 0x1de4, 0x1de5, 0x1de6, 0x1de7,
    0x1de8, 0x1de9, 0x1dea, 0x1deb, 0x1dec, 0x1ded, 0x1dee, 0x1def, 0x1df0, 0x1df1, 0x1df2, 0x1df3,
    0x1df4, 0x1df5, 0x1df6, 0x1df7, 0x1df8, 0x1df9, 0x1dfa, 0x1dfb, 0x1dfc, 0x1dfd, 0x1dfe, 0x1dff,
    0x1e00, 0x1e00, 0x1e02, 0x1e02, 0x1e04, 0x1e04, 0x1e06, 0x1e06, 0x1e08, 0x1e08, 0x1e0a, 0x1e0a,
    0x1e0c, 0x1e0c, 0x1e0e, 0x1e0e, 0x1e10, 0x1e10, 0x1e12, 0x1e12, 0x1e14, 0x1e14, 0x1e16, 0x1e16,
    0x1e18, 0x1e18, 0x1e1a, 0x1e1a, 0x1e1c, 0x1e1c, 0x1e1e, 0x1e1e, 0x1e20, 0x1e20, 0x1e22, 0x1e22,
    0x1e24, 0x1e24, 0x1e26, 0x1e26, 0x1e28, 0x1e28, 0x1e2a, 0x1e2a, 0x1e2c, 0x1e2c, 0x1e2e, 0x1e2e,
    0x1e30, 0x1e30, 0x1e32, 0x1e32, 0x1e34, 0x1e34, 0x1e36, 0x1e36, 0x1e38, 0x1e38, 0x1e3a, 0x1e3a,
    0x1e3c, 0x1e3c, 0x1e3e, 0x1e3e, 0x1e40, 0x1e40, 0x1e42, 0x1e42, 0x1e44, 0x1e44, 0x1e46, 0x1e46,
    0x1e48, 0x1e48, 0x1e4a, 0x1e4a, 0x1e4c, 0x1e4c, 0x1e4e, 0x1e4e, 0x1e50, 0x1e50, 0x1e52, 0x1e52,
    0x1e54, 0x1e54, 0x1e56, 0x1e56, 0x1e58, 0x1e58, 0x1e5a, 0x1e5a, 0x1e5c, 0x1e5c, 0x1e5e, 0x1e5e,
    0x1e60, 0x1e60, 0x1e62, 0x1e62, 0x1e64, 0x1e64, 0x1e66, 0x1e66, 0x1e68, 0x1e68, 0x1e6a, 0x1e6a,
    0x1e6c, 0x1e6c, 0x1e6e, 0x1e6e, 0x1e70, 0x1e70, 0x1e72, 0x1e72, 0x1e74, 0x1e74, 0x1e76, 0x1e76,
    0x1e78, 0x1e78, 0x1e7a, 0x1e7a, 0x1e7c, 0x1e7c, 0x1e7e, 0x1e7e, 0x1e80, 0x1e80, 0x1e82, 0x1e82,
    0x1e84, 0x1e84, 0x1e86, 0x1e86, 0x1e88, 0x1e88, 0x1e8a, 0x1e8a, 0x1e8c, 0x1e8c, 0x1e8e, 0x1e8e,
    0x1e90, 0x1e90, 0x1e92, 0x1e92, 0x1e94, 0x1e94, 0x1e96, 0x1e97, 0x1e98, 0x1e99, 0x1e9a, 0x1e9b,
    0x1e9c, 0x1e9d, 0x1e9e, 0x1e9f, 0x1ea0, 0x1ea0, 0x1ea2, 0x1ea2, 0x1ea4, 0x1ea4, 0x1ea6, 0x1ea6,
    0x1ea8, 0x1ea8, 0x1eaa, 0x1eaa, 0x1eac, 0x1eac, 0x1eae, 0x1eae, 0x1eb0, 0x1eb0, 0x1eb2, 0x1eb2,
    0x1eb4, 0x1eb4, 0x1eb6, 0x1eb6, 0x1eb8, 0x1eb8, 0x1eba, 0x1eba, 0x1ebc, 0x1ebc, 0x1ebe, 0x1ebe,
    0x1ec0, 0x1ec0, 0x1ec2, 0x1ec2, 0x1ec4, 0x1ec4, 0x1ec6, 0x1ec6, 0x1ec8, 0x1ec8, 0x1eca, 0x1eca,
    0x1ecc, 0x1ecc, 0x1ece, 0x1ece, 0x1ed0, 0x1ed0, 0x1ed2, 0x1ed2, 0x1ed4, 0x1ed4, 0x1ed6, 0x1ed6,
    0x1ed8, 0x1ed8, 0x1eda, 0x1eda, 0x1edc, 0x1edc, 0x1ede, 0x1ede, 0x1ee0, 0x1ee0, 0x1ee2, 0x1ee2,
    0x1ee4, 0x1ee4, 0x1ee6, 0x1ee6, 0x1ee8, 0x1ee8, 0x1eea, 0x1eea, 0x1eec, 0x1eec, 0x1eee, 0x1eee,
    0x1ef0, 0x1ef0, 0x1ef2, 0x1ef2, 0x1ef4, 0x1ef4, 0x1ef6, 0x1ef6, 0x1ef8, 0x1ef8, 0x1efa, 0x1efb,
    0x1efc, 0x1efd, 0x1efe, 0x1eff, 0x1f08, 0x1f09, 0x1f0a, 0x1f0b, 0x1f0c, 0x1f0d, 0x1f0e, 0x1f0f,
    0x1f08, 0x1f09, 0x1f0a, 0x1f0b, 0x1f0c, 0x1f0d, 0x1f0e, 0x1f0f, 0x1f18, 0x1f19, 0x1f1a, 0x1f1b,
    0x1f1c, 0x1f1d, 0x1f16, 0x1f17, 0x1f18, 0x1f19, 0x1f1a, 0x1f1b, 0x1f1c, 0x1f1d, 0x1f1e, 0x1f1f,
    0x1f28, 0x1f29, 0x1f2a, 0x1f2b, 0x1f2c, 0x1f2d, 0x1f2e, 0x1f2f, 0x1f28, 0x1f29, 0x1f2a, 0x1f2b,
    0x1f2c, 0x1f2d, 0x1f2e, 0x1f2f, 0x1f38, 0x1f39, 0x1f3a, 0x1f3b, 0x1f3c, 0x1f3d, 0x1f3e, 0x1f3f,
    0x1f38, 0x1f39, 0x1f3a, 0x1f3b, 0x1f3c, 0x1f3d, 0x1f3e, 0x1f3f, 0x1f48, 0x1f49, 0x1f4a, 0x1f4b,
    0x1f4c, 0x1f4d, 0x1f46, 0x1f47, 0x1f48, 0x1f49, 0x1f4a, 0x1f4b, 0x1f4c, 0x1f4d, 0x1f4e, 0x1f4f,
    0x1f50, 0x1f59, 0x1f52, 0x1f5b, 0x1f54, 0x1f5d, 0x1f56, 0x1f5f, 0x1f58, 0x1f59, 0x1f5a, 0x1f5b,
    0x1f5c, 0x1f5d, 0x1f5e, 0x1f5f, 0x1f68, 0x1f69, 0x1f6a, 0x1f6b, 0x1f6c, 0x1f6d, 0x1f6e, 0x1f6f,
    0x1f68, 0x1f69, 0x1f6a, 0x1f6b, 0x1f6c, 0x1f6d, 0x1f6e, 0x1f6f, 0x1fba, 0x1fbb, 0x1fc8, 0x1fc9,
    0x1fca, 0x1fcb, 0x1fda, 0x1fdb, 0x1ff8, 0x1ff9, 0x1fea, 0x1feb, 0x1ffa, 0x1ffb, 0x1f7e, 0x1f7f,
    0x1f88, 0x1f89, 0x1f8a, 0x1f8b, 0x1f8c, 0x1f8d, 0x1f8e, 0x1f8f, 0x1f88, 0x1f89, 0x1f8a, 0x1f8b,
    0x1f8c, 0x1f8d, 0x1f8e, 0x1f8f, 0x1f98, 0x1f99, 0x1f9a, 0x1f9b, 0x1f9c, 0x1f9d, 0x1f9e, 0x1f9f,
    0x1f98, 0x1f99, 0x1f9a, 0x1f9b, 0x1f9c, 0x1f9d, 0x1f9e, 0x1f9f, 0x1fa8, 0x1fa9, 0x1faa, 0x1fab,
    0x1fac, 0x1fad, 0x1fae, 0x1faf, 0x1fa8, 0x1fa9, 0x1faa, 0x1fab, 0x1fac, 0x1fad, 0x1fae, 0x1faf,
    0x1fb8, 0x1fb9, 0x1fb2, 0x1fbc, 0x1fb4, 0x1fb5, 0x1fb6, 0x1fb7, 0x1fb8, 0x1fb9, 0x1fba, 0x1fbb,
    0x1fbc, 0x1fbd, 0x1fbe, 0x1fbf, 0x1fc0, 0x1fc1, 0x1fc2, 0x1fc3, 0x1fc4, 0x1fc5, 0x1fc6, 0x1fc7,
    0x1fc8, 0x1fc9, 0x1fca, 0x1fcb, 0x1fc3, 0x1fcd, 0x1fce, 0x1fcf, 0x1fd8, 0x1fd9, 0x1fd2, 0x1fd3,
    0x1fd4, 0x1fd5, 0x1fd6, 0x1fd7, 0x1fd8, 0x1fd9, 0x1fda, 0x1fdb, 0x1fdc, 0x1fdd, 0x1fde, 0x1fdf,
    0x1fe8, 0x1fe9, 0x1fe2, 0x1fe3, 0x1fe4, 0x1fec, 0x1fe6, 0x1fe7, 0x1fe8, 0x1fe9, 0x1fea, 0x1feb,
    0x1fec, 0x1fed, 0x1fee, 0x1fef, 0x1ff0, 0x1ff1, 0x1ff2, 0x1ff3, 0x1ff4, 0x1ff5, 0x1ff6, 0x1ff7,
    0x1ff8, 0x1ff9, 0x1ffa, 0x1ffb, 0x1ff3, 0x1ffd, 0x1ffe, 0x1fff, 0x2000, 0x2001, 0x2002, 0x2003,
    0x2004, 0x2005, 0x2006, 0x2007, 0x2008, 0x2009, 0x200a, 0x200b, 0x200c, 0x200d, 0x200e, 0x200f,
    0x2010, 0x2011, 0x2012, 0x2013, 0x2014, 0x2015, 0x2016, 0x2017, 0x2018, 0x2019, 0x201a, 0x201b,
    0x201c, 0x201d, 0x201e, 0x201f, 0x2020, 0x2021, 0x2022, 0x2023, 0x2024, 0x2025, 0x2026, 0x2027,
    0x2028, 0x2029, 0x202a, 0x202b, 0x202c, 0x202d, 0x202e, 0x202f, 0x2030, 0x2031, 0x2032, 0x2033,
    0x2034, 0x2035, 0x2036, 0x2037, 0x2038, 0x2039, 0x203a, 0x203b, 0x203c, 0x203d, 0x203e, 0x203f,
    0x2040, 0x2041, 0x2042, 0x2043, 0x2044, 0x2045, 0x2046, 0x2047, 0x2048, 0x2049, 0x204a, 0x204b,
    0x204c, 0x204d, 0x204e, 0x204f, 0x2050, 0x2051, 0x2052, 0x2053, 0x2054, 0x2055, 0x2056, 0x2057,
    0x2058, 0x2059, 0x205a, 0x205b, 0x205c, 0x205d, 0x205e, 0x205f, 0x2060, 0x2061, 0x2062, 0x2063,
    0x2064, 0x2065, 0x2066, 0x2067, 0x2068, 0x2069, 0x206a, 0x206b, 0x206c, 0x206d, 0x206e, 0x206f,
    0x2070, 0x2071, 0x2072, 0x2073, 0x2074, 0x2075, 0x2076, 0x2077, 0x2078, 0x2079, 0x207a, 0x207b,
    0x207c, 0x207d, 0x207e, 0x207f, 0x2080, 0x2081, 0x2082, 0x2083, 0x2084, 0x2085, 0x2086, 0x2087,
    0x2088, 0x2089, 0x208a, 0x208b, 0x208c, 0x208d, 0x208e, 0x208f, 0x2090, 0x2091, 0x2092, 0x2093,
    0x2094, 0x2095, 0x2096, 0x2097, 0x2098, 0x2099, 0x209a, 0x209b, 0x209c, 0x209d, 0x209e, 0x209f,
    0x20a0, 0x20a1, 0x20a2, 0x20a3, 0x20a4, 0x20a5, 0x20a6, 0x20a7, 0x20a8, 0x20a9, 0x20aa, 0x20ab,
    0x20ac, 0x20ad, 0x20ae, 0x20af, 0x20b0, 0x20b1, 0x20b2, 0x20b3, 0x20b4, 0x20b5, 0x20b6, 0x20b7,
    0x20b8, 0x20b9, 0x20ba, 0x20bb, 0x20bc, 0x20bd, 0x20be, 0x20bf, 0x20c0, 0x20c1, 0x20c2, 0x20c3,
    0x20c4, 0x20c5, 0x20c6, 0x20c7, 0x20c8, 0x20c9, 0x20ca, 0x20cb, 0x20cc, 0x20cd, 0x20ce, 0x20cf,
    0x20d0, 0x20d1, 0x20d2, 0x20d3, 0x20d4, 0x20d5, 0x20d6, 0x20d7, 0x20d8, 0x20d9, 0x20da, 0x20db,
    0x20dc, 0x20dd, 0x20de, 0x20df, 0x20e0, 0x20e1, 0x20e2, 0x20e3, 0x20e4, 0x20e5, 0x20e6, 0x20e7,
    0x20e8, 0x20e9, 0x20ea, 0x20eb, 0x20ec, 0x20ed, 0x20ee, 0x20ef, 0x20f0, 0x20f1, 0x20f2, 0x20f3,
    0x20f4, 0x20f5, 0x20f6, 0x20f7, 0x20f8, 0x20f9, 0x20fa, 0x20fb, 0x20fc, 0x20fd, 0x20fe, 0x20ff,
    0x2100, 0x2101, 0x2102, 0x2103, 0x2104, 0x2105, 0x2106, 0x2107, 0x2108, 0x2109, 0x210a, 0x210b,
    0x210c, 0x210d, 0x210e, 0x210f, 0x2110, 0x2111, 0x2112, 0x2113, 0x2114, 0x2115, 0x2116, 0x2117,
    0x2118, 0x2119, 0x211a, 0x211b, 0x211c, 0x211d, 0x211e, 0x211f, 0x2120, 0x2121, 0x2122, 0x2123,
    0x2124, 0x2125, 0x2126, 0x2127, 0x2128, 0x2129, 0x212a, 0x212b, 0x212c, 0x212d, 0x212e, 0x212f,
    0x2130, 0x2131, 0x2132, 0x2133, 0x2134, 0x2135, 0x2136, 0x2137, 0x2138, 0x2139, 0x213a, 0x213b,
    0x213c, 0x213d, 0x213e, 0x213f, 0x2140, 0x2141, 0x2142, 0x2143, 0x2144, 0x2145, 0x2146, 0x2147,
    0x2148, 0x2149, 0x214a, 0x214b, 0x214c, 0x214d, 0x2132, 0x214f, 0x2150, 0x2151, 0x2152, 0x2153,
    0x2154, 0x2155, 0x2156, 0x2157, 0x2158, 0x2159, 0x215a, 0x215b, 0x215c, 0x215d, 0x215e, 0x215f,
    0x2160, 0x2161, 0x2162, 0x2163, 0x2164, 0x2165, 0x2166, 0x2167, 0x2168, 0x2169, 0x216a, 0x216b,
    0x216c, 0x216d, 0x216e, 0x216f, 0x2160, 0x2161, 0x2162, 0x2163, 0x2164, 0x2165, 0x2166, 0x2167,
    0x2168, 0x2169, 0x216a, 0x216b, 0x216c, 0x216d, 0x216e, 0x216f, 0x2180, 0x2181, 0x2182, 0x2183,
    0x2183, 0xffff, 0x034b, 0x24b6, 0x24b7, 0x24b8, 0x24b9, 0x24ba, 0x24bb, 0x24bc, 0x24bd, 0x24be,
    0x24bf, 0x24c0, 0x24c1, 0x24c2, 0x24c3, 0x24c4, 0x24c5, 0x24c6, 0x24c7, 0x24c8, 0x24c9, 0x24ca,
    0x24cb, 0x24cc, 0x24cd, 0x24ce, 0x24cf, 0xffff, 0x0746, 0x2c00, 0x2c01, 0x2c02, 0x2c03, 0x2c04,
    0x2c05, 0x2c06, 0x2c07, 0x2c08, 0x2c09, 0x2c0a, 0x2c0b, 0x2c0c, 0x2c0d, 0x2c0e, 0x2c0f, 0x2c10,
    0x2c11, 0x2c12, 0x2c13, 0x2c14, 0x2c15, 0x2c16, 0x2c17, 0x2c18, 0x2c19, 0x2c1a, 0x2c1b, 0x2c1c,
    0x2c1d, 0x2c1e, 0x2c1f, 0x2c20, 0x2c21, 0x2c22, 0x2c23, 0x2c24, 0x2c25, 0x2c26, 0x2c27, 0x2c28,
    0x2c29, 0x2c2a, 0x2c2b, 0x2c2c, 0x2c2d, 0x2c2e, 0x2c5f, 0x2c60, 0x2c60, 0x2c62, 0x2c63, 0x2c64,
    0x2c65, 0x2c66, 0x2c67, 0x2c67, 0x2c69, 0x2c69, 0x2c6b, 0x2c6b, 0x2c6d, 0x2c6e, 0x2c6f, 0x2c70,
    0x2c71, 0x2c72, 0x2c73, 0x2c74, 0x2c75, 0x2c75, 0x2c77, 0x2c78, 0x2c79, 0x2c7a, 0x2c7b, 0x2c7c,
    0x2c7d, 0x2c7e, 0x2c7f, 0x2c80, 0x2c80, 0x2c82, 0x2c82, 0x2c84, 0x2c84, 0x2c86, 0x2c86, 0x2c88,
    0x2c88, 0x2c8a, 0x2c8a, 0x2c8c, 0x2c8c, 0x2c8e, 0x2c8e, 0x2c90, 0x2c90, 0x2c92, 0x2c92, 0x2c94,
    0x2c94, 0x2c96, 0x2c96, 0x2c98, 0x2c98, 0x2c9a, 0x2c9a, 0x2c9c, 0x2c9c, 0x2c9e, 0x2c9e, 0x2ca0,
    0x2ca0, 0x2ca2, 0x2ca2, 0x2ca4, 0x2ca4, 0x2ca6, 0x2ca6, 0x2ca8, 0x2ca8, 0x2caa, 0x2caa, 0x2cac,
    0x2cac, 0x2cae, 0x2cae, 0x2cb0, 0x2cb0, 0x2cb2, 0x2cb2, 0x2cb4, 0x2cb4, 0x2cb6, 0x2cb6, 0x2cb8,
    0x2cb8, 0x2cba, 0x2cba, 0x2cbc, 0x2cbc, 0x2cbe, 0x2cbe, 0x2cc0, 0x2cc0, 0x2cc2, 0x2cc2, 0x2cc4,
    0x2cc4, 0x2cc6, 0x2cc6, 0x2cc8, 0x2cc8, 0x2cca, 0x2cca, 0x2ccc, 0x2ccc, 0x2cce, 0x2cce, 0x2cd0,
    0x2cd0, 0x2cd2, 0x2cd2, 0x2cd4, 0x2cd4, 0x2cd6, 0x2cd6, 0x2cd8, 0x2cd8, 0x2cda, 0x2cda, 0x2cdc,
    0x2cdc, 0x2cde, 0x2cde, 0x2ce0, 0x2ce0, 0x2ce2, 0x2ce2, 0x2ce4, 0x2ce5, 0x2ce6, 0x2ce7, 0x2ce8,
    0x2ce9, 0x2cea, 0x2ceb, 0x2cec, 0x2ced, 0x2cee, 0x2cef, 0x2cf0, 0x2cf1, 0x2cf2, 0x2cf3, 0x2cf4,
    0x2cf5, 0x2cf6, 0x2cf7, 0x2cf8, 0x2cf9, 0x2cfa, 0x2cfb, 0x2cfc, 0x2cfd, 0x2cfe, 0x2cff, 0x10a0,
    0x10a1, 0x10a2, 0x10a3, 0x10a4, 0x10a5, 0x10a6, 0x10a7, 0x10a8, 0x10a9, 0x10aa, 0x10ab, 0x10ac,
    0x10ad, 0x10ae, 0x10af, 0x10b0, 0x10b1, 0x10b2, 0x10b3, 0x10b4, 0x10b5, 0x10b6, 0x10b7, 0x10b8,
    0x10b9, 0x10ba, 0x10bb, 0x10bc, 0x10bd, 0x10be, 0x10bf, 0x10c0, 0x10c1, 0x10c2, 0x10c3, 0x10c4,
    0x10c5, 0xffff, 0xd21b, 0xff21, 0xff22, 0xff23, 0xff24, 0xff25, 0xff26, 0xff27, 0xff28, 0xff29,
    0xff2a, 0xff2b, 0xff2c, 0xff2d, 0xff2e, 0xff2f, 0xff30, 0xff31, 0xff32, 0xff33, 0xff34, 0xff35,
    0xff36, 0xff37, 0xff38, 0xff39, 0xff3a, 0xff5b, 0xff5c, 0xff5d, 0xff5e, 0xff5f, 0xff60, 0xff61,
    0xff62, 0xff63, 0xff64, 0xff65, 0xff66, 0xff67, 0xff68, 0xff69, 0xff6a, 0xff6b, 0xff6c, 0xff6d,
    0xff6e, 0xff6f, 0xff70, 0xff71, 0xff72, 0xff73, 0xff74, 0xff75, 0xff76, 0xff77, 0xff78, 0xff79,
    0xff7a, 0xff7b, 0xff7c, 0xff7d, 0xff7e, 0xff7f, 0xff80, 0xff81, 0xff82, 0xff83, 0xff84, 0xff85,
    0xff86, 0xff87, 0xff88, 0xff89, 0xff8a, 0xff8b, 0xff8c, 0xff8d, 0xff8e, 0xff8f, 0xff90, 0xff91,
    0xff92, 0xff93, 0xff94, 0xff95, 0xff96, 0xff97, 0xff98, 0xff99, 0xff9a, 0xff9b, 0xff9c, 0xff9d,
    0xff9e, 0xff9f, 0xffa0, 0xffa1, 0xffa2, 0xffa3, 0xffa4, 0xffa5, 0xffa6, 0xffa7, 0xffa8, 0xffa9,
    0xffaa, 0xffab, 0xffac, 0xffad, 0xffae, 0xffaf, 0xffb0, 0xffb1, 0xffb2, 0xffb3, 0xffb4, 0xffb5,
    0xffb6, 0xffb7, 0xffb8, 0xffb9, 0xffba, 0xffbb, 0xffbc, 0xffbd, 0xffbe, 0xffbf, 0xffc0, 0xffc1,
    0xffc2, 0xffc3, 0xffc4, 0xffc5, 0xffc6, 0xffc7, 0xffc8, 0xffc9, 0xffca, 0xffcb, 0xffcc, 0xffcd,
    0xffce, 0xffcf, 0xffd0, 0xffd1, 0xffd2, 0xffd3, 0xffd4, 0xffd5, 0xffd6, 0xffd7, 0xffd8, 0xffd9,
    0xffda, 0xffdb, 0xffdc, 0xffdd, 0xffde, 0xffdf, 0xffe0, 0xffe1, 0xffe2, 0xffe3, 0xffe4, 0xffe5,
    0xffe6, 0xffe7, 0xffe8, 0xffe9, 0xffea, 0xffeb, 0xffec, 0xffed, 0xffee, 0xffef, 0xfff0, 0xfff1,
    0xfff2, 0xfff3, 0xfff4, 0xfff5, 0xfff6, 0xfff7, 0xfff8, 0xfff9, 0xfffa, 0xfffb, 0xfffc, 0xfffd,
    0xfffe, 0xffff,
};

typedef struct {
    char* path; /* relative to the source; "" for the root */
    uint16_t* name;
    unsigned nameUnits;
    uint16_t nameHash;
    int directory;
    uint64_t size; /* a file's bytes; a directory's entries' bytes */
    uint32_t firstCluster;
    uint32_t clusters;
    size_t firstChild;
    size_t childCount;
    struct timespec modified;
    struct timespec accessed;
} Node;

typedef struct {
    Node* nodes;
    size_t count;
    size_t capacity;
    size_t* order; /* node indices in cluster order: directories, then files */
    size_t orderCount;
    uint16_t* upcase; /* 65536 units */
    uint16_t label[11];
    unsigned labelUnits;
    uint32_t clusterSize;
    unsigned clusterShift; /* sectors per cluster, as a shift */
    uint32_t clusterCount;
    uint32_t used; /* clusters allocated, from cluster 2 */
    uint32_t heapOffset; /* sectors */
    uint32_t bitmapClusters;
    uint32_t upcaseClusters;
    uint64_t fileBytes;
    uint64_t imageSize;
    uint32_t serial;
    char* error;
    size_t errorSize;
    int failure; /* set by a failure that isn't the source's: -2, not -1 */
} Layout;

static void fail(Layout* l, int local, const char* format, ...)
{
    if (local)
        l->failure = 1;
    if (!l->error || !l->errorSize)
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(l->error, l->errorSize, format, args);
    va_end(args);
}

static void put16(unsigned char* p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char* p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static void put64(unsigned char* p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static uint64_t roundUp(uint64_t value, uint64_t unit)
{
    return (value + unit - 1) / unit * unit;
}

/* UTF-8 to UTF-16. 0; -1 not UTF-8 (overlong, surrogates, past U+10FFFF);
 * -2 longer than max units (unless truncate: then what fits, whole
 * characters only). */
static int toUtf16(const char* text, uint16_t* out, unsigned max, unsigned* units, int truncate)
{
    const unsigned char* s = (const unsigned char*)text;
    unsigned n = 0;
    while (*s) {
        uint32_t c = *s;
        unsigned extra = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 4;
        if (extra == 4)
            return -1;
        if (extra)
            c &= 0x3f >> extra;
        for (unsigned i = 1; i <= extra; i++) {
            if ((s[i] & 0xc0) != 0x80)
                return -1;
            c = c << 6 | (s[i] & 0x3f);
        }
        static const uint32_t least[4] = { 0, 0x80, 0x800, 0x10000 };
        if (c < least[extra] || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
            return -1;
        unsigned need = c >= 0x10000 ? 2 : 1;
        if (n + need > max) {
            if (!truncate)
                return -2;
            break;
        }
        if (need == 2) {
            c -= 0x10000;
            out[n++] = (uint16_t)(0xd800 | c >> 10);
            out[n++] = (uint16_t)(0xdc00 | (c & 0x3ff));
        } else
            out[n++] = (uint16_t)c;
        s += 1 + extra;
    }
    *units = n;
    return 0;
}

/* What a file name can't hold (section 7.7.3), or 0. */
static int forbiddenUnit(uint16_t u)
{
    return u < 0x20 || u == '"' || u == '*' || u == '/' || u == ':' || u == '<' || u == '>' || u == '?' || u == '\\' || u == '|';
}

static uint16_t* expandUpcase(void)
{
    uint16_t* table = malloc(65536 * sizeof *table);
    if (!table)
        return NULL;
    for (unsigned i = 0; i < 65536; i++)
        table[i] = (uint16_t)i;
    unsigned at = 0;
    size_t count = sizeof upcaseCompressed / sizeof upcaseCompressed[0];
    for (size_t i = 0; i < count && at < 65536; i++) {
        if (upcaseCompressed[i] == 0xffff && i + 1 < count)
            at += upcaseCompressed[++i];
        else
            table[at++] = upcaseCompressed[i];
    }
    return table;
}

static uint16_t nameHash(const uint16_t* upcase, const uint16_t* name, unsigned units)
{
    uint16_t hash = 0;
    for (unsigned i = 0; i < units; i++) {
        uint16_t u = upcase[name[i]];
        hash = (uint16_t)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (u & 0xff));
        hash = (uint16_t)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (u >> 8));
    }
    return hash;
}

static uint32_t checksum32(uint32_t sum, const unsigned char* bytes, size_t length, int bootSector)
{
    for (size_t i = 0; i < length; i++) {
        if (bootSector && (i == 106 || i == 107 || i == 112))
            continue; /* VolumeFlags and PercentInUse */
        sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + bytes[i];
    }
    return sum;
}

static void stampOf(const struct stat* st, struct timespec* modified, struct timespec* accessed)
{
#ifdef __APPLE__
    *modified = st->st_mtimespec;
    *accessed = st->st_atimespec;
#else
    *modified = st->st_mtim;
    *accessed = st->st_atim;
#endif
}

/* An exFAT timestamp (UTC) and its 10 ms increment. Clamped to the years
 * the format holds, 1980..2107. */
static uint32_t timestamp(struct timespec t, uint8_t* increment)
{
    struct tm tm;
    time_t seconds = t.tv_sec;
    if (!gmtime_r(&seconds, &tm) || tm.tm_year < 80) {
        if (increment)
            *increment = 0;
        return 1u << 21 | 1u << 16; /* 1980-01-01 00:00:00 */
    }
    if (tm.tm_year > 207) {
        if (increment)
            *increment = 199;
        return 127u << 25 | 12u << 21 | 31u << 16 | 23u << 11 | 59u << 5 | 29u;
    }
    unsigned second = tm.tm_sec > 59 ? 59 : (unsigned)tm.tm_sec;
    if (increment)
        *increment = (uint8_t)((second & 1) * 100 + (unsigned)(t.tv_nsec / 10000000));
    return (uint32_t)(tm.tm_year - 80) << 25 | (uint32_t)(tm.tm_mon + 1) << 21 | (uint32_t)tm.tm_mday << 16
        | (uint32_t)tm.tm_hour << 11 | (uint32_t)tm.tm_min << 5 | second / 2;
}

static unsigned entriesFor(const Node* node)
{
    return 2 + (node->nameUnits + 14) / 15; /* File, Stream Extension, File Names */
}

static void freeLayout(Layout* l)
{
    for (size_t i = 0; i < l->count; i++) {
        free(l->nodes[i].path);
        free(l->nodes[i].name);
    }
    free(l->nodes);
    free(l->order);
    free(l->upcase);
}

static Node* addNode(Layout* l)
{
    if (l->count == l->capacity) {
        size_t capacity = l->capacity ? l->capacity * 2 : 256;
        Node* nodes = realloc(l->nodes, capacity * sizeof *nodes);
        if (!nodes)
            return NULL;
        l->nodes = nodes;
        l->capacity = capacity;
    }
    Node* node = &l->nodes[l->count++];
    memset(node, 0, sizeof *node);
    return node;
}

static int compareStrings(const void* a, const void* b)
{
    return strcmp(*(char* const*)a, *(char* const*)b);
}

/* A name up-cased, for finding names that differ only in case (exFAT
 * can't hold both). */
typedef struct {
    uint16_t* name;
    unsigned units;
    size_t index;
} Upcased;

static int compareUpcased(const void* a, const void* b)
{
    const Upcased* x = a;
    const Upcased* y = b;
    unsigned n = x->units < y->units ? x->units : y->units;
    for (unsigned i = 0; i < n; i++) {
        if (x->name[i] != y->name[i])
            return x->name[i] < y->name[i] ? -1 : 1;
    }
    return (int)x->units - (int)y->units;
}

static char* joinPath(const char* a, const char* b)
{
    size_t la = strlen(a), lb = strlen(b);
    char* path = malloc(la + lb + 2);
    if (!path)
        return NULL;
    memcpy(path, a, la);
    size_t at = la;
    if (la && lb)
        path[at++] = '/';
    memcpy(path + at, b, lb + 1);
    return path;
}

/* Reads one folder's names into new child nodes, sorted by name. */
static int listFolder(Layout* l, const char* src, size_t index)
{
    char* folder = joinPath(src, l->nodes[index].path);
    if (!folder) {
        fail(l, 1, "out of memory");
        return -1;
    }
    DIR* dir = opendir(folder);
    if (!dir) {
        fail(l, 1, "%s couldn't be read (errno %d)", folder, errno);
        free(folder);
        return -1;
    }
    char** names = NULL;
    size_t count = 0, capacity = 0;
    int result = -1;
    struct dirent* entry;
    errno = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 64;
            char** grown = realloc(names, capacity * sizeof *grown);
            if (!grown) {
                fail(l, 1, "out of memory");
                goto done;
            }
            names = grown;
        }
        if (!(names[count] = strdup(entry->d_name))) {
            fail(l, 1, "out of memory");
            goto done;
        }
        count++;
        errno = 0;
    }
    if (errno) {
        fail(l, 1, "%s couldn't be read (errno %d)", folder, errno);
        goto done;
    }
    if (count)
        qsort(names, count, sizeof *names, compareStrings);

    l->nodes[index].firstChild = l->count;
    l->nodes[index].childCount = count;
    uint64_t entries = index ? 0 : 3; /* the root's volume label, bitmap and up-case entries */
    for (size_t i = 0; i < count; i++) {
        const char* parentPath = l->nodes[index].path;
        char* path = joinPath(parentPath, names[i]);
        char* full = path ? joinPath(src, path) : NULL;
        Node* node = full ? addNode(l) : NULL;
        if (!node) {
            free(path);
            free(full);
            fail(l, 1, "out of memory");
            goto done;
        }
        node->path = path;
        struct stat st;
        if (lstat(full, &st)) {
            fail(l, 1, "%s couldn't be read (errno %d)", path, errno);
            free(full);
            goto done;
        }
        free(full);
        if (S_ISLNK(st.st_mode)) {
            fail(l, 0, "%s is a link", path);
            goto done;
        }
        if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
            fail(l, 0, "%s is a special file", path);
            goto done;
        }
        node->directory = S_ISDIR(st.st_mode);
        node->size = node->directory ? 0 : (uint64_t)st.st_size;
        stampOf(&st, &node->modified, &node->accessed);
        uint16_t name[255];
        unsigned units = 0;
        int r = toUtf16(names[i], name, 255, &units, 0);
        if (r) {
            fail(l, 0, r == -1 ? "%s: its name isn't UTF-8" : "%s: its name is longer than exFAT allows (255)", path);
            goto done;
        }
        for (unsigned u = 0; u < units; u++) {
            if (forbiddenUnit(name[u])) {
                fail(l, 0, "%s: exFAT names can't hold control characters or \" * : < > ? \\ |", path);
                goto done;
            }
        }
        /* The specification allows these, but drivers with Windows' name
         * rules (macOS's: it lists such a file and can't open it) drop the
         * trailing dots and spaces when they look a name up. */
        if (name[units - 1] == '.' || name[units - 1] == ' ') {
            fail(l, 0, "%s: a name ending in a dot or a space can't be opened from exFAT", path);
            goto done;
        }
        if (!(node->name = malloc(units * sizeof *node->name))) {
            fail(l, 1, "out of memory");
            goto done;
        }
        memcpy(node->name, name, units * sizeof *name);
        node->nameUnits = units;
        node->nameHash = nameHash(l->upcase, name, units);
        entries += entriesFor(node);
    }
    entries *= 32;
    if (entries > MAX_DIRECTORY) {
        fail(l, 0, "%s holds too many files for one exFAT folder", index ? l->nodes[index].path : "the folder");
        goto done;
    }
    l->nodes[index].size = entries;

    /* Names that differ only in case. */
    if (count > 1) {
        Upcased* sorted = calloc(count, sizeof *sorted);
        if (!sorted) {
            fail(l, 1, "out of memory");
            goto done;
        }
        int clash = 0, memory = 0;
        for (size_t i = 0; i < count && !memory; i++) {
            const Node* node = &l->nodes[l->nodes[index].firstChild + i];
            sorted[i].index = l->nodes[index].firstChild + i;
            sorted[i].units = node->nameUnits;
            if (!(sorted[i].name = malloc(node->nameUnits * sizeof(uint16_t)))) {
                memory = 1;
                break;
            }
            for (unsigned u = 0; u < node->nameUnits; u++)
                sorted[i].name[u] = l->upcase[node->name[u]];
        }
        if (!memory) {
            qsort(sorted, count, sizeof *sorted, compareUpcased);
            for (size_t i = 1; i < count && !clash; i++) {
                if (!compareUpcased(&sorted[i - 1], &sorted[i])) {
                    fail(l, 0, "%s and %s differ only in case, which exFAT can't hold", l->nodes[sorted[i - 1].index].path, l->nodes[sorted[i].index].path);
                    clash = 1;
                }
            }
        } else
            fail(l, 1, "out of memory");
        for (size_t i = 0; i < count; i++)
            free(sorted[i].name);
        free(sorted);
        if (clash || memory)
            goto done;
    }
    result = 0;
done:
    for (size_t i = 0; i < count; i++)
        free(names[i]);
    free(names);
    closedir(dir);
    free(folder);
    return result;
}

/* The tree, breadth first: each folder's children are next to each other
 * in nodes, after the folder. */
static int listTree(Layout* l, const char* src)
{
    struct stat st;
    if (stat(src, &st)) {
        fail(l, errno != ENOENT && errno != ENOTDIR, "%s couldn't be read (errno %d)", src, errno);
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fail(l, 0, "%s isn't a folder", src);
        return -1;
    }
    Node* root = addNode(l);
    if (!root || !(root->path = strdup(""))) {
        fail(l, 1, "out of memory");
        return -1;
    }
    root->directory = 1;
    stampOf(&st, &root->modified, &root->accessed);
    for (size_t i = 0; i < l->count; i++) {
        if (l->nodes[i].directory && listFolder(l, src, i))
            return -1;
    }
    return 0;
}

/* Clusters for everything, the FAT and heap offsets and the image size.
 * Everything allocated is contiguous from cluster 2: bitmap, up-case
 * table, the folders (root first), then the files. */
static int layOut(Layout* l, uint32_t clusterSize, int automatic)
{
    for (;;) {
        l->clusterSize = clusterSize;
        l->clusterShift = 0;
        while ((SECTOR << l->clusterShift) < clusterSize)
            l->clusterShift++;
        uint64_t fixed = 0;
        l->fileBytes = 0;
        int tooMany = 0;
        for (size_t i = 0; i < l->count; i++) {
            Node* node = &l->nodes[i];
            uint64_t clusters = (node->size + clusterSize - 1) / clusterSize;
            if (node->directory && !clusters)
                clusters = 1;
            if (clusters > MAX_CLUSTERS)
                tooMany = 1;
            node->clusters = (uint32_t)clusters;
            fixed += clusters;
            if (!node->directory)
                l->fileBytes += node->size;
        }
        l->upcaseClusters = (uint32_t)((sizeof upcaseCompressed + clusterSize - 1) / clusterSize);
        fixed += l->upcaseClusters;
        uint64_t spare = l->fileBytes / 200;
        spare = spare < SPARE_MIN ? SPARE_MIN : spare > SPARE_MAX ? SPARE_MAX : spare;
        uint64_t spareClusters = (spare + clusterSize - 1) / clusterSize;
        uint64_t align = (clusterSize > HEAP_ALIGN ? clusterSize : HEAP_ALIGN) / SECTOR;

        /* The bitmap and the FAT grow with the cluster count, and the free
         * clusters round the image up to 1 MiB: settle on a count. */
        uint64_t count = fixed + spareClusters;
        for (int round = 0; round < 64 && count <= MAX_CLUSTERS; round++) {
            uint64_t bitmap = ((count + 7) / 8 + clusterSize - 1) / clusterSize;
            if (count < fixed + bitmap + spareClusters) {
                count = fixed + bitmap + spareClusters;
                continue;
            }
            uint64_t heap = roundUp(FAT_OFFSET + ((count + 2) * 4 + SECTOR - 1) / SECTOR, align);
            uint64_t size = heap * SECTOR + count * clusterSize;
            uint64_t extra = (roundUp(size, IMAGE_ALIGN) - size) / clusterSize;
            if (!extra)
                break;
            count += extra;
        }
        uint64_t bitmapClusters = ((count + 7) / 8 + clusterSize - 1) / clusterSize;
        uint64_t heap = roundUp(FAT_OFFSET + ((count + 2) * 4 + SECTOR - 1) / SECTOR, align);
        if (count > MAX_CLUSTERS || count < fixed + bitmapClusters || heap > 0xffffffffu)
            tooMany = 1;
        if (tooMany) {
            if (automatic && clusterSize < 32u * 1024 * 1024) {
                clusterSize *= 2;
                continue;
            }
            fail(l, 0, "the folder is too big for exFAT with %u KiB clusters", clusterSize / 1024);
            return -1;
        }
        l->bitmapClusters = (uint32_t)bitmapClusters;
        l->clusterCount = (uint32_t)count;
        l->heapOffset = (uint32_t)heap;
        l->imageSize = heap * SECTOR + count * clusterSize;
        break;
    }

    free(l->order);
    l->order = malloc((l->count ? l->count : 1) * sizeof *l->order);
    if (!l->order) {
        fail(l, 1, "out of memory");
        return -1;
    }
    l->orderCount = 0;
    for (int files = 0; files < 2; files++) {
        for (size_t i = 0; i < l->count; i++) {
            if (l->nodes[i].directory == !files)
                l->order[l->orderCount++] = i;
        }
    }
    uint32_t next = 2 + l->bitmapClusters + l->upcaseClusters;
    for (size_t k = 0; k < l->orderCount; k++) {
        Node* node = &l->nodes[l->order[k]];
        node->firstCluster = node->clusters ? next : 0;
        next += node->clusters;
    }
    l->used = next - 2;
    return 0;
}

static int prepare(Layout* l, const char* src, const PS5ExfatOptions* options)
{
    uint32_t clusterSize = options ? options->cluster_size : 0;
    if (clusterSize && (clusterSize < 4096 || clusterSize > 32u * 1024 * 1024 || (clusterSize & (clusterSize - 1)))) {
        fail(l, 0, "the cluster size %u isn't a power of two from 4 KiB to 32 MiB", clusterSize);
        return -1;
    }
    if (!(l->upcase = expandUpcase())) {
        fail(l, 1, "out of memory");
        return -1;
    }
    if (options && options->label) {
        if (toUtf16(options->label, l->label, 11, &l->labelUnits, 1)) {
            fail(l, 0, "the volume label isn't UTF-8");
            return -1;
        }
        for (unsigned i = 0; i < l->labelUnits; i++) {
            if (forbiddenUnit(l->label[i]))
                l->label[i] = '_';
        }
    }
    if (listTree(l, src))
        return -1;
    return layOut(l, clusterSize ? clusterSize : DEFAULT_CLUSTER, !clusterSize);
}

int64_t ps5exfat_measure(const char* src, const PS5ExfatOptions* options, char* error, size_t error_size)
{
    Layout l = { .error = error, .errorSize = error_size };
    if (!src) {
        fail(&l, 0, "no folder");
        return -1;
    }
    int64_t size = prepare(&l, src, options) ? -1 : (int64_t)l.imageSize;
    freeLayout(&l);
    return size;
}

/* The image, written front to back through one big buffer. */
typedef struct {
    int fd;
    unsigned char* buffer;
    size_t fill;
    uint64_t offset; /* bytes handed over, buffered or written */
} Output;

static int flushOutput(Output* out)
{
    size_t done = 0;
    while (done < out->fill) {
        ssize_t n = write(out->fd, out->buffer + done, out->fill - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    out->fill = 0;
    return 0;
}

static int emit(Output* out, const void* bytes, size_t length)
{
    const unsigned char* p = bytes;
    while (length) {
        size_t room = BUFFER - out->fill;
        size_t n = length < room ? length : room;
        memcpy(out->buffer + out->fill, p, n);
        out->fill += n;
        out->offset += n;
        p += n;
        length -= n;
        if (out->fill == BUFFER && flushOutput(out))
            return -1;
    }
    return 0;
}

static int emitZeros(Output* out, uint64_t length)
{
    while (length) {
        size_t room = BUFFER - out->fill;
        size_t n = length < room ? (size_t)length : room;
        memset(out->buffer + out->fill, 0, n);
        out->fill += n;
        out->offset += n;
        length -= n;
        if (out->fill == BUFFER && flushOutput(out))
            return -1;
    }
    return 0;
}

static int emit32(Output* out, uint32_t value)
{
    unsigned char b[4];
    put32(b, value);
    return emit(out, b, 4);
}

static uint64_t clusterOffset(const Layout* l, uint32_t cluster)
{
    return (uint64_t)l->heapOffset * SECTOR + (uint64_t)(cluster - 2) * l->clusterSize;
}

/* The main and backup boot regions, then zeros up to the FAT. */
static int writeBootRegion(const Layout* l, Output* out)
{
    unsigned char region[12 * SECTOR];
    memset(region, 0, sizeof region);
    unsigned char* b = region;
    b[0] = 0xeb;
    b[1] = 0x76;
    b[2] = 0x90;
    memcpy(b + 3, "EXFAT   ", 8);
    put64(b + 64, 0); /* PartitionOffset */
    put64(b + 72, l->imageSize / SECTOR); /* VolumeLength */
    put32(b + 80, FAT_OFFSET);
    put32(b + 84, l->heapOffset - FAT_OFFSET); /* FatLength, padded to the heap */
    put32(b + 88, l->heapOffset);
    put32(b + 92, l->clusterCount);
    put32(b + 96, l->nodes[0].firstCluster); /* FirstClusterOfRootDirectory */
    put32(b + 100, l->serial);
    put16(b + 104, 0x0100); /* FileSystemRevision 1.00 */
    put16(b + 106, 0); /* VolumeFlags: clean, FAT 0 */
    b[108] = 9; /* BytesPerSectorShift: 512 */
    b[109] = (unsigned char)l->clusterShift;
    b[110] = 1; /* NumberOfFats */
    b[111] = 0x80; /* DriveSelect */
    b[112] = (unsigned char)((uint64_t)l->used * 100 / l->clusterCount); /* PercentInUse */
    memset(b + 120, 0xf4, 390); /* BootCode: hlt, as formatters fill it */
    b[510] = 0x55;
    b[511] = 0xaa;
    for (int s = 1; s <= 8; s++) { /* the extended boot sectors' signature */
        region[s * SECTOR + SECTOR - 2] = 0x55;
        region[s * SECTOR + SECTOR - 1] = 0xaa;
    }
    /* Sectors 9 (OEM parameters) and 10 (reserved) stay zero. */
    uint32_t sum = checksum32(0, region, SECTOR, 1);
    sum = checksum32(sum, region + SECTOR, 10 * SECTOR, 0);
    for (unsigned i = 0; i < SECTOR; i += 4)
        put32(region + 11 * SECTOR + i, sum);
    if (emit(out, region, sizeof region) || emit(out, region, sizeof region))
        return -1;
    return emitZeros(out, (uint64_t)FAT_OFFSET * SECTOR - out->offset);
}

static int writeChain(Output* out, uint32_t first, uint32_t clusters)
{
    for (uint32_t i = 0; i < clusters; i++) {
        if (emit32(out, i + 1 < clusters ? first + i + 1 : 0xffffffffu))
            return -1;
    }
    return 0;
}

/* The FAT: a chain for every allocation, in cluster order, then free. */
static int writeFat(const Layout* l, Output* out)
{
    if (emit32(out, 0xfffffff8u) || emit32(out, 0xffffffffu))
        return -1;
    if (writeChain(out, 2, l->bitmapClusters) || writeChain(out, 2 + l->bitmapClusters, l->upcaseClusters))
        return -1;
    for (size_t k = 0; k < l->orderCount; k++) {
        const Node* node = &l->nodes[l->order[k]];
        if (writeChain(out, node->firstCluster, node->clusters))
            return -1;
    }
    return emitZeros(out, (uint64_t)l->heapOffset * SECTOR - out->offset);
}

static int writeBitmap(const Layout* l, Output* out)
{
    uint32_t used = l->used;
    unsigned char ff[4096];
    memset(ff, 0xff, sizeof ff);
    uint64_t full = used / 8;
    while (full) {
        size_t n = full < sizeof ff ? (size_t)full : sizeof ff;
        if (emit(out, ff, n))
            return -1;
        full -= n;
    }
    uint64_t written = used / 8;
    if (used % 8) {
        unsigned char last = (unsigned char)((1u << (used % 8)) - 1);
        if (emit(out, &last, 1))
            return -1;
        written++;
    }
    return emitZeros(out, (uint64_t)l->bitmapClusters * l->clusterSize - written);
}

static int writeUpcase(const Layout* l, Output* out)
{
    unsigned char bytes[sizeof upcaseCompressed];
    for (size_t i = 0; i < sizeof upcaseCompressed / 2; i++)
        put16(bytes + 2 * i, upcaseCompressed[i]);
    if (emit(out, bytes, sizeof bytes))
        return -1;
    return emitZeros(out, (uint64_t)l->upcaseClusters * l->clusterSize - sizeof bytes);
}

/* One node's entry set: File, Stream Extension, File Names. */
static int writeEntrySet(Output* out, const Node* node, uint32_t clusterSize)
{
    unsigned char set[32 * 19];
    unsigned entries = entriesFor(node);
    memset(set, 0, 32 * entries);
    unsigned char* file = set;
    file[0] = 0x85;
    file[1] = (unsigned char)(entries - 1); /* SecondaryCount */
    put16(file + 4, node->directory ? 0x10 : 0x20); /* Directory or Archive */
    uint8_t modified10ms = 0;
    uint32_t modified = timestamp(node->modified, &modified10ms);
    put32(file + 8, modified); /* Create: the source's modification time */
    put32(file + 12, modified);
    put32(file + 16, timestamp(node->accessed, NULL));
    file[20] = modified10ms;
    file[21] = modified10ms;
    file[22] = file[23] = file[24] = 0x80; /* UTC */

    /* A folder's length is its clusters; a file's, its bytes. */
    unsigned char* stream = set + 32;
    uint64_t length = node->directory ? (uint64_t)node->clusters * clusterSize : node->size;
    stream[0] = 0xc0;
    stream[1] = 0x01; /* AllocationPossible; the FAT chain is real, NoFatChain clear */
    stream[3] = (unsigned char)node->nameUnits;
    put16(stream + 4, node->nameHash);
    put64(stream + 8, length); /* ValidDataLength */
    put32(stream + 20, node->firstCluster);
    put64(stream + 24, length); /* DataLength */

    for (unsigned i = 0; i < node->nameUnits; i++) {
        unsigned char* name = set + 64 + 32 * (i / 15);
        name[0] = 0xc1;
        put16(name + 2 + 2 * (i % 15), node->name[i]);
    }
    uint16_t sum = 0;
    for (unsigned i = 0; i < 32 * entries; i++) {
        if (i == 2 || i == 3)
            continue;
        sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + set[i]);
    }
    put16(file + 2, sum);
    return emit(out, set, 32 * entries);
}

static int writeDirectory(const Layout* l, Output* out, const Node* dir)
{
    uint64_t start = out->offset;
    if (dir == &l->nodes[0]) {
        unsigned char e[96];
        memset(e, 0, sizeof e);
        e[0] = 0x83; /* Volume Label; CharacterCount 0 when there's none */
        e[1] = (unsigned char)l->labelUnits;
        for (unsigned i = 0; i < l->labelUnits; i++)
            put16(e + 2 + 2 * i, l->label[i]);
        e[32] = 0x81; /* Allocation Bitmap */
        put32(e + 32 + 20, 2);
        put64(e + 32 + 24, ((uint64_t)l->clusterCount + 7) / 8);
        e[64] = 0x82; /* Up-case Table */
        unsigned char bytes[sizeof upcaseCompressed];
        for (size_t i = 0; i < sizeof upcaseCompressed / 2; i++)
            put16(bytes + 2 * i, upcaseCompressed[i]);
        put32(e + 64 + 4, checksum32(0, bytes, sizeof bytes, 0));
        put32(e + 64 + 20, 2 + l->bitmapClusters);
        put64(e + 64 + 24, sizeof bytes);
        if (emit(out, e, sizeof e))
            return -1;
    }
    for (size_t i = 0; i < dir->childCount; i++) {
        if (writeEntrySet(out, &l->nodes[dir->firstChild + i], l->clusterSize))
            return -1;
    }
    return emitZeros(out, (uint64_t)dir->clusters * l->clusterSize - (out->offset - start));
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

typedef struct {
    ps5exfat_progress_fn function;
    void* data;
    double last;
} Progress;

static void report(Progress* p, uint64_t done, uint64_t total, const char* current, int always)
{
    if (!p->function)
        return;
    double t = now();
    if (!always && t - p->last < 0.1)
        return;
    p->last = t;
    p->function(done, total, current, p->data);
}

/* -2 failed, -3 cancelled. */
static int copyFile(Layout* l, Output* out, const char* src, const Node* node, uint64_t* done, Progress* progress, volatile int* cancel)
{
    char* full = joinPath(src, node->path);
    if (!full) {
        fail(l, 1, "out of memory");
        return -2;
    }
    int fd = open(full, O_RDONLY);
    free(full);
    if (fd < 0) {
        fail(l, 1, "%s couldn't be opened (errno %d)", node->path, errno);
        return -2;
    }
    uint64_t left = node->size;
    while (left) {
        if (cancel && *cancel) {
            close(fd);
            fail(l, 0, "cancelled");
            return -3;
        }
        size_t room = BUFFER - out->fill;
        size_t want = left < room ? (size_t)left : room;
        ssize_t n = read(fd, out->buffer + out->fill, want);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            fail(l, 1, n ? "%s couldn't be read (errno %d)" : "%s got shorter while it was copied", node->path, errno);
            close(fd);
            return -2;
        }
        out->fill += (size_t)n;
        out->offset += (uint64_t)n;
        left -= (uint64_t)n;
        *done += (uint64_t)n;
        if (out->fill == BUFFER && flushOutput(out)) {
            fail(l, 1, "the image couldn't be written (errno %d)", errno);
            close(fd);
            return -2;
        }
        report(progress, *done, l->fileBytes, node->path, 0);
    }
    close(fd);
    uint64_t tail = (uint64_t)node->clusters * l->clusterSize - node->size;
    if (emitZeros(out, tail)) {
        fail(l, 1, "the image couldn't be written (errno %d)", errno);
        return -2;
    }
    return 0;
}

/* A serial that follows from the tree and the time of the build. */
static uint32_t serialOf(const Layout* l)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < l->count; i++) {
        for (const char* p = l->nodes[i].path; *p; p++)
            h = (h ^ (unsigned char)*p) * 16777619u;
        h = (h ^ (uint32_t)l->nodes[i].size) * 16777619u;
        h = (h ^ (uint32_t)l->nodes[i].modified.tv_sec) * 16777619u;
    }
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return h ^ (uint32_t)t.tv_sec ^ (uint32_t)t.tv_nsec;
}

static int writeImage(Layout* l, Output* out, const char* src, ps5exfat_progress_fn progressFunction, void* data, volatile int* cancel)
{
    Progress progress = { progressFunction, data, 0 };
    if (writeBootRegion(l, out) || writeFat(l, out) || writeBitmap(l, out) || writeUpcase(l, out)) {
        fail(l, 1, "the image couldn't be written (errno %d)", errno);
        return -2;
    }
    for (size_t k = 0; k < l->orderCount; k++) {
        const Node* node = &l->nodes[l->order[k]];
        if (!node->directory)
            break;
        if (writeDirectory(l, out, node)) {
            fail(l, 1, "the image couldn't be written (errno %d)", errno);
            return -2;
        }
    }
    report(&progress, 0, l->fileBytes, "", 1);
    uint64_t done = 0;
    for (size_t k = 0; k < l->orderCount; k++) {
        const Node* node = &l->nodes[l->order[k]];
        if (node->directory)
            continue;
        if (out->offset != (node->clusters ? clusterOffset(l, node->firstCluster) : out->offset)) {
            fail(l, 1, "internal error: %s is out of place", node->path);
            return -2;
        }
        int r = copyFile(l, out, src, node, &done, &progress, cancel);
        if (r)
            return r;
    }
    if (flushOutput(out)) {
        fail(l, 1, "the image couldn't be written (errno %d)", errno);
        return -2;
    }
    /* The free clusters at the end stay sparse. */
    if (out->offset != clusterOffset(l, 2 + l->used) || ftruncate(out->fd, (off_t)l->imageSize) || fsync(out->fd)) {
        fail(l, 1, "the image couldn't be written (errno %d)", errno);
        return -2;
    }
    report(&progress, done, l->fileBytes, "", 1);
    return 0;
}

/* image's folder is src or a folder in it (the image would be in what it is
 * made of, and never match it). Unknown (a folder that can't be resolved): no. */
static int insideFolder(const char* image, const char* src)
{
    char* folder = strdup(image);
    if (!folder)
        return 0;
    char* slash = strrchr(folder, '/');
    if (!slash)
        strcpy(folder, ".");
    else if (slash == folder)
        folder[1] = 0;
    else
        *slash = 0;
    char* a = realpath(folder, NULL);
    char* b = realpath(src, NULL);
    free(folder);
    size_t n = b ? strlen(b) : 0;
    int inside = a && b && !strncmp(a, b, n) && (a[n] == 0 || a[n] == '/' || (n && b[n - 1] == '/'));
    free(a);
    free(b);
    return inside;
}

int ps5exfat_build(const char* src, const char* image, const PS5ExfatOptions* options,
    ps5exfat_progress_fn progress, void* data, volatile int* cancel, char* error, size_t error_size)
{
    Layout l = { .error = error, .errorSize = error_size };
    if (!src || !image || !*image) {
        fail(&l, 0, "no folder or image");
        return -1;
    }
    struct stat st;
    if (!lstat(image, &st)) {
        fail(&l, 0, "%s already exists", image);
        return -1;
    }
    if (insideFolder(image, src)) {
        fail(&l, 0, "the image can't go inside the folder it is made from");
        return -1;
    }
    if (prepare(&l, src, options)) {
        freeLayout(&l);
        return l.failure ? -2 : -1;
    }
    l.serial = serialOf(&l);

    int result = -2;
    char* part = malloc(strlen(image) + 6);
    char* folder = strdup(image);
    Output out = { .fd = -1 };
    if (!part || !folder || !(out.buffer = malloc(BUFFER))) {
        fail(&l, 1, "out of memory");
        goto done;
    }
    sprintf(part, "%s.part", image);
    char* slash = strrchr(folder, '/');
    if (!slash)
        strcpy(folder, ".");
    else if (slash == folder)
        folder[1] = 0;
    else
        *slash = 0;
    struct statvfs fs;
    uint64_t margin = options && options->free_margin ? options->free_margin : DEFAULT_MARGIN;
    if (statvfs(folder, &fs)) {
        fail(&l, 1, "%s couldn't be checked for space (errno %d)", folder, errno);
        goto done;
    }
    uint64_t space = (uint64_t)fs.f_bavail * (fs.f_frsize ? fs.f_frsize : fs.f_bsize);
    if (space < l.imageSize + margin) {
        fail(&l, 1, "not enough space: the image needs %llu MB and %llu MB more left free, %llu MB are free",
            (unsigned long long)(l.imageSize >> 20), (unsigned long long)(margin >> 20), (unsigned long long)(space >> 20));
        goto done;
    }
    /* never over a .part that is there (a paused download of that name) */
    out.fd = open(part, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (out.fd < 0 && errno == EEXIST) {
        fail(&l, 0, "%s is there already (a paused download, or an image that wasn't finished): delete it first", part);
        result = -1;
        goto done;
    }
    if (out.fd < 0) {
        fail(&l, 1, "%s couldn't be made (errno %d)", part, errno);
        goto done;
    }
    result = writeImage(&l, &out, src, progress, data, cancel);
    if (close(out.fd) && !result) {
        fail(&l, 1, "the image couldn't be written (errno %d)", errno);
        result = -2;
    }
    out.fd = -1;
    if (!result) {
        if (!lstat(image, &st)) {
            fail(&l, 0, "%s already exists", image);
            result = -1;
        } else if (rename(part, image)) {
            fail(&l, 1, "%s couldn't be renamed (errno %d)", part, errno);
            result = -2;
        }
    }
    if (result)
        unlink(part);
done:
    if (out.fd >= 0)
        close(out.fd);
    free(out.buffer);
    free(part);
    free(folder);
    freeLayout(&l);
    return result;
}

/* ---------------------------------------------------------------- reading an image back (ps5exfat_verify) */

/* Read independently of the writer's layout: the boot region, the FAT and
 * the directories as the specification describes them, so a mistake in
 * the writer isn't repeated here. */

typedef struct {
    int fd;
    uint64_t fileSize;
    uint32_t sectorSize, clusterSize, clusterCount, heapOffset; /* heapOffset in sectors */
    uint32_t* fat;
    uint32_t fatEntries;
    uint64_t done, total;
    int counting; /* the first pass: only the sizes, for the progress */
    ps5exfat_progress_fn progress;
    void* data;
    volatile int* cancel;
    unsigned char* buffer; /* BUFFER bytes: the image */
    unsigned char* other;  /* BUFFER bytes: the source */
    char* error;
    size_t errorSize;
    double lastReport;
} Check;

static int checkFail(Check* c, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    if (c->error && c->errorSize)
        vsnprintf(c->error, c->errorSize, format, args);
    va_end(args);
    return -1;
}

static uint16_t get16(const unsigned char* p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t get32(const unsigned char* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const unsigned char* p)
{
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

static int readAt(int fd, void* buffer, size_t length, uint64_t offset)
{
    size_t got = 0;
    while (got < length) {
        ssize_t n = pread(fd, (char*)buffer + got, length - got, (off_t)(offset + got));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    return 0;
}

static int validCluster(const Check* c, uint32_t cluster)
{
    return cluster >= 2 && cluster - 2 < c->clusterCount;
}

/* The clusters of a chain (or a contiguous run when noChain), in order:
 * calls step for each one. length: bytes the stream has (its clusters
 * must cover it, and a chain may not end early or loop). */
static int forEachCluster(Check* c, uint32_t first, uint64_t length, int noChain, int (*step)(Check*, uint64_t offset, size_t bytes, void* user),
    void* user)
{
    if (!length)
        return first ? checkFail(c, "an empty stream has a cluster") : 0;
    uint64_t clusters = (length + c->clusterSize - 1) / c->clusterSize;
    if (clusters > c->clusterCount)
        return checkFail(c, "a stream is longer than the volume");
    uint32_t cluster = first;
    uint64_t left = length;
    for (uint64_t i = 0; i < clusters; i++) {
        if (!validCluster(c, cluster))
            return checkFail(c, "a stream points outside the volume (cluster %u)", cluster);
        uint64_t offset = (uint64_t)c->heapOffset * c->sectorSize + (uint64_t)(cluster - 2) * c->clusterSize;
        size_t bytes = left < c->clusterSize ? (size_t)left : c->clusterSize;
        if (offset + bytes > c->fileSize)
            return checkFail(c, "a stream is past the image's end");
        int r = step(c, offset, bytes, user);
        if (r)
            return r;
        left -= bytes;
        if (i + 1 == clusters)
            break;
        if (noChain)
            cluster++;
        else {
            if (cluster >= c->fatEntries)
                return checkFail(c, "the FAT is too short");
            uint32_t next = c->fat[cluster];
            if (!validCluster(c, next))
                return checkFail(c, "a FAT chain ends early (cluster %u -> %#x)", cluster, next);
            cluster = next;
        }
    }
    if (!noChain && validCluster(c, cluster) && cluster < c->fatEntries && c->fat[cluster] < 0xfffffff8u)
        return checkFail(c, "a FAT chain goes on past its stream's end");
    return 0;
}

typedef struct {
    unsigned char* bytes;
    size_t length, size;
} Collected;

static int collectStep(Check* c, uint64_t offset, size_t bytes, void* user)
{
    Collected* d = user;
    if (d->length + bytes > d->size)
        return checkFail(c, "a directory is too big");
    if (readAt(c->fd, d->bytes + d->length, bytes, offset))
        return checkFail(c, "the image couldn't be read (errno %d)", errno);
    d->length += bytes;
    return 0;
}

/* The root directory's length: its chain followed to the end. */
static int64_t chainLength(Check* c, uint32_t first)
{
    uint64_t clusters = 0;
    uint32_t cluster = first;
    while (validCluster(c, cluster) && cluster < c->fatEntries) {
        if (++clusters > c->clusterCount)
            return checkFail(c, "the root directory's chain loops");
        uint32_t next = c->fat[cluster];
        if (next >= 0xfffffff8u)
            return (int64_t)(clusters * c->clusterSize);
        cluster = next;
    }
    return checkFail(c, "the root directory's chain is broken");
}

typedef struct {
    int fd;
    uint64_t sourceOffset;
} Compare;

static int compareStep(Check* c, uint64_t offset, size_t bytes, void* user)
{
    Compare* cmp = user;
    for (size_t at = 0; at < bytes;) {
        if (c->cancel && *c->cancel)
            return checkFail(c, "Cancelled");
        size_t n = bytes - at < BUFFER ? bytes - at : BUFFER;
        if (readAt(c->fd, c->buffer, n, offset + at))
            return checkFail(c, "the image couldn't be read (errno %d)", errno);
        if (readAt(cmp->fd, c->other, n, cmp->sourceOffset))
            return checkFail(c, "the source changed or couldn't be read (errno %d)", errno);
        if (memcmp(c->buffer, c->other, n))
            return -2;
        at += n;
        cmp->sourceOffset += n;
        c->done += n;
        double t = now();
        if (c->progress && t - c->lastReport >= 0.25) {
            c->lastReport = t;
            c->progress(c->done, c->total, "", c->data);
        }
    }
    return 0;
}

static int checkDirectory(Check* c, const char* src, const unsigned char* bytes, size_t length, int root, unsigned depth);
typedef struct Names Names;
static int checkEntries(Check* c, const char* src, const unsigned char* bytes, size_t length, int root, unsigned depth, Names* matched);

/* The names a directory of the image holds (each must be in the folder,
 * once: a count alone would pass one file listed twice and another left out). */
struct Names {
    char** names;
    unsigned count, capacity;
};

static void namesFree(Names* n)
{
    for (unsigned i = 0; i < n->count; i++)
        free(n->names[i]);
    free(n->names);
}

static int namesAdd(Names* n, const char* name)
{
    if (n->count == n->capacity) {
        unsigned capacity = n->capacity ? n->capacity * 2 : 64;
        char** grown = realloc(n->names, capacity * sizeof *grown);
        if (!grown)
            return -1;
        n->names = grown;
        n->capacity = capacity;
    }
    if (!(n->names[n->count] = strdup(name)))
        return -1;
    n->count++;
    return 0;
}

static int compareNames(const void* a, const void* b)
{
    return strcmp(*(char* const*)a, *(char* const*)b);
}

/* One entry set (file + stream + names) at e, n entries in all. */
static int checkEntrySet(Check* c, const char* src, const unsigned char* e, unsigned n, unsigned depth, Names* matched)
{
    uint16_t sum = 0;
    for (unsigned i = 0; i < 32 * n; i++) {
        if (i == 2 || i == 3)
            continue;
        sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + e[i]);
    }
    if (sum != get16(e + 2))
        return checkFail(c, "an entry set's checksum is wrong in %s", src);
    const unsigned char* stream = e + 32;
    if (stream[0] != 0xc0)
        return checkFail(c, "a file entry without its stream entry in %s", src);
    unsigned units = stream[3];
    if (!units || n < 2 + (units + 14) / 15)
        return checkFail(c, "a file entry with a bad name in %s", src);
    char name[256 * 3 + 1];
    size_t out = 0;
    for (unsigned i = 0; i < units; i++) {
        const unsigned char* part = e + 64 + 32 * (i / 15);
        if (part[0] != 0xc1)
            return checkFail(c, "a file name entry is missing in %s", src);
        uint32_t u = get16(part + 2 + 2 * (i % 15));
        if (u >= 0xd800 && u < 0xdc00 && i + 1 < units) {
            const unsigned char* nextPart = e + 64 + 32 * ((i + 1) / 15);
            uint32_t low = get16(nextPart + 2 + 2 * ((i + 1) % 15));
            if (low >= 0xdc00 && low < 0xe000) {
                u = 0x10000 + ((u - 0xd800) << 10) + (low - 0xdc00);
                i++;
            }
        }
        if (u < 0x80)
            name[out++] = (char)u;
        else if (u < 0x800) {
            name[out++] = (char)(0xc0 | u >> 6);
            name[out++] = (char)(0x80 | (u & 0x3f));
        } else if (u < 0x10000) {
            name[out++] = (char)(0xe0 | u >> 12);
            name[out++] = (char)(0x80 | ((u >> 6) & 0x3f));
            name[out++] = (char)(0x80 | (u & 0x3f));
        } else {
            name[out++] = (char)(0xf0 | u >> 18);
            name[out++] = (char)(0x80 | ((u >> 12) & 0x3f));
            name[out++] = (char)(0x80 | ((u >> 6) & 0x3f));
            name[out++] = (char)(0x80 | (u & 0x3f));
        }
    }
    name[out] = 0;
    int directory = (get16(e + 4) & 0x10) != 0;
    int noChain = (stream[1] & 0x02) != 0;
    uint64_t valid = get64(stream + 8), length = get64(stream + 24);
    uint32_t first = get32(stream + 20);
    char* path = joinPath(src, name);
    if (!path)
        return checkFail(c, "out of memory");
    struct stat st;
    int result = 0;
    if (lstat(path, &st))
        result = checkFail(c, "%s is in the image but not in the folder", path);
    else if (directory != S_ISDIR(st.st_mode))
        result = checkFail(c, "%s is a %s in the image", path, directory ? "folder" : "file");
    else if (directory) {
        if (length % c->clusterSize || length > MAX_DIRECTORY)
            result = checkFail(c, "%s: a folder's length is wrong in the image", path);
        else {
            Collected d = { malloc(length ? length : 1), 0, length };
            if (!d.bytes)
                result = checkFail(c, "out of memory");
            else {
                result = forEachCluster(c, first, length, noChain, collectStep, &d);
                if (!result)
                    result = checkDirectory(c, path, d.bytes, d.length, 0, depth + 1);
                free(d.bytes);
            }
        }
    } else if (valid != length || length != (uint64_t)st.st_size)
        result = checkFail(c, "%s: %llu bytes in the image, %llu in the folder", path, (unsigned long long)length,
            (unsigned long long)st.st_size);
    else if (c->counting)
        c->total += length;
    else if (length) {
        Compare cmp = { open(path, O_RDONLY), 0 };
        if (cmp.fd < 0)
            result = checkFail(c, "%s couldn't be read (errno %d)", path, errno);
        else {
            result = forEachCluster(c, first, length, noChain, compareStep, &cmp);
            if (result == -2)
                result = checkFail(c, "%s differs from the image's copy", path);
            close(cmp.fd);
        }
    } else if (first)
        result = checkFail(c, "%s: an empty file with a cluster", path);
    free(path);
    if (!result && namesAdd(matched, name))
        result = checkFail(c, "out of memory");
    return result;
}

static int checkDirectory(Check* c, const char* src, const unsigned char* bytes, size_t length, int root, unsigned depth)
{
    if (depth > 64)
        return checkFail(c, "folders nested too deep");
    Names matched = { NULL, 0, 0 };
    int result = checkEntries(c, src, bytes, length, root, depth, &matched);
    if (!result && matched.count > 1) {
        qsort(matched.names, matched.count, sizeof *matched.names, compareNames);
        for (unsigned i = 1; i < matched.count && !result; i++) {
            if (!strcmp(matched.names[i - 1], matched.names[i]))
                result = checkFail(c, "%s/%s is in the image twice", src, matched.names[i]);
        }
    }
    unsigned count = matched.count;
    namesFree(&matched);
    if (result)
        return result;
    /* everything in the folder is in the image (each image name is in the
     * folder, once, so the same count means the same names) */
    DIR* folder = opendir(src);
    if (!folder)
        return checkFail(c, "%s couldn't be read (errno %d)", src, errno);
    unsigned there = 0;
    struct dirent* entry;
    while ((entry = readdir(folder)))
        there += strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..");
    closedir(folder);
    if (there != count)
        return checkFail(c, "%s has %u entries, the image %u", src, there, count);
    return 0;
}

/* The entries of one directory of the image, each checked against the folder. */
static int checkEntries(Check* c, const char* src, const unsigned char* bytes, size_t length, int root, unsigned depth, Names* matched)
{
    int sawBitmap = 0, sawUpcase = 0;
    for (size_t at = 0; at + 32 <= length;) {
        const unsigned char* e = bytes + at;
        unsigned char type = e[0];
        if (!type)
            break; /* the end */
        if (!(type & 0x80)) {
            at += 32; /* unused */
            continue;
        }
        if (type == 0x81 || type == 0x82 || type == 0x83 || type == 0xa0) {
            if (!root)
                return checkFail(c, "a volume entry outside the root in %s", src);
            if (type == 0x81)
                sawBitmap = 1;
            if (type == 0x82) {
                sawUpcase = 1;
                if (get32(e + 4) != 0xe619d30du)
                    return checkFail(c, "the up-case table isn't the specification's (the PS5 refuses others)");
            }
            at += 32;
            continue;
        }
        if (type != 0x85)
            return checkFail(c, "an unexpected entry (%#x) in %s", type, src);
        unsigned n = 1u + e[1];
        if (n < 3 || n > 19 || at + 32 * n > length)
            return checkFail(c, "a file entry set is cut short in %s", src);
        int r = checkEntrySet(c, src, e, n, depth, matched);
        if (r)
            return r;
        at += 32 * n;
    }
    if (root && (!sawBitmap || !sawUpcase))
        return checkFail(c, "the root has no %s", sawBitmap ? "up-case table" : "allocation bitmap");
    return 0;
}

int ps5exfat_verify(const char* image, const char* src, ps5exfat_progress_fn progress, void* data, volatile int* cancel, char* error,
    size_t error_size)
{
    Check c = { .fd = -1, .progress = progress, .data = data, .cancel = cancel, .error = error, .errorSize = error_size };
    if (error && error_size)
        error[0] = 0;
    int result = -1;
    unsigned char region[24 * SECTOR];
    Collected root = { NULL, 0, 0 };
    c.fd = open(image, O_RDONLY);
    struct stat st;
    if (c.fd < 0 || fstat(c.fd, &st)) {
        checkFail(&c, "%s couldn't be opened (errno %d)", image, errno);
        goto done;
    }
    c.fileSize = (uint64_t)st.st_size;
    if (readAt(c.fd, region, sizeof region, 0)) {
        checkFail(&c, "the image is too short");
        goto done;
    }
    const unsigned char* b = region;
    if (memcmp(b + 3, "EXFAT   ", 8) || b[510] != 0x55 || b[511] != 0xaa) {
        checkFail(&c, "not an exFAT volume");
        goto done;
    }
    unsigned sectorShift = b[108], clusterShift = b[109];
    if (sectorShift != 9 || sectorShift + clusterShift > 25 || b[110] != 1) {
        checkFail(&c, "an unexpected geometry (sectors 2^%u, clusters 2^%u sectors, %u FATs)", sectorShift, clusterShift, b[110]);
        goto done;
    }
    /* the boot checksum, in both regions, and the backup the same */
    uint32_t sum = checksum32(0, region, SECTOR, 1);
    sum = checksum32(sum, region + SECTOR, 10 * SECTOR, 0);
    for (unsigned i = 0; i < SECTOR; i += 4) {
        if (get32(region + 11 * SECTOR + i) != sum) {
            checkFail(&c, "the boot region's checksum is wrong");
            goto done;
        }
    }
    if (memcmp(region, region + 12 * SECTOR, 12 * SECTOR)) {
        checkFail(&c, "the backup boot region differs");
        goto done;
    }
    c.sectorSize = 1u << sectorShift;
    c.clusterSize = c.sectorSize << clusterShift;
    uint32_t fatOffset = get32(b + 80), fatLength = get32(b + 84);
    c.heapOffset = get32(b + 88);
    c.clusterCount = get32(b + 92);
    uint64_t volume = get64(b + 72) * c.sectorSize;
    uint64_t heapEnd = (uint64_t)c.heapOffset * c.sectorSize + (uint64_t)c.clusterCount * c.clusterSize;
    if (volume > c.fileSize || heapEnd > volume || (uint64_t)fatLength * c.sectorSize < ((uint64_t)c.clusterCount + 2) * 4
        || (uint64_t)fatOffset + fatLength > c.heapOffset) {
        checkFail(&c, "the volume's sizes don't fit the image");
        goto done;
    }
    c.fatEntries = c.clusterCount + 2;
    if ((uint64_t)c.fatEntries * 4 > (512ull << 20) || !(c.fat = malloc((size_t)c.fatEntries * 4))) {
        checkFail(&c, "the FAT is too big to check");
        goto done;
    }
    if (readAt(c.fd, c.fat, (size_t)c.fatEntries * 4, (uint64_t)fatOffset * c.sectorSize)) {
        checkFail(&c, "the FAT couldn't be read");
        goto done;
    }
    for (uint32_t i = 0; i < c.fatEntries; i++)
        c.fat[i] = get32((unsigned char*)&c.fat[i]);
    c.buffer = malloc(BUFFER);
    c.other = malloc(BUFFER);
    if (!c.buffer || !c.other) {
        checkFail(&c, "out of memory");
        goto done;
    }
    int64_t rootLength = chainLength(&c, get32(b + 96));
    if (rootLength < 0)
        goto done;
    if (rootLength > MAX_DIRECTORY) {
        checkFail(&c, "the root directory is too big");
        goto done;
    }
    root.size = (size_t)rootLength;
    if (!(root.bytes = malloc(root.size))) {
        checkFail(&c, "out of memory");
        goto done;
    }
    if (forEachCluster(&c, get32(b + 96), (uint64_t)rootLength, 0, collectStep, &root))
        goto done;
    /* the sizes first (the progress's total), then the bytes */
    c.counting = 1;
    if (checkDirectory(&c, src, root.bytes, root.length, 1, 0))
        goto done;
    c.counting = 0;
    if (progress)
        progress(0, c.total, "", data);
    if (checkDirectory(&c, src, root.bytes, root.length, 1, 0))
        goto done;
    if (progress)
        progress(c.done, c.total, "", data);
    result = 0;
done:
    if (result && cancel && *cancel && error && error_size)
        snprintf(error, error_size, "Cancelled");
    if (c.fd >= 0)
        close(c.fd);
    free(c.fat);
    free(c.buffer);
    free(c.other);
    free(root.bytes);
    return result ? (cancel && *cancel ? -3 : -1) : 0;
}

/* ---------------------------------------------------------------- one file out of an image (ps5exfat_read_file) */

/* Any exFAT volume, also other builders' (512..4096-byte sectors, any
 * cluster size). A cluster whose FAT entry is 0 counts as followed by the
 * next one: some builders write contiguous files with NoFatChain clear and
 * leave the FAT zeroed.
 *
 * The image is a download, so it may be hostile: a directory is walked
 * once, front to back (its chain is never re-walked from the start), no
 * chain is followed for more steps than the volume has clusters, and a
 * lookup reads at most LOOKUP_MAX of a directory. */
#define LOOKUP_MAX (16u * 1024 * 1024)
#define WINDOW 65536 /* the image is read through two windows this big: the FAT's and the data's */

typedef struct {
    uint64_t at;
    uint32_t length; /* 0: empty */
    unsigned char bytes[WINDOW];
} Window;

typedef struct {
    int fd;
    uint64_t fileSize;
    uint32_t sectorSize, clusterSize, clusterCount, heapOffset, fatOffset, root;
    char* error;
    size_t errorSize;
    Window* fat;
    Window* data;
} Reader;

/* length bytes at offset, through w. -1 past the end or unreadable. */
static int windowRead(Reader* r, Window* w, void* out, size_t length, uint64_t offset)
{
    if (!w || length > WINDOW)
        return readAt(r->fd, out, length, offset);
    if (!w->length || offset < w->at || offset + length > w->at + w->length) {
        uint64_t at = offset - offset % 4096;
        if (offset + length > at + WINDOW)
            at = offset;
        uint64_t n = r->fileSize > at ? r->fileSize - at : 0;
        if (n > WINDOW)
            n = WINDOW;
        w->length = 0;
        if (n < length || offset + length > at + n || readAt(r->fd, w->bytes, (size_t)n, at))
            return -1;
        w->at = at;
        w->length = (uint32_t)n;
    }
    memcpy(out, w->bytes + (offset - w->at), length);
    return 0;
}

static int readerFail(Reader* r, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    if (r->error && r->errorSize)
        vsnprintf(r->error, r->errorSize, format, args);
    va_end(args);
    return -1;
}

/* The cluster after cluster in a stream; 0 at its end or on a bad entry. */
static uint32_t nextCluster(Reader* r, uint32_t cluster, int noChain)
{
    if (noChain)
        return cluster + 1;
    unsigned char entry[4];
    if (windowRead(r, r->fat, entry, 4, (uint64_t)r->fatOffset * r->sectorSize + (uint64_t)cluster * 4))
        return 0;
    uint32_t next = get32(entry);
    if (!next)
        return cluster + 1; /* a zeroed FAT: contiguous */
    return next;
}

static int readerCluster(const Reader* r, uint32_t cluster)
{
    return cluster >= 2 && cluster - 2 < r->clusterCount;
}

/* length bytes of a stream from its start (a file's: read once). -1 damaged. */
static int readStream(Reader* r, uint32_t first, int noChain, unsigned char* out, size_t length)
{
    uint32_t cluster = first;
    uint64_t in = 0;
    while (length) {
        if (!readerCluster(r, cluster))
            return readerFail(r, "a stream points outside the volume");
        uint64_t at = (uint64_t)r->heapOffset * r->sectorSize + (uint64_t)(cluster - 2) * r->clusterSize + in;
        size_t n = r->clusterSize - in < length ? (size_t)(r->clusterSize - in) : length;
        if (at + n > r->fileSize || readAt(r->fd, out, n, at))
            return readerFail(r, "a stream is past the image's end");
        out += n;
        length -= n;
        in = 0;
        if (length)
            cluster = nextCluster(r, cluster, noChain);
    }
    return 0;
}

/* Whether the UTF-16 name (units) is name (UTF-8), ASCII letters in either case. */
static int sameName(const unsigned char* set, unsigned units, const char* name, size_t length)
{
    uint16_t wanted[255];
    unsigned count = 0;
    char* copy = malloc(length + 1);
    if (!copy)
        return 0;
    memcpy(copy, name, length);
    copy[length] = 0;
    int r = toUtf16(copy, wanted, 255, &count, 0);
    free(copy);
    if (r || count != units)
        return 0;
    for (unsigned i = 0; i < units; i++) {
        uint16_t u = get16(set + 64 + 32 * (i / 15) + 2 + 2 * (i % 15)), w = wanted[i];
        if (u >= 'a' && u <= 'z')
            u -= 32;
        if (w >= 'a' && w <= 'z')
            w -= 32;
        if (u != w)
            return 0;
    }
    return 1;
}

/* A directory read front to back: the cluster holding byte `start` of it. */
typedef struct {
    uint32_t cluster;
    uint64_t start; /* the directory offset cluster begins at */
    uint64_t steps; /* clusters followed */
    int noChain;
} Walk;

/* Moves w forward to the cluster holding at (never back). -1 damaged. */
static int walkTo(Reader* r, Walk* w, uint64_t at)
{
    while (at >= w->start + r->clusterSize) {
        if (!readerCluster(r, w->cluster) || ++w->steps > r->clusterCount)
            return readerFail(r, "a folder's chain runs past the volume's clusters");
        w->cluster = nextCluster(r, w->cluster, w->noChain);
        w->start += r->clusterSize;
    }
    return 0;
}

/* length bytes of the directory from at (at at or after the last one asked). */
static int walkRead(Reader* r, Walk* w, uint64_t at, unsigned char* out, size_t length)
{
    if (walkTo(r, w, at))
        return -1;
    Walk ahead = *w; /* a set may run into the next clusters: w stays */
    while (length) {
        if (walkTo(r, &ahead, at) || !readerCluster(r, ahead.cluster))
            return readerFail(r, "a stream points outside the volume");
        uint64_t in = at - ahead.start;
        size_t n = r->clusterSize - in < length ? (size_t)(r->clusterSize - in) : length;
        uint64_t from = (uint64_t)r->heapOffset * r->sectorSize + (uint64_t)(ahead.cluster - 2) * r->clusterSize + in;
        if (from + n > r->fileSize || windowRead(r, r->data, out, n, from))
            return readerFail(r, "a stream is past the image's end");
        out += n;
        at += n;
        length -= n;
    }
    return 0;
}

/* In the directory (first, noChain, length; length 0: the root, read to
 * its end entry), the entry set called name: its stream's first cluster,
 * flags and length, whether a folder. 1 found, 0 not, -1 damaged. */
static int findEntry(Reader* r, uint32_t first, int noChain, uint64_t length, const char* name, size_t nameLength, uint32_t* outFirst,
    int* outNoChain, uint64_t* outLength, int* outDirectory)
{
    uint64_t limit = length ? length : MAX_DIRECTORY;
    const uint64_t volume = (uint64_t)r->clusterCount * r->clusterSize;
    if (limit > volume)
        limit = volume;
    if (limit > LOOKUP_MAX)
        limit = LOOKUP_MAX;
    Walk w = { .cluster = first, .noChain = noChain };
    unsigned char set[19 * 32];
    for (uint64_t at = 0; at + 32 <= limit; at += 32) {
        if (walkRead(r, &w, at, set, 32))
            return !length && at ? 0 : -1; /* the root's chain ended without an end entry */
        unsigned char type = set[0];
        if (!type)
            return 0;
        if (type != 0x85)
            continue;
        unsigned n = 1u + set[1];
        if (n < 3 || n > 19 || at + 32 * n > limit)
            return readerFail(r, "a file entry set is cut short");
        if (walkRead(r, &w, at, set, 32 * n))
            return -1;
        const unsigned char* stream = set + 32;
        unsigned units = stream[3];
        if (stream[0] != 0xc0 || !units || n < 2 + (units + 14) / 15)
            return readerFail(r, "a file entry without its stream entry");
        if (sameName(set, units, name, nameLength)) {
            *outDirectory = (get16(set + 4) & 0x10) != 0;
            *outNoChain = (stream[1] & 0x02) != 0;
            *outFirst = get32(stream + 20);
            *outLength = get64(stream + 24);
            return 1;
        }
        at += 32 * (n - 1);
    }
    return 0;
}

int ps5exfat_read_file(const char* image, const char* path, void* buffer, size_t size, uint64_t* length, char* error, size_t error_size)
{
    Reader r = { .fd = -1, .error = error, .errorSize = error_size };
    if (error && error_size)
        error[0] = 0;
    int result = -1;
    r.fat = calloc(1, sizeof *r.fat);
    r.data = calloc(1, sizeof *r.data);
    if (!r.fat || !r.data) {
        readerFail(&r, "Out of memory");
        goto done;
    }
    unsigned char b[SECTOR];
    r.fd = open(image, O_RDONLY);
    struct stat st;
    if (r.fd < 0 || fstat(r.fd, &st)) {
        readerFail(&r, "%s couldn't be opened (errno %d)", image, errno);
        goto done;
    }
    r.fileSize = (uint64_t)st.st_size;
    if (readAt(r.fd, b, sizeof b, 0) || memcmp(b + 3, "EXFAT   ", 8) || b[510] != 0x55 || b[511] != 0xaa) {
        readerFail(&r, "not an exFAT image");
        goto done;
    }
    unsigned sectorShift = b[108], clusterShift = b[109];
    if (sectorShift < 9 || sectorShift > 12 || sectorShift + clusterShift > 25) {
        readerFail(&r, "an exFAT volume with an unexpected geometry");
        goto done;
    }
    r.sectorSize = 1u << sectorShift;
    r.clusterSize = r.sectorSize << clusterShift;
    r.fatOffset = get32(b + 80);
    r.heapOffset = get32(b + 88);
    r.clusterCount = get32(b + 92);
    r.root = get32(b + 96);
    uint32_t dir = r.root;
    int noChain = 0, directory = 1;
    uint64_t dirLength = 0, fileLength = 0;
    for (const char* p = path; *p;) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        size_t n = strcspn(p, "/");
        if (!directory) {
            readerFail(&r, "%s isn't in the image", path);
            goto done;
        }
        uint32_t first;
        int found = findEntry(&r, dir, noChain, dirLength, p, n, &first, &noChain, &fileLength, &directory);
        if (found < 0) {
            result = -2;
            goto done;
        }
        if (!found) {
            readerFail(&r, "%s isn't in the image", path);
            goto done;
        }
        dir = first;
        dirLength = fileLength;
        if (directory && !dirLength) {
            readerFail(&r, "%s: an empty folder entry", path);
            goto done;
        }
        p += n;
    }
    if (directory) {
        readerFail(&r, "%s isn't a file in the image", path);
        goto done;
    }
    if (length)
        *length = fileLength;
    size_t want = fileLength < size ? (size_t)fileLength : size;
    if (want && readStream(&r, dir, noChain, buffer, want)) {
        result = -2;
        goto done;
    }
    result = 0;
done:
    if (r.fd >= 0)
        close(r.fd);
    free(r.fat);
    free(r.data);
    return result;
}
