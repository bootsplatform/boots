#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>

#define MAX_LINE        4096
#define MAX_FIELD       512
#define MAX_ART_LINES   128
#define MAX_ART_WIDTH   256
#define MAX_CODE_SIZE   131072
#define MAX_COMMANDS    64
#define MAX_CMD_LEN     256
#define MAX_FILES       128
#define MAX_PATH        1024
#define MAX_INLINED     32
#define BOOTS_DIR       ".boots"
#define CACHE_DIR       ".boots/cache"
#define VERSION         "2.0.0"

typedef enum { LANG_C, LANG_CPP, LANG_PYTHON, LANG_UNKNOWN } Language;

typedef struct {
    char filename[MAX_PATH];
    char code[MAX_CODE_SIZE];
    int  code_len;
} InlinedFile;

typedef struct {
    char name[MAX_FIELD];
    char version[MAX_FIELD];
    char author[MAX_FIELD];
    char description[MAX_FIELD];
    char engine[MAX_FIELD];
    char sensors[MAX_FIELD];
    char actuators[MAX_FIELD];
    char power[MAX_FIELD];
    char comm[MAX_FIELD];
    char srcdir[MAX_PATH];
    char main_file[MAX_PATH];
    char extra_flags[MAX_FIELD];
    char files[MAX_FILES][MAX_PATH];
    int  num_files;
    char art[MAX_ART_LINES][MAX_ART_WIDTH];
    int  art_lines;
    char code[MAX_CODE_SIZE];
    int  code_len;
    InlinedFile inlined[MAX_INLINED];
    int  num_inlined;
    Language lang;
    char commands[MAX_COMMANDS][MAX_CMD_LEN];
    int  num_commands;
} Robot;

static const char *R  = "\033[0m";
static const char *B  = "\033[1m";
static const char *CY = "\033[36m";
static const char *GR = "\033[32m";
static const char *YL = "\033[33m";
static const char *RD = "\033[31m";
static const char *MG = "\033[35m";
static const char *BL = "\033[34m";
static const char *DM = "\033[2m";

static const char *DEFAULT_ART[] = {
    "   .------.",
    "  / o    o \\",
    " |  ------  |",
    " |  BOOTS   |",
    "  \\________/",
    "  |        |",
    "  |        |",
    "  |__|  |__|",
    NULL
};

static const char *BANNER =
    "\033[36m\033[1m"
    "  ██████╗  ██████╗  ██████╗ ████████╗███████╗\n"
    "  ██╔══██╗██╔═══██╗██╔═══██╗╚══██╔══╝██╔════╝\n"
    "  ██████╔╝██║   ██║██║   ██║   ██║   ███████╗\n"
    "  ██╔══██╗██║   ██║██║   ██║   ██║   ╚════██║\n"
    "  ██████╔╝╚██████╔╝╚██████╔╝   ██║   ███████║\n"
    "  ╚═════╝  ╚═════╝  ╚═════╝    ╚═╝   ╚══════╝\n"
    "\033[0m";

static void trim(char *s) {
    char *p = s;
    while (isspace((unsigned char)*p)) p++;
    memmove(s, p, strlen(p) + 1);
    int len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len-1])) s[--len] = '\0';
}

static void ensure_dirs(void) {
    mkdir(BOOTS_DIR, 0755);
    mkdir(CACHE_DIR, 0755);
}

static void ensure_robot_dir(const char *name) {
    char p[MAX_PATH];
    ensure_dirs();
    snprintf(p, sizeof(p), "%s/%s", CACHE_DIR, name);
    mkdir(p, 0755);
}

static Language detect_language(const char *s) {
    char buf[64];
    strncpy(buf, s, 63); buf[63] = '\0';
    for (int i = 0; buf[i]; i++) buf[i] = tolower((unsigned char)buf[i]);
    trim(buf);
    if (strcmp(buf, "c") == 0) return LANG_C;
    if (strcmp(buf, "c++") == 0 || strcmp(buf, "cpp") == 0) return LANG_CPP;
    if (strcmp(buf, "python") == 0 || strcmp(buf, "python3") == 0) return LANG_PYTHON;
    return LANG_UNKNOWN;
}

static int write_file(const char *path, const char *data, int len) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "%swrite_file: cannot open '%s': %s%s\n", RD, path, strerror(errno), R); return 0; }
    fwrite(data, 1, len, f);
    fclose(f);
    return 1;
}

static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) { fprintf(stderr, "%scopy: cannot open '%s': %s%s\n", RD, src, strerror(errno), R); return 0; }
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); fprintf(stderr, "%scopy: cannot create '%s': %s%s\n", RD, dst, strerror(errno), R); return 0; }
    char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(in); fclose(out);
    return 1;
}

static void append_code(Robot *robot, const char *line) {
    int rem = MAX_CODE_SIZE - robot->code_len - 2;
    if (rem <= 0) return;
    int ll = strlen(line); if (ll > rem) ll = rem;
    memcpy(robot->code + robot->code_len, line, ll);
    robot->code_len += ll;
    robot->code[robot->code_len++] = '\n';
}

static void append_inlined(InlinedFile *f, const char *line) {
    int rem = MAX_CODE_SIZE - f->code_len - 2;
    if (rem <= 0) return;
    int ll = strlen(line); if (ll > rem) ll = rem;
    memcpy(f->code + f->code_len, line, ll);
    f->code_len += ll;
    f->code[f->code_len++] = '\n';
}

static int parse_robot(const char *path, Robot *robot) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "%s%sError:%s Cannot open '%s': %s\n", RD, B, R, path, strerror(errno));
        return 0;
    }
    memset(robot, 0, sizeof(Robot));

    typedef enum { S_NONE, S_HW, S_ART, S_CODE, S_CMD, S_FILES, S_INLINED } Sec;
    Sec sec = S_NONE;
    int cur_inlined = -1;
    char line[MAX_LINE];
    char lang_buf[64] = "c";

    while (fgets(line, sizeof(line), f)) {
        int len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';

        if (line[0] == '[') {
            if (!strcmp(line, "[hardware]"))  { sec = S_HW; continue; }
            if (!strcmp(line, "[art]"))        { sec = S_ART; continue; }
            if (!strcmp(line, "[code]"))        { sec = S_CODE; continue; }
            if (!strcmp(line, "[commands]"))   { sec = S_CMD; continue; }
            if (!strcmp(line, "[files]"))      { sec = S_FILES; continue; }
            if (!strncmp(line, "[code:", 6) && line[len-1] == ']') {
                if (robot->num_inlined < MAX_INLINED) {
                    cur_inlined = robot->num_inlined++;
                    char fname[MAX_PATH];
                    strncpy(fname, line + 6, len - 7);
                    fname[len-7] = '\0';
                    trim(fname);
                    strncpy(robot->inlined[cur_inlined].filename, fname, MAX_PATH-1);
                    robot->inlined[cur_inlined].code_len = 0;
                    sec = S_INLINED;
                }
                continue;
            }
            sec = S_NONE; cur_inlined = -1;
            continue;
        }

        if (sec == S_NONE) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0'; char *k = line, *v = eq+1;
            trim(k); trim(v);
            if      (!strcmp(k,"name"))        strncpy(robot->name, v, MAX_FIELD-1);
            else if (!strcmp(k,"version"))     strncpy(robot->version, v, MAX_FIELD-1);
            else if (!strcmp(k,"author"))      strncpy(robot->author, v, MAX_FIELD-1);
            else if (!strcmp(k,"description")) strncpy(robot->description, v, MAX_FIELD-1);
            else if (!strcmp(k,"language"))    strncpy(lang_buf, v, 63);
            else if (!strcmp(k,"srcdir"))      strncpy(robot->srcdir, v, MAX_PATH-1);
            else if (!strcmp(k,"main"))        strncpy(robot->main_file, v, MAX_PATH-1);
            else if (!strcmp(k,"flags"))       strncpy(robot->extra_flags, v, MAX_FIELD-1);
        } else if (sec == S_HW) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0'; char *k = line, *v = eq+1;
            trim(k); trim(v);
            if      (!strcmp(k,"engine"))    strncpy(robot->engine, v, MAX_FIELD-1);
            else if (!strcmp(k,"sensors"))   strncpy(robot->sensors, v, MAX_FIELD-1);
            else if (!strcmp(k,"actuators")) strncpy(robot->actuators, v, MAX_FIELD-1);
            else if (!strcmp(k,"power"))     strncpy(robot->power, v, MAX_FIELD-1);
            else if (!strcmp(k,"comm"))      strncpy(robot->comm, v, MAX_FIELD-1);
        } else if (sec == S_ART) {
            if (robot->art_lines < MAX_ART_LINES)
                strncpy(robot->art[robot->art_lines++], line, MAX_ART_WIDTH-1);
        } else if (sec == S_CODE) {
            append_code(robot, line);
        } else if (sec == S_INLINED && cur_inlined >= 0) {
            append_inlined(&robot->inlined[cur_inlined], line);
        } else if (sec == S_FILES) {
            trim(line);
            if (line[0] && robot->num_files < MAX_FILES)
                strncpy(robot->files[robot->num_files++], line, MAX_PATH-1);
        } else if (sec == S_CMD) {
            trim(line);
            if (line[0] && robot->num_commands < MAX_COMMANDS)
                strncpy(robot->commands[robot->num_commands++], line, MAX_CMD_LEN-1);
        }
    }
    fclose(f);
    robot->lang = detect_language(lang_buf);
    if (!robot->name[0]) {
        const char *base = strrchr(path, '/');
        base = base ? base+1 : path;
        strncpy(robot->name, base, MAX_FIELD-1);
        char *dot = strrchr(robot->name, '.'); if (dot) *dot = '\0';
    }
    return 1;
}

static void sep(const char *col) {
    printf("%s%s━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━%s\n", col, B, R);
}

static void print_card(const Robot *robot) {
    sep(CY);
    printf("%s%s  ROBOT: %s%s  v%s%s\n", B, CY, robot->name, YL, robot->version[0]?robot->version:"1.0", R);
    if (robot->description[0]) printf("%s  %s%s\n", DM, robot->description, R);
    if (robot->author[0])      printf("%s  by %s%s\n", DM, robot->author, R);
    sep(CY);
    if (robot->art_lines > 0)
        for (int i = 0; i < robot->art_lines; i++) printf("%s%s%s\n", GR, robot->art[i], R);
    else
        for (int i = 0; DEFAULT_ART[i]; i++) printf("%s%s%s\n", GR, DEFAULT_ART[i], R);
    sep(BL);
    printf("%s%s  HARDWARE SPECS%s\n", B, BL, R);
    if (robot->engine[0])    printf("  %sEngine:%s    %s\n", YL, R, robot->engine);
    if (robot->sensors[0])   printf("  %sSensors:%s   %s\n", YL, R, robot->sensors);
    if (robot->actuators[0]) printf("  %sActuators:%s %s\n", YL, R, robot->actuators);
    if (robot->power[0])     printf("  %sPower:%s     %s\n", YL, R, robot->power);
    if (robot->comm[0])      printf("  %sComm:%s      %s\n", YL, R, robot->comm);
    const char *ls = robot->lang==LANG_C?"C":robot->lang==LANG_CPP?"C++":robot->lang==LANG_PYTHON?"Python":"Unknown";
    printf("  %sLanguage:%s  %s\n", YL, R, ls);

    int has_multi = robot->num_files > 0 || robot->srcdir[0] || robot->num_inlined > 1;
    if (has_multi) {
        printf("  %sProject:%s   multi-file\n", YL, R);
        if (robot->srcdir[0])   printf("  %sSrcDir:%s    %s\n", YL, R, robot->srcdir);
        if (robot->main_file[0]) printf("  %sMain:%s      %s\n", YL, R, robot->main_file);
        if (robot->num_files > 0) {
            printf("  %sFiles:%s\n", YL, R);
            for (int i = 0; i < robot->num_files; i++)
                printf("    %s• %s%s\n", MG, robot->files[i], R);
        }
        if (robot->num_inlined > 0) {
            printf("  %sInlined:%s\n", YL, R);
            for (int i = 0; i < robot->num_inlined; i++)
                printf("    %s• %s%s  %s(%d bytes)%s\n", MG, robot->inlined[i].filename, R,
                       DM, robot->inlined[i].code_len, R);
        }
    }
    if (robot->extra_flags[0]) printf("  %sFlags:%s     %s\n", YL, R, robot->extra_flags);
    sep(BL);
}

static const char *robot_dir(const char *name) {
    static char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s/%s", CACHE_DIR, name);
    return p;
}
static const char *bin_path(const char *name) {
    static char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s/%s/%s_robot", CACHE_DIR, name, name);
    return p;
}
static const char *py_main_path(const Robot *robot) {
    static char p[MAX_PATH];
    if (robot->main_file[0])
        snprintf(p, sizeof(p), "%s/%s/%s", CACHE_DIR, robot->name, robot->main_file);
    else
        snprintf(p, sizeof(p), "%s/%s/main.py", CACHE_DIR, robot->name);
    return p;
}

static int collect_srcdir_files(const char *srcdir, Language lang,
                                 char out[][MAX_PATH], int *count, int max) {
    DIR *d = opendir(srcdir);
    if (!d) {
        fprintf(stderr, "%scannot open srcdir '%s': %s%s\n", RD, srcdir, strerror(errno), R);
        return 0;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) && *count < max) {
        const char *nm = ent->d_name;
        int l = strlen(nm);
        int match = 0;
        if (lang == LANG_C   && l > 2 && !strcmp(nm+l-2, ".c"))   match = 1;
        if (lang == LANG_CPP && l > 4 && (!strcmp(nm+l-4,".cpp")||!strcmp(nm+l-2,".cc"))) match = 1;
        if (lang == LANG_CPP && l > 2 && !strcmp(nm+l-2, ".c"))   match = 1;
        if (lang == LANG_PYTHON && l > 3 && !strcmp(nm+l-3, ".py")) match = 1;
        if (match) {
            snprintf(out[*count], MAX_PATH, "%s/%s", srcdir, nm);
            (*count)++;
        }
    }
    closedir(d);
    return 1;
}

static int copy_srcdir_headers(const char *srcdir, const char *destdir) {
    DIR *d = opendir(srcdir);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        const char *nm = ent->d_name;
        int l = strlen(nm);
        if (l > 2 && !strcmp(nm+l-2, ".h")) {
            char src[MAX_PATH], dst[MAX_PATH];
            snprintf(src, sizeof(src), "%s/%s", srcdir, nm);
            snprintf(dst, sizeof(dst), "%s/%s", destdir, nm);
            copy_file(src, dst);
        }
        if (l > 4 && !strcmp(nm+l-4, ".hpp")) {
            char src[MAX_PATH], dst[MAX_PATH];
            snprintf(src, sizeof(src), "%s/%s", srcdir, nm);
            snprintf(dst, sizeof(dst), "%s/%s", destdir, nm);
            copy_file(src, dst);
        }
    }
    closedir(d);
    return 1;
}

static int run_compiler(const char *cmd) {
    FILE *p = popen(cmd, "r");
    if (!p) { fprintf(stderr, "%sFailed to run compiler.%s\n", RD, R); return 0; }
    char buf[512]; int had = 0;
    while (fgets(buf, sizeof(buf), p)) {
        if (!had) { printf("%s--- Compiler Output ---%s\n", YL, R); had = 1; }
        printf("  %s%s%s", DM, buf, R);
    }
    int ret = pclose(p);
    if (ret != 0) { fprintf(stderr, "%s%s[ERROR]%s Compilation failed.\n", RD, B, R); return 0; }
    return 1;
}

static int compile_robot(const Robot *robot, const char *robot_file_path) {
    ensure_robot_dir(robot->name);
    const char *rdir = robot_dir(robot->name);
    const char *bin  = bin_path(robot->name);

    char robot_base_dir[MAX_PATH];
    strncpy(robot_base_dir, robot_file_path, sizeof(robot_base_dir)-1);
    char *slash = strrchr(robot_base_dir, '/');
    if (slash) *slash = '\0'; else strcpy(robot_base_dir, ".");

    if (robot->lang == LANG_PYTHON) {
        int written = 0;
        if (robot->num_inlined > 0) {
            for (int i = 0; i < robot->num_inlined; i++) {
                char dst[MAX_PATH];
                snprintf(dst, sizeof(dst), "%s/%s", rdir, robot->inlined[i].filename);
                char ddir[MAX_PATH]; strncpy(ddir, dst, sizeof(ddir)-1);
                char *sl = strrchr(ddir, '/'); if (sl) { *sl = '\0'; mkdir(ddir, 0755); }
                if (!write_file(dst, robot->inlined[i].code, robot->inlined[i].code_len)) return 0;
                printf("  %s+%s %s\n", GR, R, robot->inlined[i].filename);
                written++;
            }
        }
        if (robot->code_len > 0) {
            const char *mf = robot->main_file[0] ? robot->main_file : "main.py";
            char dst[MAX_PATH]; snprintf(dst, sizeof(dst), "%s/%s", rdir, mf);
            if (!write_file(dst, robot->code, robot->code_len)) return 0;
            printf("  %s+%s %s (main)\n", GR, R, mf);
            written++;
        }
        if (robot->num_files > 0) {
            for (int i = 0; i < robot->num_files; i++) {
                char src[MAX_PATH];
                if (robot->files[i][0] == '/') strncpy(src, robot->files[i], MAX_PATH-1);
                else snprintf(src, sizeof(src), "%s/%s", robot_base_dir, robot->files[i]);
                const char *bname = strrchr(robot->files[i], '/');
                bname = bname ? bname+1 : robot->files[i];
                char dst[MAX_PATH]; snprintf(dst, sizeof(dst), "%s/%s", rdir, bname);
                if (!copy_file(src, dst)) return 0;
                printf("  %s+%s %s\n", GR, R, bname);
                written++;
            }
        }
        if (robot->srcdir[0]) {
            char abs_srcdir[MAX_PATH];
            if (robot->srcdir[0] == '/') strncpy(abs_srcdir, robot->srcdir, MAX_PATH-1);
            else snprintf(abs_srcdir, sizeof(abs_srcdir), "%s/%s", robot_base_dir, robot->srcdir);
            char collected[MAX_FILES][MAX_PATH]; int nc = 0;
            collect_srcdir_files(abs_srcdir, LANG_PYTHON, collected, &nc, MAX_FILES);
            for (int i = 0; i < nc; i++) {
                const char *bname = strrchr(collected[i], '/');
                bname = bname ? bname+1 : collected[i];
                char dst[MAX_PATH]; snprintf(dst, sizeof(dst), "%s/%s", rdir, bname);
                if (!copy_file(collected[i], dst)) return 0;
                printf("  %s+%s %s\n", GR, R, bname);
                written++;
            }
        }
        if (!written) { printf("%s[WARN]%s No Python files found.\n", YL, R); return 0; }
        char mainpy[MAX_PATH]; snprintf(mainpy, sizeof(mainpy), "%s/%s",
            rdir, robot->main_file[0] ? robot->main_file : "main.py");
        chmod(mainpy, 0755);
        printf("%s%s[OK]%s Python project ready in %s/\n", GR, B, R, rdir);
        return 1;
    }

    const char *cc = (robot->lang == LANG_CPP) ? "g++" : "gcc";
    char srcs[MAX_FILES][MAX_PATH]; int nsrcs = 0;
    char objs_buf[MAX_FILES * (MAX_PATH + 4)]; objs_buf[0] = '\0';
    int compile_ok = 1;

    copy_srcdir_headers(robot_base_dir, rdir);

    if (robot->num_inlined > 0) {
        for (int i = 0; i < robot->num_inlined; i++) {
            char dst[MAX_PATH];
            snprintf(dst, sizeof(dst), "%s/%s", rdir, robot->inlined[i].filename);
            char ddir[MAX_PATH]; strncpy(ddir, dst, sizeof(ddir)-1);
            char *sl = strrchr(ddir, '/'); if (sl) { *sl = '\0'; mkdir(ddir, 0755); }
            if (!write_file(dst, robot->inlined[i].code, robot->inlined[i].code_len)) return 0;
            const char *nm = robot->inlined[i].filename;
            int nl = strlen(nm);
            int is_src = (nl>2&&!strcmp(nm+nl-2,".c")) || (nl>4&&!strcmp(nm+nl-4,".cpp"))
                       || (nl>3&&!strcmp(nm+nl-3,".cc"));
            if (is_src && nsrcs < MAX_FILES) {
                strncpy(srcs[nsrcs++], dst, MAX_PATH-1);
                printf("  %s+%s %s (inlined)\n", GR, R, nm);
            }
        }
    }

    if (robot->code_len > 0) {
        const char *ext = (robot->lang == LANG_CPP) ? ".cpp" : ".c";
        char dst[MAX_PATH]; snprintf(dst, sizeof(dst), "%s/main%s", rdir, ext);
        if (!write_file(dst, robot->code, robot->code_len)) return 0;
        if (nsrcs < MAX_FILES) { strncpy(srcs[nsrcs++], dst, MAX_PATH-1); printf("  %s+%s main%s (inline)\n", GR, R, ext); }
    }

    if (robot->num_files > 0) {
        for (int i = 0; i < robot->num_files; i++) {
            char src[MAX_PATH];
            if (robot->files[i][0] == '/') strncpy(src, robot->files[i], MAX_PATH-1);
            else snprintf(src, sizeof(src), "%s/%s", robot_base_dir, robot->files[i]);
            const char *bname = strrchr(robot->files[i], '/');
            bname = bname ? bname+1 : robot->files[i];
            char dst[MAX_PATH]; snprintf(dst, sizeof(dst), "%s/%s", rdir, bname);
            if (!copy_file(src, dst)) return 0;
            int nl = strlen(bname);
            int is_src = (nl>2&&!strcmp(bname+nl-2,".c")) || (nl>4&&!strcmp(bname+nl-4,".cpp"))
                       || (nl>3&&!strcmp(bname+nl-3,".cc"));
            if (is_src && nsrcs < MAX_FILES) { strncpy(srcs[nsrcs++], dst, MAX_PATH-1); printf("  %s+%s %s\n", GR, R, bname); }
        }
    }

    if (robot->srcdir[0]) {
        char abs_srcdir[MAX_PATH];
        if (robot->srcdir[0] == '/') strncpy(abs_srcdir, robot->srcdir, MAX_PATH-1);
        else snprintf(abs_srcdir, sizeof(abs_srcdir), "%s/%s", robot_base_dir, robot->srcdir);
        copy_srcdir_headers(abs_srcdir, rdir);
        char collected[MAX_FILES][MAX_PATH]; int nc = 0;
        collect_srcdir_files(abs_srcdir, robot->lang, collected, &nc, MAX_FILES);
        for (int i = 0; i < nc; i++) {
            const char *bname = strrchr(collected[i], '/');
            bname = bname ? bname+1 : collected[i];
            char dst[MAX_PATH]; snprintf(dst, sizeof(dst), "%s/%s", rdir, bname);
            if (!copy_file(collected[i], dst)) return 0;
            if (nsrcs < MAX_FILES) { strncpy(srcs[nsrcs++], dst, MAX_PATH-1); printf("  %s+%s %s\n", GR, R, bname); }
        }
    }

    if (nsrcs == 0) { printf("%s[WARN]%s No source files found.\n", YL, R); return 0; }

    printf("%s%s[COMPILE]%s %s (%d file%s) ...\n", CY, B, R,
           robot->lang==LANG_CPP?"C++":"C", nsrcs, nsrcs==1?"":"s");

    for (int i = 0; i < nsrcs; i++) {
        char obj[MAX_PATH]; snprintf(obj, sizeof(obj), "%s.o", srcs[i]);
        char cmd[MAX_PATH*3];
        snprintf(cmd, sizeof(cmd), "%s -O2 -I\"%s\" -c -o \"%s\" \"%s\" %s 2>&1",
                 cc, rdir, obj, srcs[i], robot->extra_flags);
        if (!run_compiler(cmd)) { compile_ok = 0; break; }
        int ol = strlen(objs_buf);
        snprintf(objs_buf + ol, sizeof(objs_buf) - ol, " \"%s\"", obj);
    }

    if (!compile_ok) return 0;

    char link_cmd[MAX_PATH*4];
    snprintf(link_cmd, sizeof(link_cmd), "%s -O2 -o \"%s\"%s -lm %s 2>&1",
             cc, bin, objs_buf, robot->extra_flags);
    if (!run_compiler(link_cmd)) return 0;

    printf("%s%s[OK]%s Binary: %s  (%d source file%s)\n", GR, B, R, bin, nsrcs, nsrcs==1?"":"s");
    return 1;
}

static void do_init(const char *path) {
    printf("%s", BANNER);
    Robot robot;
    if (!parse_robot(path, &robot)) return;
    print_card(&robot);
    printf("%s%s[INIT]%s Building robot '%s'...\n", CY, B, R, robot.name);
    if (compile_robot(&robot, path))
        printf("\n%s%s  Robot '%s' is ONLINE.%s\n  %sRun: boots interact %s%s\n",
               GR, B, robot.name, R, DM, path, R);
    else
        printf("\n%s%s[FAIL]%s Robot '%s' could not initialize.\n", RD, B, R, robot.name);
}

static void do_interact(const char *path) {
    Robot robot;
    if (!parse_robot(path, &robot)) return;
    if (robot.art_lines > 0) {
        printf("\n");
        for (int i = 0; i < robot.art_lines; i++) printf("%s%s%s\n", GR, robot.art[i], R);
        printf("\n");
    }
    printf("\n%s%s╔══════════════════════════════════════╗%s\n", CY, B, R);
    printf("%s%s║   ROBOT: %-28s║%s\n", CY, B, robot.name, R);
    printf("%s%s╚══════════════════════════════════════╝%s\n", CY, B, R);
    if (robot.num_commands > 0) {
        printf("%s  Commands:%s\n", YL, R);
        for (int i = 0; i < robot.num_commands; i++)
            printf("    %s• %s%s\n", MG, robot.commands[i], R);
    }
    printf("%s  Built-ins: help, status, exit%s\n\n", DM, R);

    char input[MAX_LINE];
    while (1) {
        printf("%s%s[%s]%s %s>%s ", CY, B, robot.name, R, GR, R);
        fflush(stdout);
        if (!fgets(input, sizeof(input), stdin)) break;
        int len = strlen(input);
        if (len > 0 && input[len-1] == '\n') input[--len] = '\0';
        trim(input);
        if (!input[0]) continue;

        if (!strcmp(input,"exit") || !strcmp(input,"quit")) {
            printf("%s%s[%s]%s Disconnected.\n", CY, B, robot.name, R); break;
        }
        if (!strcmp(input,"help")) {
            printf("  %sCommands:%s\n", YL, R);
            for (int i = 0; i < robot.num_commands; i++)
                printf("    %s• %s%s\n", MG, robot.commands[i], R);
            printf("    %s• status  exit%s\n", MG, R);
            continue;
        }
        if (!strcmp(input,"status")) {
            time_t t = time(NULL); struct tm *ti = localtime(&t); char tb[64];
            strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", ti);
            printf("  %sName:%s     %s\n", YL, R, robot.name);
            printf("  %sVersion:%s  %s\n", YL, R, robot.version[0]?robot.version:"1.0");
            printf("  %sEngine:%s   %s\n", YL, R, robot.engine[0]?robot.engine:"N/A");
            printf("  %sPower:%s    %s\n", YL, R, robot.power[0]?robot.power:"N/A");
            printf("  %sTime:%s     %s\n", YL, R, tb);
            continue;
        }

        if (robot.lang == LANG_PYTHON) {
            const char *mpy = py_main_path(&robot);
            struct stat st;
            if (stat(mpy, &st) != 0) {
                printf("%sNot initialized. Run: boots init %s%s\n", RD, path, R); continue;
            }
            char cmd[MAX_LINE + MAX_PATH + 64];
            snprintf(cmd, sizeof(cmd), "python3 \"%s\" %s 2>&1", mpy, input);
            (void)system(cmd);
        } else {
            const char *b = bin_path(robot.name);
            struct stat st;
            if (stat(b, &st) != 0) {
                printf("%sNot initialized. Run: boots init <file>.robot%s\n", RD, R); continue;
            }
            char cmd[MAX_LINE + MAX_PATH + 16];
            snprintf(cmd, sizeof(cmd), "\"%s\" %s", b, input);
            (void)system(cmd);
        }
    }
}

static void do_list(void) {
    printf("%s%s  INITIALIZED ROBOTS%s\n", B, CY, R); sep(CY);
    ensure_dirs();
    DIR *d = opendir(CACHE_DIR);
    if (!d) { printf("  %sNone.%s\n", DM, R); sep(CY); return; }
    struct dirent *ent; int n = 0;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        char rpath[MAX_PATH]; snprintf(rpath, sizeof(rpath), "%s/%s", CACHE_DIR, ent->d_name);
        struct stat st; stat(rpath, &st);
        if (!S_ISDIR(st.st_mode)) continue;
        char bin[MAX_PATH]; snprintf(bin, sizeof(bin), "%s/%s_robot", rpath, ent->d_name);
        char py[MAX_PATH];  snprintf(py,  sizeof(py),  "%s/main.py",  rpath);
        struct stat bs, ps;
        int has_bin = (stat(bin, &bs) == 0);
        int has_py  = (stat(py,  &ps) == 0);
        const char *tag = has_bin ? "(C/C++)" : has_py ? "(Python)" : "(pending)";
        printf("  %s• %s%s%s  %s%s%s\n", MG, B, ent->d_name, R, DM, tag, R);
        n++;
    }
    closedir(d);
    if (!n) printf("  %sNo robots initialized yet.%s\n", DM, R);
    sep(CY);
}

static void do_info(const char *path) {
    Robot robot; if (!parse_robot(path, &robot)) return;
    print_card(&robot);
    if (robot.num_commands > 0) {
        printf("%s%s  COMMANDS%s\n", B, MG, R);
        for (int i = 0; i < robot.num_commands; i++)
            printf("  %s• %s%s\n", MG, robot.commands[i], R);
        sep(MG);
    }
}

static int rm_rf(const char *path) {
    DIR *d = opendir(path);
    if (!d) { remove(path); return 0; }
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (!strcmp(ent->d_name,".") || !strcmp(ent->d_name,"..")) continue;
        char sub[MAX_PATH]; snprintf(sub, sizeof(sub), "%s/%s", path, ent->d_name);
        struct stat st; stat(sub, &st);
        if (S_ISDIR(st.st_mode)) rm_rf(sub);
        else remove(sub);
    }
    closedir(d);
    rmdir(path);
    return 1;
}

static void do_remove(const char *name) {
    char rdir[MAX_PATH]; snprintf(rdir, sizeof(rdir), "%s/%s", CACHE_DIR, name);
    struct stat st;
    if (stat(rdir, &st) == 0 && S_ISDIR(st.st_mode)) {
        rm_rf(rdir);
        printf("%s%s[OK]%s Robot '%s' removed.\n", GR, B, R, name);
    } else {
        printf("%s[INFO]%s '%s' not found in cache.\n", YL, R, name);
    }
}

static void do_example(void) {
    mkdir("ARIA_project", 0755);

    const char *robot_file =
"name        = ARIA\n"
"version     = 3.0\n"
"author      = BootsUser\n"
"description = Advanced Responsive Intelligence Agent (multi-file)\n"
"language    = c\n"
"srcdir      = ARIA_project\n"
"\n"
"[hardware]\n"
"engine      = Dual ARM Cortex-M7 @ 400MHz\n"
"sensors     = Ultrasonic, IR, Gyroscope, Accelerometer, Camera\n"
"actuators   = 6DOF Servo Arms, Omni-wheel Drive, Gripper\n"
"power       = 48V LiPo 10000mAh with wireless charging\n"
"comm        = WiFi 6, Bluetooth 5.2, CAN Bus\n"
"\n"
"[art]\n"
"       .------.\n"
"      /  O  O  \\\n"
"     |  ------  |\n"
"     |   ARIA   |\n"
"      \\________/\n"
"    __|        |__\n"
"   /  |        |  \\\n"
"  /   |________|   \\\n"
"       |      |\n"
"      _|      |_\n"
"     |__|    |__|\n"
"\n"
"[commands]\n"
"greet\n"
"scan\n"
"calculate <num> <op> <num>\n"
"move <direction>\n"
"say <message>\n"
"time\n"
"memory\n"
"\n";

    const char *header_file =
"#ifndef ARIA_H\n"
"#define ARIA_H\n"
"void cmd_greet(void);\n"
"void cmd_scan(void);\n"
"void cmd_calculate(int argc, char **argv);\n"
"void cmd_move(const char *dir);\n"
"void cmd_say(int argc, char **argv);\n"
"void cmd_time(void);\n"
"void cmd_memory(void);\n"
"#endif\n";

    const char *sensors_file =
"#include <stdio.h>\n"
"#include <stdlib.h>\n"
"#include <time.h>\n"
"#include \"aria.h\"\n"
"\n"
"void cmd_scan(void) {\n"
"    srand((unsigned)time(NULL));\n"
"    printf(\"[SCAN] Initiating environment scan...\\n\");\n"
"    printf(\"  Ultrasonic : %d cm\\n\", 20 + rand() % 280);\n"
"    printf(\"  IR         : %s\\n\", rand()%2 ? \"CLEAR\" : \"OBJECT at 8cm\");\n"
"    printf(\"  Gyro       : X=%.2f Y=%.2f Z=%.2f deg/s\\n\",\n"
"           (rand()%200-100)/100.0,(rand()%200-100)/100.0,(rand()%200-100)/100.0);\n"
"    printf(\"  Accel      : %.2f m/s^2\\n\", 9.8 + (rand()%20-10)/100.0);\n"
"    printf(\"  Battery    : %d%%\\n\", 65 + rand()%35);\n"
"    printf(\"  Temp       : %.1f C\\n\", 35.0 + rand()%10);\n"
"    printf(\"[SCAN] Complete.\\n\");\n"
"}\n";

    const char *motion_file =
"#include <stdio.h>\n"
"#include <string.h>\n"
"#include \"aria.h\"\n"
"\n"
"void cmd_move(const char *dir) {\n"
"    if (!dir || !dir[0]) { printf(\"Directions: forward back left right stop spin\\n\"); return; }\n"
"    if (strstr(dir,\"forward\"))    printf(\"[DRIVE] Moving forward at 0.5 m/s\\n\");\n"
"    else if (strstr(dir,\"back\"))  printf(\"[DRIVE] Reversing at 0.3 m/s\\n\");\n"
"    else if (strstr(dir,\"left\"))  printf(\"[TURN] Rotating left 45 deg\\n\");\n"
"    else if (strstr(dir,\"right\")) printf(\"[TURN] Rotating right 45 deg\\n\");\n"
"    else if (strstr(dir,\"stop\"))  printf(\"[STOP] All motors halted\\n\");\n"
"    else if (strstr(dir,\"spin\"))  printf(\"[SPIN] 360-degree rotation\\n\");\n"
"    else                           printf(\"[MOVE] Unknown: %s\\n\", dir);\n"
"}\n";

    const char *ai_file =
"#include <stdio.h>\n"
"#include <string.h>\n"
"#include <stdlib.h>\n"
"#include <math.h>\n"
"#include <time.h>\n"
"#include \"aria.h\"\n"
"\n"
"void cmd_greet(void) {\n"
"    printf(\"Hello! I am ARIA v3.0 - Advanced Responsive Intelligence Agent.\\n\");\n"
"    printf(\"Multi-module systems nominal. All subsystems online.\\n\");\n"
"}\n"
"\n"
"void cmd_calculate(int argc, char **argv) {\n"
"    if (argc < 5) { printf(\"Usage: calculate <num> <op> <num>\\n\"); return; }\n"
"    double a = atof(argv[2]), b = atof(argv[4]);\n"
"    char op = argv[3][0];\n"
"    double res = 0; int ok = 1;\n"
"    switch(op) {\n"
"        case '+': res = a + b; break;\n"
"        case '-': res = a - b; break;\n"
"        case '*': res = a * b; break;\n"
"        case '/': if (b==0){printf(\"  Error: division by zero\\n\");return;} res=a/b; break;\n"
"        case '^': res = pow(a,b); break;\n"
"        default:  ok = 0;\n"
"    }\n"
"    if (ok) printf(\"  %g %c %g = %g\\n\", a, op, b, res);\n"
"    else    printf(\"  Unknown operator '%c'. Use: + - * / ^\\n\", op);\n"
"}\n"
"\n"
"void cmd_say(int argc, char **argv) {\n"
"    if (argc < 3) { printf(\"Usage: say <message>\\n\"); return; }\n"
"    printf(\"[ARIA]: \");\n"
"    for (int i = 2; i < argc; i++) { if(i>2) printf(\" \"); printf(\"%s\", argv[i]); }\n"
"    printf(\"\\n\");\n"
"}\n"
"\n"
"void cmd_time(void) {\n"
"    time_t t = time(NULL); struct tm *ti = localtime(&t); char buf[64];\n"
"    strftime(buf, sizeof(buf), \"%Y-%m-%d %H:%M:%S\", ti);\n"
"    printf(\"[ARIA CLOCK] %s\\n\", buf);\n"
"}\n"
"\n"
"void cmd_memory(void) {\n"
"    FILE *f = fopen(\"/proc/meminfo\", \"r\");\n"
"    if (!f) { printf(\"  Memory info unavailable.\\n\"); return; }\n"
"    char line[256]; int shown = 0;\n"
"    while (fgets(line, sizeof(line), f) && shown < 3) {\n"
"        if (!strncmp(line,\"MemTotal\",8)||!strncmp(line,\"MemFree\",7)||!strncmp(line,\"MemAvail\",8))\n"
"            { printf(\"  %s\", line); shown++; }\n"
"    }\n"
"    fclose(f);\n"
"}\n";

    const char *main_file =
"#include <stdio.h>\n"
"#include <string.h>\n"
"#include \"aria.h\"\n"
"\n"
"int main(int argc, char **argv) {\n"
"    if (argc < 2) { printf(\"Commands: greet scan calculate move say time memory\\n\"); return 1; }\n"
"    const char *cmd = argv[1];\n"
"    if      (!strcmp(cmd,\"greet\"))     cmd_greet();\n"
"    else if (!strcmp(cmd,\"scan\"))      cmd_scan();\n"
"    else if (!strcmp(cmd,\"calculate\")) cmd_calculate(argc, argv);\n"
"    else if (!strcmp(cmd,\"move\"))      cmd_move(argc>2?argv[2]:\"\");\n"
"    else if (!strcmp(cmd,\"say\"))       cmd_say(argc, argv);\n"
"    else if (!strcmp(cmd,\"time\"))      cmd_time();\n"
"    else if (!strcmp(cmd,\"memory\"))    cmd_memory();\n"
"    else { printf(\"Unknown: %s\\nTry: greet scan calculate move say time memory\\n\",cmd); return 1; }\n"
"    return 0;\n"
"}\n";

    struct { const char *name; const char *content; } files[] = {
        {"ARIA.robot",              robot_file},
        {"ARIA_project/aria.h",     header_file},
        {"ARIA_project/sensors.c",  sensors_file},
        {"ARIA_project/motion.c",   motion_file},
        {"ARIA_project/ai.c",       ai_file},
        {"ARIA_project/main.c",     main_file},
    };
    for (int i = 0; i < (int)(sizeof(files)/sizeof(files[0])); i++) {
        FILE *fout = fopen(files[i].name, "w");
        if (!fout) { fprintf(stderr, "Cannot create %s\n", files[i].name); continue; }
        fputs(files[i].content, fout); fclose(fout);
        printf("  %s+%s %s\n", GR, R, files[i].name);
    }
    printf("%s%s[OK]%s Example project created.\n", GR, B, R);
    printf("  %sProject layout:%s\n", DM, R);
    printf("    ARIA.robot           <- robot definition\n");
    printf("    ARIA_project/\n");
    printf("      aria.h             <- shared header\n");
    printf("      main.c             <- entry point\n");
    printf("      sensors.c          <- sensor module\n");
    printf("      motion.c           <- motion module\n");
    printf("      ai.c               <- AI/logic module\n");
    printf("  %sRun: boots init ARIA.robot%s\n", DM, R);
}

static void usage(const char *prog) {
    printf("%s", BANNER);
    printf("%s%sUsage:%s %s <command> [args]\n\n", B, CY, R, prog);
    printf("%s%sCommands:%s\n", B, YL, R);
    printf("  %sinit%s     <file.robot>   Compile and initialize a robot\n", GR, R);
    printf("  %sinteract%s <file.robot>   Start interactive session\n", GR, R);
    printf("  %sinfo%s     <file.robot>   Show robot specs and project layout\n", GR, R);
    printf("  %slist%s                    List all initialized robots\n", GR, R);
    printf("  %sremove%s   <robot_name>   Remove robot from cache\n", GR, R);
    printf("  %sexample%s                 Generate multi-file ARIA example project\n", GR, R);
    printf("  %shelp%s                    Show this help\n\n", GR, R);
    printf("%s%s.robot File - Multi-file Options:%s\n", B, MG, R);
    printf("  srcdir   = ./src      auto-collect all source files from directory\n");
    printf("  main     = main.py    designate the Python entry point\n");
    printf("  flags    = -lpthread  extra compiler/linker flags\n\n");
    printf("  %s[files]%s             explicit file list (relative to .robot file):\n", YL, R);
    printf("    src/main.c\n");
    printf("    src/utils.c\n");
    printf("    lib/sensor.c\n\n");
    printf("  %s[code:filename.c]%s   inline a named file inside the .robot:\n", YL, R);
    printf("    (C / C++ / Python source for filename.c)\n\n");
    printf("  %s[code]%s              single-file inline (original syntax, still supported)\n\n", YL, R);
    printf("%sVersion: %s%s\n", DM, VERSION, R);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 0; }
    const char *cmd = argv[1];
    if (!strcmp(cmd,"help") || !strcmp(cmd,"--help") || !strcmp(cmd,"-h")) { usage(argv[0]); return 0; }
    if (!strcmp(cmd,"list"))    { do_list(); return 0; }
    if (!strcmp(cmd,"example")) { do_example(); return 0; }
    if (argc < 3) {
        fprintf(stderr, "%sError:%s '%s' requires an argument. See 'boots help'.\n", RD, R, cmd);
        return 1;
    }
    const char *arg = argv[2];
    if      (!strcmp(cmd,"init"))     do_init(arg);
    else if (!strcmp(cmd,"interact")) do_interact(arg);
    else if (!strcmp(cmd,"info"))     do_info(arg);
    else if (!strcmp(cmd,"remove"))   do_remove(arg);
    else {
        fprintf(stderr, "%sUnknown command: %s%s\nRun 'boots help'\n", RD, cmd, R);
        return 1;
    }
    return 0;
}