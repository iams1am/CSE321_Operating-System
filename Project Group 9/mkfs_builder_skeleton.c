/* MiniVSFS mkfs_builder
 * Author: <Siam Abdullah>, ID <21301682> 
 * Group: 9
 * Course: CSE 321, Term Project (Summer 2025)
 * Notes: Builds a valid MiniVSFS image with root directory only.
 */



// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_minivsfs.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>  // for memcpy, memset
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>
#include <getopt.h> // for getopt_long
#include <sys/types.h>


#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12
//#define _POSIX_C_SOURCE 200809L



uint64_t g_random_seed = 0; // This should be replaced by seed value from the CLI.

// below contains some basic structures you need for your project
// you are free to create more structures as you require

#pragma pack(push, 1)
typedef struct {
    // CREATE YOUR SUPERBLOCK HERE
    // ADD ALL FIELDS AS PROVIDED BY THE SPECIFICATION
    uint32_t magic;              // 0x4D565346
    uint32_t version;            // 1
    uint32_t block_size;         // 4096
    uint64_t total_blocks;
    uint64_t inode_count;

    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;   // = 1
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;    // = 1
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;

    uint64_t root_inode;         // = 1 (inodes are 1-indexed)
    uint64_t mtime_epoch;        // build time (Unix epoch)
    uint32_t flags;              // 0

    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint32_t checksum;            // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    // CREATE YOUR INODE HERE
    // IF CREATED CORRECTLY, THE STATIC_ASSERT ERROR SHOULD BE GONE
    uint16_t mode;          // (0100000)8 file, (0040000)8 dir
    uint16_t links;         // link count
    uint32_t uid;           // 0
    uint32_t gid;           // 0
    uint64_t size_bytes;
    uint64_t atime;         // build time
    uint64_t mtime;         // build time
    uint64_t ctime;         // build time
    uint32_t direct[DIRECT_MAX]; // 12 direct, absolute block numbers

    uint32_t reserved_0;    // 0
    uint32_t reserved_1;    // 0
    uint32_t reserved_2;    // 0
    uint32_t proj_id;       // group id
    uint32_t uid16_gid16;   // 0
    uint64_t xattr_ptr;     // 0

    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint64_t inode_crc;   // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0

} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    // CREATE YOUR DIRECTORY ENTRY STRUCTURE HERE
    // IF CREATED CORRECTLY, THE STATIC_ASSERT ERROR SHOULD BE GONE
    uint32_t inode_no;    // 0 if free
    uint8_t  type;        // 1=file, 2=dir
    char     name[58];    // NUL-terminated if shorter
    uint8_t  checksum; // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");


// ==========================DO NOT CHANGE THIS PORTION=========================
// These functions are there for your help. You should refer to the specifications to see how you can use them.
// ====================================CRC32====================================
uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
// ====================================CRC32====================================

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    // zero crc area before computing
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c; // low 4 bytes carry the crc
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];   // covers ino(4) + type(1) + name(58)
    de->checksum = x;
}

// =====================
// CLI options structure
// =====================
struct options {
    char image[256];
    int size_kib;
    int inodes;
};

static void parse_args(int argc, char **argv, struct options *opts) {
    static struct option long_opts[] = {
        {"image", required_argument, 0, 'i'},
        {"size-kib", required_argument, 0, 's'},
        {"inodes", required_argument, 0, 'n'},
        {0,0,0,0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "i:s:n:", long_opts, NULL)) != -1) {
        switch (c) {
        case 'i': strncpy(opts->image, optarg, sizeof(opts->image)-1); break;
        case 's': opts->size_kib = atoi(optarg); break;
        case 'n': opts->inodes = atoi(optarg); break;
        default:
            fprintf(stderr, "Usage: %s --image FILE --size-kib N --inodes N\n", argv[0]);
            exit(1);
        }
    }

    if (opts->size_kib < 180 || opts->size_kib > 4096 || opts->size_kib % 4 != 0) {
        fprintf(stderr, "Invalid size-kib: must be [180..4096], multiple of 4\n");
        exit(1);
    }
    if (opts->inodes < 128 || opts->inodes > 512) {
        fprintf(stderr, "Invalid inode count: must be [128..512]\n");
        exit(1);
    }
    if (opts->image[0] == '\0') {
        fprintf(stderr, "No image file specified\n");
        exit(1);
    }
}
// ===================== Helper functions =====================
static void compute_layout(int size_kib, int inodes) {
    uint64_t total_blocks = (size_kib * 1024) / BS;

    // how many blocks for inode table?
    uint64_t inode_table_blocks = (inodes * INODE_SIZE + BS - 1) / BS; // ceil division

    uint64_t superblock_start = 0;
    uint64_t inode_bitmap_start = 1;
    uint64_t data_bitmap_start = 2;
    uint64_t inode_table_start = 3;
    uint64_t data_region_start = inode_table_start + inode_table_blocks;

    uint64_t data_region_blocks = total_blocks - data_region_start;

    printf("Total blocks: %" PRIu64 "\n", total_blocks);
    printf("Inode count:        %d\n", inodes);
    printf("Inode table blocks: %lu\n", inode_table_blocks);
    printf("Superblock start:   %lu\n", superblock_start);
    printf("Inode bitmap start: %lu\n", inode_bitmap_start);
    printf("Data bitmap start:  %lu\n", data_bitmap_start);
    printf("Inode table start:  %lu\n", inode_table_start);
    printf("Data region start:  %lu\n", data_region_start);
    printf("Data region blocks: %lu\n", data_region_blocks);
}
// ===================== Helper functions =====================
static inline void set_bit(uint8_t *bm, uint64_t index) {
    bm[index >> 3] |= (uint8_t)(1u << (index & 7));
}

static void write_block(FILE *f, uint64_t blkno, const void *data) {
    // For this project image size (≤ 4 MiB), plain fseek is safe.
    long byte_off = (long)(blkno * (uint64_t)BS);
    if (fseek(f, byte_off, SEEK_SET) != 0) {
        perror("fseek");
        exit(1);
    }
    if (fwrite(data, 1, BS, f) != BS) {
        perror("fwrite block");
        exit(1);
    }
}


static void build_fs_image(const char *image_path, int size_kib, int inodes) {
    // ---- 1) Compute layout ----
    uint64_t total_blocks = (size_kib * 1024ULL) / BS;
    uint64_t inode_table_blocks = (inodes * INODE_SIZE + BS - 1) / BS;

    uint64_t superblock_start   = 0;
    uint64_t inode_bitmap_start = 1;
    uint64_t data_bitmap_start  = 2;
    uint64_t inode_table_start  = 3;
    uint64_t data_region_start  = inode_table_start + inode_table_blocks;
    uint64_t data_region_blocks = total_blocks - data_region_start;

    if (data_region_blocks == 0) {
        fprintf(stderr, "Not enough blocks for data region. Increase size-kib or reduce inodes.\n");
        exit(1);
    }

    // ---- 2) Prepare in-memory buffers ----
    // bitmaps are exactly one block each (spec)
    uint8_t *inode_bitmap = calloc(1, BS);
    uint8_t *data_bitmap  = calloc(1, BS);
    if (!inode_bitmap || !data_bitmap) { perror("calloc bitmap"); exit(1); }

    // inode table: enough blocks to hold 'inodes' * 128B
    size_t it_bytes = inode_table_blocks * BS;
    uint8_t *inode_table_bytes = calloc(1, it_bytes);
    if (!inode_table_bytes) { perror("calloc inode table"); exit(1); }

    // A block buffer for writing single blocks
    uint8_t *blk = calloc(1, BS);
    if (!blk) { perror("calloc blk"); exit(1); }

    // ---- 3) Superblock ----
    superblock_t sb = {0};
    sb.magic               = 0x4D565346u; // 'MVSF'
    sb.version             = 1;
    sb.block_size          = BS;
    sb.total_blocks        = total_blocks;
    sb.inode_count         = inodes;

    sb.inode_bitmap_start  = inode_bitmap_start;
    sb.inode_bitmap_blocks = 1;
    sb.data_bitmap_start   = data_bitmap_start;
    sb.data_bitmap_blocks  = 1;
    sb.inode_table_start   = inode_table_start;
    sb.inode_table_blocks  = inode_table_blocks;
    sb.data_region_start   = data_region_start;
    sb.data_region_blocks  = data_region_blocks;

    sb.root_inode          = ROOT_INO;              // 1
    sb.mtime_epoch         = (uint64_t)time(NULL);  // build time
    sb.flags               = 0;

    superblock_crc_finalize(&sb); // writes sb.checksum

    // ---- 4) Bitmaps: mark root inode (1) and its first data block ----
    // inodes are 1-indexed logically; bitmap index 0 => inode #1
    set_bit(inode_bitmap, 0);

    // data bitmap index 0 => first data block in data region is allocated for root dir
    set_bit(data_bitmap, 0);

    // ---- 5) Inode table: write root inode (#1) ----
    // Map inode #1 to array index 0
    inode_t *it = (inode_t *)inode_table_bytes;
    inode_t *root = &it[0];
    memset(root, 0, sizeof(*root));

    // mode: directory 0040000 (octal)
    root->mode       = 040000;     // directory
    root->links      = 2;          // '.' and '..'
    root->uid        = 0;
    root->gid        = 0;
    root->size_bytes = BS;         // one full directory block
    root->atime = root->mtime = root->ctime = (uint64_t)time(NULL);

    // first direct pointer points to the absolute block number of the data block
    root->direct[0]  = (uint32_t)data_region_start;
    for (int i = 1; i < DIRECT_MAX; ++i) root->direct[i] = 0;

    root->reserved_0 = root->reserved_1 = root->reserved_2 = 0;
    root->proj_id    = 0;
    root->uid16_gid16= 0;
    root->xattr_ptr  = 0;

    inode_crc_finalize(root);

    // ---- 6) Build the root directory data block with '.' and '..' ----
    memset(blk, 0, BS);
    dirent64_t *d0 = (dirent64_t *)blk;             // first 64 bytes
    dirent64_t *d1 = (dirent64_t *)(blk + 64);      // second 64 bytes

    // '.'
    memset(d0, 0, sizeof(*d0));
    d0->inode_no = ROOT_INO;
    d0->type     = 2;   // directory
    strncpy(d0->name, ".", sizeof(d0->name) - 1);
    dirent_checksum_finalize(d0);

    // '..' (root points to itself)
    memset(d1, 0, sizeof(*d1));
    d1->inode_no = ROOT_INO;
    d1->type     = 2;
    strncpy(d1->name, "..", sizeof(d1->name) - 1);
    dirent_checksum_finalize(d1);

    // ---- 7) Create/truncate the output file to the exact size ----
    FILE *f = fopen(image_path, "wb+");
    if (!f) { perror("fopen image"); exit(1); }

    // Pre-size file by writing zero blocks
    uint8_t *zero = calloc(1, BS);
    if (!zero) { perror("calloc zero"); exit(1); }
    for (uint64_t b = 0; b < total_blocks; ++b) {
        if (fwrite(zero, 1, BS, f) != BS) { perror("fwrite zero"); exit(1); }
    }
    free(zero);

    // Helper to write a block at absolute block number
        // auto write_block = [&](uint64_t blkno, const void *data){
        //     if (fseeko(f, (off_t)(blkno * (off_t)BS), SEEK_SET) != 0) { perror("fseeko"); exit(1); }
        //     if (fwrite(data, 1, BS, f) != BS) { perror("fwrite block"); exit(1); }
        // };

    // ---- 8) Write superblock, bitmaps, inode table, and root dir block ----
    // Superblock
    {
        uint8_t sb_block[BS]; memset(sb_block, 0, BS);
        memcpy(sb_block, &sb, sizeof(sb));
        write_block(f, superblock_start, sb_block);
    }
    // Inode bitmap
    write_block(f, inode_bitmap_start, inode_bitmap);
    // Data bitmap
    write_block(f, data_bitmap_start, data_bitmap);

    // Inode table: may span multiple blocks
    for (uint64_t i = 0; i < inode_table_blocks; ++i) {
        write_block(f, inode_table_start + i, inode_table_bytes + i * BS);
    }

    // Root directory data block at data_region_start
    write_block(f, data_region_start, blk);

    fclose(f);
    free(inode_bitmap);
    free(data_bitmap);
    free(inode_table_bytes);
    free(blk);

    printf("Wrote image '%s' successfully.\n", image_path);
    printf("Root inode at #%u, root dir block at #%lu\n", ROOT_INO, data_region_start);
}

// ========================= MAIN =========================
int main(int argc, char **argv) {
    crc32_init(); // always first, so CRC table is ready
    // WRITE YOUR DRIVER CODE HERE
    // PARSE YOUR CLI PARAMETERS
    // THEN CREATE YOUR FILE SYSTEM WITH A ROOT DIRECTORY
    // THEN SAVE THE DATA INSIDE THE OUTPUT IMAGE


    struct options opts = (struct options){0};
    parse_args(argc, argv, &opts);

    printf("Image = %s\n", opts.image);
    printf("Size  = %d KiB\n", opts.size_kib);
    printf("Inodes= %d\n", opts.inodes);

    compute_layout(opts.size_kib, opts.inodes);

    build_fs_image(opts.image, opts.size_kib, opts.inodes);
    return 0;
}

// ===========================================================================
// ========================= END OF FILE ============================================
// ===========================================================================
