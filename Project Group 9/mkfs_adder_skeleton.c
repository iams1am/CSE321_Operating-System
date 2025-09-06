/* MiniVSFS mkfs_adder
 * Author: <Siam Abdullah>, ID <21301***> 
 * Group: 9
 * Course: CSE 321, Term Project (Summer 2025)
 * Notes: Adds a host file into an existing MiniVSFS image (<= 12 direct blocks).
 */



#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>   // for memcpy, memset
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <getopt.h>    // struct option, getopt_long, required_argument, optarg
#include <sys/stat.h>  // struct stat, stat()


#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12


#pragma pack(push, 1)
// ------------ On-disk structures ------------
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


//================================================
//================================================
// ================= Bit helpers =================
static inline int test_bit(const uint8_t *bm, uint64_t idx) {
    return (bm[idx >> 3] >> (idx & 7)) & 1u;
}
static inline void set_bit(uint8_t *bm, uint64_t idx) {
    bm[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}
static int find_first_zero_bit(const uint8_t *bm, uint64_t max_bits) {
    for (uint64_t i=0; i<max_bits; ++i) if (!test_bit(bm,i)) return (int)i;
    return -1;
}

// ================= Block IO =================
static void read_block(FILE *f, uint64_t blkno, void *buf) {
    long off = (long)(blkno * (uint64_t)BS);
    if (fseek(f, off, SEEK_SET) != 0) { perror("fseek read"); exit(1); }
    if (fread(buf, 1, BS, f) != BS) { perror("fread block"); exit(1); }
}
static void write_block(FILE *f, uint64_t blkno, const void *buf) {
    long off = (long)(blkno * (uint64_t)BS);
    if (fseek(f, off, SEEK_SET) != 0) { perror("fseek write"); exit(1); }
    if (fwrite(buf, 1, BS, f) != BS) { perror("fwrite block"); exit(1); }
}

// ================= CLI =================
struct options {
    char input[256];
    char output[256];
    char file[256];
};
static void parse_args(int argc, char **argv, struct options *opts) {
    static struct option long_opts[] = {
        {"input",  required_argument, 0, 'i'},
        {"output", required_argument, 0, 'o'},
        {"file",   required_argument, 0, 'f'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "i:o:f:", long_opts, NULL)) != -1) {
        switch (c) {
            case 'i': strncpy(opts->input,  optarg, sizeof(opts->input)-1); break;
            case 'o': strncpy(opts->output, optarg, sizeof(opts->output)-1); break;
            case 'f': strncpy(opts->file,   optarg, sizeof(opts->file)-1); break;
            default:
                fprintf(stderr, "Usage: %s --input in.img --output out.img --file myfile.txt\n", argv[0]);
                exit(1);
        }
    }
    if (!opts->input[0] || !opts->output[0] || !opts->file[0]) {
        fprintf(stderr, "Missing required args. Usage: %s --input in.img --output out.img --file myfile.txt\n", argv[0]);
        exit(1);
    }
}

// basename (simple, POSIX-free)
static void path_basename(const char *path, char *out, size_t outsz) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash+1 : path;
    strncpy(out, base, outsz-1);
    out[outsz-1] = '\0';
}

// ================= Core: add one file =================
static void add_file_to_image(const char *in_path, const char *out_path, const char *hostfile) {
    // 1) Open input image & read superblock
    FILE *fi = fopen(in_path, "rb");
    if (!fi) { perror("fopen input"); exit(1); }

    superblock_t sb;
    uint8_t blk0[BS]; memset(blk0,0,BS);
    read_block(fi, 0, blk0);
    memcpy(&sb, blk0, sizeof(sb));

    if (sb.magic != 0x4D565346u || sb.block_size != BS || sb.version != 1) {
        fprintf(stderr, "Invalid MiniVSFS image.\n");
        exit(1);
    }
    if (sb.inode_bitmap_blocks != 1 || sb.data_bitmap_blocks != 1) {
        fprintf(stderr, "Unexpected bitmap sizes.\n");
        exit(1);
    }

    // 2) Load bitmaps and inode table
    uint8_t *inode_bm = malloc(BS);
    uint8_t *data_bm  = malloc(BS);
    if (!inode_bm || !data_bm) { perror("malloc bitmap"); exit(1); }

    read_block(fi, sb.inode_bitmap_start, inode_bm);
    read_block(fi, sb.data_bitmap_start,  data_bm);

    size_t it_bytes = (size_t)sb.inode_table_blocks * BS;
    uint8_t *inode_table_bytes = malloc(it_bytes);
    if (!inode_table_bytes) { perror("malloc inode table"); exit(1); }
    for (uint64_t i=0; i<sb.inode_table_blocks; ++i) {
        read_block(fi, sb.inode_table_start + i, inode_table_bytes + i*BS);
    }

    // 3) Read host file
    struct stat st; 
    if (stat(hostfile, &st) != 0) { perror("stat hostfile"); exit(1); }
    uint64_t fsize = (uint64_t)st.st_size;
    uint64_t need_blocks = (fsize + BS - 1) / BS;

    if (need_blocks > DIRECT_MAX) {
        fprintf(stderr, "File too large for 12 direct blocks (%llu blocks needed)\n", (unsigned long long)need_blocks);
        exit(1);
    }
    if (need_blocks > sb.data_region_blocks) {
        fprintf(stderr, "Not enough data blocks in image.\n");
        exit(1);
    }

    FILE *fh = fopen(hostfile, "rb");
    if (!fh) { perror("fopen hostfile"); exit(1); }

    // 4) Find a free inode (first-fit)
    int inode_idx = find_first_zero_bit(inode_bm, sb.inode_count);
    if (inode_idx < 0) { fprintf(stderr, "No free inode available.\n"); exit(1); }
    uint32_t new_ino = (uint32_t)(inode_idx + 1);

    // 5) Find free data blocks (first-fit)
    uint32_t blk_indices[DIRECT_MAX] = {0};
    uint64_t found = 0;
    for (uint64_t i=0; i<sb.data_region_blocks && found<need_blocks; ++i) {
        if (!test_bit(data_bm, i)) {
            blk_indices[found++] = (uint32_t)i;  // index inside data region
        }
    }
    if (found < need_blocks) {
        fprintf(stderr, "Not enough free data blocks.\n");
        exit(1);
    }

    // 6) Create output image by copying input, then patch
    FILE *fo = fopen(out_path, "wb+");
    if (!fo) { perror("fopen output"); exit(1); }

    // Copy whole file (small images, simple loop)
    rewind(fi);
    uint8_t buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fi)) > 0) {
        if (fwrite(buf, 1, n, fo) != n) { perror("copy write"); exit(1); }
    }

    // 7) Write file data blocks into output image
    uint8_t blockbuf[BS];
    for (uint64_t k=0; k<need_blocks; ++k) {
        memset(blockbuf, 0, BS);
        size_t rn = fread(blockbuf, 1, BS, fh); // last block may be partial; zero-padded
        (void)rn;  // we always write a full zero-padded block
        // absolute block number in image:
        uint64_t abs_blk = sb.data_region_start + blk_indices[k];
        write_block(fo, abs_blk, blockbuf);
    }
    fclose(fh);

    // 8) Update data bitmap
    for (uint64_t k=0; k<need_blocks; ++k) set_bit(data_bm, blk_indices[k]);
    write_block(fo, sb.data_bitmap_start, data_bm);

    // 9) Create new inode in table
    inode_t *itable = (inode_t*)inode_table_bytes;
    inode_t *ni = &itable[inode_idx];
    memset(ni, 0, sizeof(*ni));

    ni->mode       = 0100000;   // regular file
    ni->links      = 1;
    ni->uid        = 0;
    ni->gid        = 0;
    ni->size_bytes = fsize;
    ni->atime = ni->mtime = ni->ctime = (uint64_t)time(NULL);

    for (uint64_t k=0; k<need_blocks; ++k)
        ni->direct[k] = (uint32_t)(sb.data_region_start + blk_indices[k]);
    for (int k=(int)need_blocks; k<DIRECT_MAX; ++k)
        ni->direct[k] = 0;

    inode_crc_finalize(ni);

    // 10) Mark inode bitmap & write
    set_bit(inode_bm, (uint64_t)inode_idx);
    write_block(fo, sb.inode_bitmap_start, inode_bm);

    // 11) Append dirent to root directory block
    // Load root inode to get its first data block (root dir block)
    inode_t *root = &itable[0];                        // inode #1 → index 0
    uint32_t root_dir_block = root->direct[0];         // absolute block number
    uint8_t dirblk[BS];
    read_block(fo, root_dir_block, dirblk);

    // Find free 64B slot (inode_no == 0)
    dirent64_t *de = (dirent64_t*)dirblk;
    int placed = 0;
    for (int i=0; i<(int)(BS/sizeof(dirent64_t)); ++i) {
        if (de[i].inode_no == 0) {
            de[i].inode_no = new_ino;
            de[i].type     = 1; // file
            char base[58]; path_basename(hostfile, base, sizeof(base));
            strncpy(de[i].name, base, sizeof(de[i].name)-1);
            de[i].name[sizeof(de[i].name)-1] = '\0';
            dirent_checksum_finalize(&de[i]);
            placed = 1;
            break;
        }
    }
    if (!placed) {
        fprintf(stderr, "Root directory block has no free entry.\n");
        exit(1);
    }
    write_block(fo, root_dir_block, dirblk);

    // 12) Write back inode table (all blocks)
    for (uint64_t i=0; i<sb.inode_table_blocks; ++i) {
        write_block(fo, sb.inode_table_start + i, inode_table_bytes + i*BS);
    }

    // 13) Update superblock mtime + checksum
    sb.mtime_epoch = (uint64_t)time(NULL);
    superblock_crc_finalize(&sb);
    memset(blk0, 0, BS);
    memcpy(blk0, &sb, sizeof(sb));
    write_block(fo, 0, blk0);

    fclose(fi);
    fclose(fo);
    free(inode_bm);
    free(data_bm);
    free(inode_table_bytes);

    printf("Added '%s' as inode #%u using %llu block(s). Output: %s\n",
           hostfile, new_ino, (unsigned long long)need_blocks, out_path);
}

// ================= main =================
int main(int argc, char **argv) {
    crc32_init();
    // WRITE YOUR DRIVER CODE HERE
    // PARSE YOUR CLI PARAMETERS
    // THEN ADD THE SPECIFIED FILE TO YOUR FILE SYSTEM
    // UPDATE THE .IMG FILE ON DISK
    struct options opts = {0};
    parse_args(argc, argv, &opts);

    printf("Input  = %s\n", opts.input);
    printf("Output = %s\n", opts.output);
    printf("File   = %s\n", opts.file);

    add_file_to_image(opts.input, opts.output, opts.file);
    return 0;
}

// =================================================
// =================================================
// ================= END OF FILE =================
// =================================================
// =================================================    
