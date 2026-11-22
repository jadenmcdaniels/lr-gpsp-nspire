/* Folders for tests/arm/libbench.c: the calculator's opendir and readdir
 * (Ndless's dirent.h, the OS's own calls), which libbench_os.c stands in
 * for. tests/arm/shim/dirent.h has none (the other tests list no folder). */
#ifndef LIBBENCH_DIRENT_H
#define LIBBENCH_DIRENT_H
struct dirent { char d_name[256]; unsigned char d_type; };
typedef struct shim_dir DIR;
#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10
DIR *opendir(const char *name);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);
#endif
