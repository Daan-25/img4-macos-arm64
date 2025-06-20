#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "libvfs/vfs.h"

#define FOURCC(tag) (unsigned char)((tag) >> 24), (unsigned char)((tag) >> 16), (unsigned char)((tag) >> 8), (unsigned char)(tag)

static int
str2hex(int buflen, unsigned char *buf, const char *str)
{
    unsigned char *ptr = buf;
    int seq = -1;
    while (buflen > 0) {
        int nibble = *str++;
        if (nibble >= '0' && nibble <= '9') {
            nibble -= '0';
        } else {
            nibble |= 0x20;
            if (nibble < 'a' || nibble > 'f') {
                break;
            }
            nibble -= 'a' - 10;
        }
        if (seq >= 0) {
            *buf++ = (seq << 4) | nibble;
            buflen--;
            seq = -1;
        } else {
            seq = nibble;
        }
    }
    return buf - ptr;
}

static int
read_file_silent(const char *name, unsigned char **buf, size_t *size)
{
    void *p;
    size_t n, sz;
    FHANDLE in = file_open(name, O_RDONLY);
    if (!in) {
        return -1;
    }
    sz = in->length(in);
    if ((ssize_t)sz < 0) {
        in->close(in);
        return -1;
    }
    p = malloc(sz);
    if (!p) {
        in->close(in);
        return -1;
    }
    n = in->read(in, p, sz);
    in->close(in);
    if (n != sz) {
        free(p);
        return -1;
    }
    *buf = p;
    *size = sz;
    return 0;
}

static int
read_file(const char *name, unsigned char **buf, size_t *size)
{
    int rv = read_file_silent(name, buf, size);
    if (rv) {
        fprintf(stderr, "[e] cannot read '%s'\n", name);
    }
    return rv;
}

static int
write_file(const char *name, void *buf, size_t size)
{
    size_t written;
    FHANDLE out = file_open(name, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (!out) {
        fprintf(stderr, "[e] cannot write '%s'\n", name);
        return -1;
    }
    written = out->write(out, buf, size);
    if (written != size) {
        out->close(out);
        fprintf(stderr, "[e] cannot write '%s'\n", name);
        return -1;
    }
    return out->close(out);
}

typedef struct {
    size_t off;
    int skip;
    uint8_t ov, nv;
} PATCH;

static int
apply_patch(FHANDLE fd, const char *patchfile, int force, int undo)
{
    FILE *f;
    char buf[BUFSIZ];
    PATCH *patches = NULL;
    unsigned i, max = 0, len = 0;
    size_t length;
    int rv = 0;

    length = fd->length(fd);
    if ((ssize_t)length < 0) {
        return -1;
    }

    f = fopen(patchfile, "rt");
    if (!f) {
        fprintf(stderr, "[e] cannot read '%s'\n", patchfile);
        return -1;
    }

    while (fgets(buf, sizeof(buf), f)) {
        PATCH patch;
        char *p, *q = buf;
        p = strchr(buf, '\n');
        if (p == NULL) {
            fprintf(stderr, "[e] patch: malformed line\n");
            rv = -1;
            break;
        }
        if (p > buf && p[-1] == '\r') {
            p--;
        }
        *p = '\0';
        buf[strcspn(buf, "#;")] = '\0';
        if (buf[0] == '\0') {
            continue;
        }
        p = q;
        errno = 0;
        patch.off = strtoull(p, &q, 0);
        if (errno || p == q) {
            fprintf(stderr, "[e] patch: malformed line\n");
            rv = -1;
            break;
        }
        p = q;
        errno = 0;
        patch.ov = strtoul(p, &q, 0);
        if (errno || p == q) {
            fprintf(stderr, "[e] patch: malformed line\n");
            rv = -1;
            break;
        }
        p = q;
        errno = 0;
        patch.nv = strtoul(q, &q, 0);
        if (errno || p == q) {
            fprintf(stderr, "[e] patch: malformed line\n");
            rv = -1;
            break;
        }
        if (patch.off > length) {
            fprintf(stderr, "[e] patch: offset 0x%zx too big\n", patch.off);
            rv = -1;
            break;
        }
        if (len >= max) {
            PATCH *tmp;
            if (max == 0) {
                max = 8;
            }
            max *= 2;
            tmp = realloc(patches, max * sizeof(PATCH));
            if (!tmp) {
                fprintf(stderr, "[e] patch: out of memory\n");
                rv = -1;
                break;
            }
            patches = tmp;
        }
        patch.skip = 0;
        if (undo) {
            uint8_t tv = patch.ov;
            patch.ov = patch.nv;
            patch.nv = tv;
        }
        patches[len++] = patch;
    }

    fclose(f);

    if (rv) {
        free(patches);
        return rv;
    }

    for (i = 0; i < len; i++) {
        size_t n, off;
        unsigned char cv, nv, ov;
        PATCH *p = &patches[i];
        off = fd->lseek(fd, p->off, SEEK_SET);
        if (off != p->off) {
            fprintf(stderr, "[e] patch: cannot seek to 0x%zx\n", p->off);
            rv = -1;
            break;
        }
        n = fd->read(fd, &cv, 1);
        if (n != 1) {
            fprintf(stderr, "[e] patch: cannot read from 0x%zx\n", off);
            rv = -1;
            break;
        }
        nv = p->nv;
        ov = p->ov;
        if (cv != ov) {
            if (cv == nv) {
                fprintf(stderr, "[w] patch: offset 0x%zx is already patched: %02x\n", off, cv);
            } else {
                fprintf(stderr, "[w] patch: offset 0x%zx has %02x, expected %02x\n", off, cv, ov);
                if (!force) {
                    rv = -1;
                }
                break;
            }
        }
        p->skip = (cv == nv);
    }

    if (rv) {
        free(patches);
        return rv;
    }

    for (i = 0; i < len; i++) {
        size_t n, off;
        PATCH *p = &patches[i];
        if (p->skip) {
            continue;
        }
        off = fd->lseek(fd, p->off, SEEK_SET);
        if (off != p->off) {
            fprintf(stderr, "[e] patch: cannot seek to 0x%zx\n", p->off);
            rv = -1;
            break;
        }
        n = fd->write(fd, &p->nv, 1);
        if (n != 1) {
            fprintf(stderr, "[e] patch: cannot patch 0x%zx\n", off);
            rv = -1;
            break;
        }
    }

    free(patches);
    return rv;
}

static FHANDLE
make_img4(const char *iname, FHANDLE *orig)
{
    static const unsigned char stub[] = {
        0x30, 0x18, 0x16, 0x04, 0x49, 0x4d, 0x34, 0x50, 0x16, 0x04, 0x6e, 0x6f,
        0x6e, 0x65, 0x16, 0x07,  'U',  'n',  'k',  'n',  'o',  'w',  'n', 0x04,
        0x01, 0x00
    };
    size_t total;
    unsigned char *tmp, xfer[4096];
    FHANDLE fd, src = file_open(iname, O_RDONLY);
    if (!src) {
        return NULL;
    }
    total = src->length(src);
    tmp = malloc(sizeof(stub));
    if (!tmp) {
        src->close(src);
        return NULL;
    }
    memcpy(tmp, stub, sizeof(stub));
    *orig = memory_open(O_RDWR, tmp, sizeof(stub));
    if (*orig == NULL) {
        free(tmp);
        src->close(src);
        return NULL;
    }
    fd = img4_reopen(*orig, NULL, 0);
    if (fd) {
        fd->lseek(fd, 0, SEEK_SET);
        for (;;) {
            ssize_t n, written;
            n = src->read(src, xfer, sizeof(xfer));
            if (n <= 0) {
                break;
            }
            written = fd->write(fd, xfer, n);
            if (written != n) {
                break;
            }
            total -= written;
        }
        if (total) {
            fd->close(fd);
            fd = NULL;
        }
    }
    src->close(src);
    return fd;
}

static FHANDLE
replace_img4(const char *iname, const char *replacer, FHANDLE *orig)
{
    size_t total;
    unsigned char xfer[4096];
    FHANDLE fd, src = file_open(replacer, O_RDONLY);
    if (!src) {
        return NULL;
    }
    total = src->length(src);
    *orig = memory_open_from_file(iname, O_RDWR);
    if (*orig == NULL) {
        src->close(src);
        return NULL;
    }
    fd = img4_reopen(*orig, NULL, FLAG_IMG4_SKIP_DECOMPRESSION);
    if (fd) {
        fd->ftruncate(fd, total);
        fd->lseek(fd, 0, SEEK_SET);
        for (;;) {
            ssize_t n, written;
            n = src->read(src, xfer, sizeof(xfer));
            if (n <= 0) {
                break;
            }
            written = fd->write(fd, xfer, n);
            if (written != n) {
                break;
            }
            total -= written;
        }
        if (total) {
            fd->close(fd);
            fd = NULL;
        }
    }
    src->close(src);
    return fd;
}

static void __attribute__((noreturn))
usage(const char *argv0)
{
    printf("usage: %s -i <input> [-o <output>] [-k <ivkey>] [GETTERS] [MODIFIERS]\n", argv0);
    printf("    -i <file>       read from <file>\n");
    printf("    -o <file>       write image to <file>\n");
    printf("    -k <ivkey>      use <ivkey> to decrypt\n");
    printf("    -z              operate on compressed data\n");
    printf("    --json          output information in JSON format\n");
    printf("getters:\n");
    printf("    -l              list all info\n");
    printf("    -w <file>       write watchtower to <file>\n");
    printf("    -g <file>       write keybag to <file>\n");
    printf("    -m <file>       write ticket to <file>\n");
    printf("    -e <file>       write epinfo to <file>\n");
    printf("    -c <info>       check signature with <info>\n");
    printf("    -q <prop>       query property\n");
    printf("    -f              check hash against manifest\n");
    printf("    -n              print nonce\n");
    printf("    -b              print kbags\n");
    printf("    -v              print version\n");
    printf("modifiers:\n");
    printf("    -T <fourcc>     set type <fourcc>\n");
    printf("    -P[f|u] <file>  apply patch from <file> (f=force, u=undo)\n");
    printf("    -W <file>       set watchtower from <file>\n");
    printf("    -M <file>       set ticket from <file>\n");
    printf("    -E <file>       set epinfo from <file>\n");
    printf("    -N <nonce>      set <nonce> if ticket is set/present\n");
    printf("    -V <version>    set <version>\n");
    printf("    -R <file>       replace payload\n");
    printf("    -G <file>       set keybag from file (internal use only)\n");
    printf("    -B <bag> <bag>  create keybag (internal use only)\n");
    printf("    -F              update payload hash in manifest\n");
    printf("    -D              leave IMG4 decrypted\n");
    printf("    -J              convert lzfse to lzss\n");
    printf("    -U              convert lzfse to plain\n");
    printf("    -A              treat input as plain file and wrap it up into ASN.1\n");
    printf("note: if no modifier is present and -o is specified, extract the bare image\n");
    printf("note: if modifiers are present and -o is not specified, modify the input file\n");
    printf("note: sigcheck info is: \"CHIP=0x8960,ECID=0x1122334455667788[,...]\"\n");
    exit(0);
}

int
main(int argc, char **argv)
{
    const char *argv0 = argv[0];
    const char *iname = NULL;
    const char *oname = NULL;
    const char *ik = NULL;
    const char *wname = NULL;
    const char *gname = NULL;
    const char *mname = NULL;
    const char *ename = NULL;
    const char *query = NULL;
    char *cinfo = NULL;
    int list_only = 0;
    int get_nonce = 0;
    int get_kbags = 0;
    int get_version = 0;
    const char *set_type = NULL;
    const char *set_patch = NULL;
    int pf = 0;
    int pu = 0;
    const char *set_wtower = NULL;
    const char *set_manifest = NULL;
    const char *set_version = NULL;
    const char *set_replacer = NULL;
    const char *set_epinfo = NULL;
    const char *set_kb1 = NULL;
    const char *set_kb2 = NULL;
    int set_nonce = 0;
    uint64_t nonce = 0;
    const char *set_keybag = NULL;
    int set_decrypt = 0;
    int set_convert = 0;
    int set_wrap = 0;
    int img4flags = 0;

    bool json_output = false;

    int rv, rc = 0;
    unsigned char *buf;
    size_t sz;
    int modify;
    unsigned type;
    FHANDLE fd, orig = NULL;
    unsigned char *k, ivkey[16 + 32], kb1[16 + 32], kb2[16 + 32];

    while (--argc > 0) {
        const char *arg = *++argv;
        if (strcmp(arg, "--json") == 0) {
            json_output = true;
            continue;
        }
        if (*arg == '-') switch (arg[1]) {
            case 'h':
                usage(argv0);
                continue;
            case 'l':
                list_only = 1;
                img4flags |= FLAG_IMG4_SKIP_DECOMPRESSION;
                continue;
            case 'n':
                get_nonce = 1;
                continue;
            case 'b':
                get_kbags = 1;
                continue;
            case 'v':
                get_version = 1;
                continue;
            case 'D':
                set_decrypt = 1;
                continue;
            case 'J':
                set_convert = 1;
                continue;
            case 'U':
                set_convert = -1;
                continue;
            case 'A':
                set_wrap = 1;
                continue;
            case 'z':
                img4flags |= FLAG_IMG4_SKIP_DECOMPRESSION;
                continue;
            case 'f':
                img4flags |= FLAG_IMG4_VERIFY_HASH;
                continue;
            case 'F':
                img4flags |= FLAG_IMG4_UPDATE_HASH;
                continue;
            case 'i':
                if (argc >= 2) { iname = *++argv; argc--; continue; }
            case 'o':
                if (argc >= 2) { oname = *++argv; argc--; continue; }
            case 'k':
                if (argc >= 2) { ik = *++argv; argc--; continue; }
            case 'w':
                if (argc >= 2) { wname = *++argv; argc--; continue; }
            case 'g':
                if (argc >= 2) { gname = *++argv; argc--; continue; }
            case 'm':
                if (argc >= 2) { mname = *++argv; argc--; continue; }
            case 'c':
                if (argc >= 2) { cinfo = *++argv; argc--; continue; }
            case 'e':
                if (argc >= 2) { ename = *++argv; argc--; continue; }
            case 'q':
                if (argc >= 2) { query = *++argv; argc--; continue; }
            case 'T':
                if (argc >= 2) { set_type = *++argv; argc--; continue; }
            case 'P':
                if (argc >= 2) { set_patch = *++argv; argc--; pf = (!!strchr(arg, 'f')); pu = (!!strchr(arg, 'u')); continue; }
            case 'W':
                if (argc >= 2) { set_wtower = *++argv; argc--; continue; }
            case 'M':
                if (argc >= 2) { set_manifest = *++argv; argc--; continue; }
            case 'E':
                if (argc >= 2) { set_epinfo = *++argv; argc--; continue; }
            case 'N':
                if (argc >= 2) { set_nonce = 1; nonce = strtoull(*++argv, NULL, 16); argc--; continue; }
            case 'V':
                if (argc >= 2) { set_version = *++argv; argc--; continue; }
            case 'R':
                if (argc >= 2) { set_replacer = *++argv; argc--; continue; }
            case 'G':
                if (argc >= 2) { set_keybag = *++argv; argc--; continue; }
            case 'B':
                if (argc >= 3) { set_kb1 = *++argv; argc--; set_kb2 = *++argv; argc--; continue; }
                fprintf(stderr, "[e] argument to '%s' is missing\n", arg);
                return -1;
            default:
                fprintf(stderr, "[e] illegal option '%s'\n", arg);
                return -1;
        }
        if (!oname) {
            oname = arg;
        } else {
            ik = arg;
        }
    }

    if (!iname) {
        fprintf(stderr, "[e] no input file name\n");
        return -1;
    }

    modify = set_type || set_patch || set_wtower || set_manifest || set_nonce || set_decrypt || set_convert || set_version || set_wrap || set_kb1 || set_keybag || set_replacer || set_epinfo || (img4flags & FLAG_IMG4_UPDATE_HASH);

    k = (unsigned char *)ik;
    if (ik) {
        if (str2hex(sizeof(ivkey), ivkey, ik) != sizeof(ivkey)) {
            fprintf(stderr, "[e] invalid ivkey\n");
            return -1;
        }
        k = ivkey;
    }
    if (set_kb1 && (!set_kb2 || str2hex(16 + 32, kb1, set_kb1) != 16 + 32 || str2hex(16 + 32, kb2, set_kb2) != 16 + 32)) {
        fprintf(stderr, "[e] invalid keybags\n");
        return -1;
    }

    // open
    if (!modify || list_only || get_nonce || get_kbags || get_version || query) {
        fd = img4_reopen(file_open(iname, O_RDONLY), k, img4flags);
    } else if (set_wrap) {
        if (!oname) oname = iname;
        fd = make_img4(iname, &orig);
    } else if (set_replacer) {
        if (!oname) oname = iname;
        fd = replace_img4(iname, set_replacer, &orig);
    } else if (!oname) {
        fd = img4_reopen(file_open(iname, O_RDWR), k, img4flags);
    } else {
        fd = img4_reopen(orig = memory_open_from_file(iname, O_RDWR), k, img4flags);
    }

    if (!fd) {
        fprintf(stderr, "[e] cannot open '%s'\n", iname);
        return -1;
    }

    // get stuff
    rv = fd->ioctl(fd, IOCTL_IMG4_GET_TYPE, &type);
    if (rv) {
        fprintf(stderr, "[e] cannot identify\n");
        fd->close(fd);
        return -1;
    }

    if (list_only) {
        if (json_output) {
            const char *separator = "";
            printf("{");

            printf("%s\"type\": \"%c%c%c%c\"", separator, FOURCC(type));
            separator = ", ";

            if (fd->ioctl(fd, IOCTL_IMG4_GET_VERSION, &buf, &sz) == 0) {
                printf("%s\"version\": \"%.*s\"", separator, (int)sz, buf);
            }

            if (fd->ioctl(fd, IOCTL_MEM_GET_BACKING, &buf, &sz) == 0 && sz) {
                 printf("%s\"data_size\": %zu", separator, sz);
            }
            
            unsigned char kbag1[48], kbag2[48];
            if (fd->ioctl(fd, IOCTL_IMG4_GET_KEYBAG2, kbag1, kbag2) == 0) {
                printf("%s\"keybags\": [\"", separator);
                for (unsigned i = 0; i < sizeof(kbag1); i++) printf("%02X", kbag1[i]);
                printf("\", \"");
                for (unsigned i = 0; i < sizeof(kbag2); i++) printf("%02X", kbag2[i]);
                printf("\"]");
            }

            if (fd->ioctl(fd, IOCTL_IMG4_GET_MANIFEST, &buf, &sz) == 0 && sz) {
                printf("%s\"manifest_size\": %zu", separator, sz);
            }
            
            if (fd->ioctl(fd, IOCTL_IMG4_GET_EP_INFO, &buf, &sz) == 0 && sz) {
                 printf("%s\"epinfo_size\": %zu", separator, sz);
            }

            uint64_t list_nonce = 0;
            if (fd->ioctl(fd, IOCTL_IMG4_GET_NONCE, &list_nonce) == 0) {
                printf("%s\"nonce\": \"0x%016llx\"", separator, list_nonce);
            }

            printf("}\n");

        } else {
            printf("type -> %c%c%c%c\n", FOURCC(type));
            if (fd->ioctl(fd, IOCTL_IMG4_GET_VERSION, &buf, &sz) == 0) {
                printf("version -> %.*s\n", (int)sz, buf);
            }
            if (fd->ioctl(fd, IOCTL_MEM_GET_BACKING, &buf, &sz) == 0 && sz) {
                printf("DATA %zu\n", sz);
            }
            unsigned char kbag1[48], kbag2[48];
            if (fd->ioctl(fd, IOCTL_IMG4_GET_KEYBAG2, kbag1, kbag2) == 0) {
                unsigned i;
                printf("kbag1 -> "); for (i = 0; i < sizeof(kbag1); i++) printf("%02X", kbag1[i]); printf("\n");
                printf("kbag2 -> "); for (i = 0; i < sizeof(kbag2); i++) printf("%02X", kbag2[i]); printf("\n");
            }
            if (fd->ioctl(fd, IOCTL_IMG4_GET_MANIFEST, &buf, &sz) == 0 && sz) {
                printf("IM4M.der %zu\n", sz);
            }
            if (fd->ioctl(fd, IOCTL_IMG4_GET_EP_INFO, &buf, &sz) == 0 && sz) {
                printf("INFO.der %zu\n", sz);
            }
            uint64_t list_nonce = 0;
            if (fd->ioctl(fd, IOCTL_IMG4_GET_NONCE, &list_nonce) == 0) {
                printf("nonce -> 0x%016llx\n", list_nonce);
            }
        }
        return fd->close(fd);
    }
    
    // Niet-list-only getters
    if (!get_nonce && !get_kbags && !get_version && !query) {
        if (!json_output) {
             printf("%c%c%c%c\n", FOURCC(type));
        }
    }

    if (wname) {  }
    if (gname) {  }
    if (mname) {  }
    if (ename) {  }
    if (cinfo) {  }

    if (query) {
        unsigned char result[256];
        unsigned int i, len = sizeof(result);
        rv = fd->ioctl(fd, IOCTL_IMG4_QUERY_PROP, query, result, &len);
        if (rv) {
            fprintf(stderr, "[e] query failed\n");
        } else {
            if (json_output) {
                printf("{\"property\": \"%s\", \"value\": \"0x", query);
                for (i = 0; i < len; i++) printf("%02x", result[i]);
                printf("\"}\n");
            } else {
                for (i = 0; i < len; i++) printf("%02x", result[i]);
                printf("\n");
            }
        }
        rc |= rv;
    }

    if (get_nonce) {
        uint64_t gnonce = 0;
        rv = fd->ioctl(fd, IOCTL_IMG4_GET_NONCE, &gnonce);
        if (rv == 0) {
            if (json_output) {
                printf("{\"nonce\": \"0x%016llx\"}\n", gnonce);
            } else {
                printf("0x%016llx\n", gnonce);
            }
        }
    }
    
    if (get_kbags) {
        unsigned char kbag1[48], kbag2[48];
        rv = fd->ioctl(fd, IOCTL_IMG4_GET_KEYBAG2, kbag1, kbag2);
        if (rv) {
            fprintf(stderr, "[e] cannot get keybag\n");
        } else {
            if (json_output) {
                printf("{\"keybags\": [\"");
                for (unsigned i = 0; i < sizeof(kbag1); i++) printf("%02X", kbag1[i]);
                printf("\", \"");
                for (unsigned i = 0; i < sizeof(kbag2); i++) printf("%02X", kbag2[i]);
                printf("\"]}\n");
            } else {
                unsigned i;
                for (i = 0; i < sizeof(kbag1); i++) printf("%02X", kbag1[i]); printf("\n");
                for (i = 0; i < sizeof(kbag2); i++) printf("%02X", kbag2[i]); printf("\n");
            }
        }
    }

    if (get_version) {
        char *version;
        rv = fd->ioctl(fd, IOCTL_IMG4_GET_VERSION, &version, &sz);
        if (rv == 0) {
            if (json_output) {
                printf("{\"version\": \"%.*s\"}\n", (int)sz, version);
            } else {
                printf("%.*s\n", (int)sz, version);
            }
        }
    }

    if (set_type) {
        if (strlen(set_type) != 4) {
            fprintf(stderr, "[e] invalid type '%s'\n", set_type);
            rv = -1;
        } else {
            type = (set_type[0] << 24) | (set_type[1] << 16) | (set_type[2] << 8) | set_type[3];
            rv = fd->ioctl(fd, IOCTL_IMG4_SET_TYPE, type);
            if (rv) {
                fprintf(stderr, "[e] cannot set type\n");
            }
        }
        rc |= rv;
    }
    if (set_patch) {
        rv = apply_patch(fd, set_patch, pf, pu);
        if (rv) {
            fprintf(stderr, "[e] cannot apply patch\n");
        }
        rc |= rv;
    }
    if (set_wtower) {
        rv = read_file(set_wtower, &buf, &sz);
        if (rv == 0) {
            rv = fd->ioctl(fd, IOCTL_LZSS_SET_WTOWER, buf, sz);
            if (rv) {
                fprintf(stderr, "[e] cannot set watchtower\n");
            }
            free(buf);
        }
        rc |= rv;
    }
    if (set_manifest) {
        rv = read_file(set_manifest, &buf, &sz);
        if (rv == 0) {
            rv = fd->ioctl(fd, IOCTL_IMG4_SET_MANIFEST, buf, sz);
            if (rv) {
                fprintf(stderr, "[e] cannot set manifest\n");
            }
            free(buf);
        }
        rc |= rv;
    }
    if (set_epinfo) {
        rv = read_file(set_epinfo, &buf, &sz);
        if (rv == 0) {
            rv = fd->ioctl(fd, IOCTL_IMG4_SET_EP_INFO, buf, sz);
            if (rv) {
                fprintf(stderr, "[e] cannot set ep info\n");
            }
            free(buf);
        }
        rc |= rv;
    }
    if (set_nonce) {
        rv = fd->ioctl(fd, IOCTL_IMG4_SET_NONCE, nonce);
        if (rv) {
            fprintf(stderr, "[e] cannot set nonce 0x%16llx\n", nonce);
        }
        rc |= rv;
    }
    if (set_version) {
        rv = fd->ioctl(fd, IOCTL_IMG4_SET_VERSION, set_version, strlen(set_version));
        if (rv) {
            fprintf(stderr, "[e] cannot set version %s\n", set_version);
        }
        rc |= rv;
    }
    if (set_decrypt) {
        rv = fd->ioctl(fd, IOCTL_ENC_SET_NOENC);
        if (rv) {
            fprintf(stderr, "[e] cannot set noenc\n");
        }
        rc |= rv;
    }
    if (set_convert == 1) {
        rv = fd->ioctl(fd, IOCTL_LZFSE_SET_LZSS);
        if (rv) {
            fprintf(stderr, "[e] cannot set convert\n");
        }
        rc |= rv;
    }
    if (set_convert == -1) {
        rv = fd->ioctl(fd, IOCTL_LZFSE_SET_NOCOMP);
        if (rv) {
            fprintf(stderr, "[e] cannot set convert\n");
        }
        rc |= rv;
    }
    if (set_kb1) {
        rv = fd->ioctl(fd, IOCTL_IMG4_SET_KEYBAG2, kb1, kb2);
        if (rv) {
            fprintf(stderr, "[e] cannot set keybag\n");
        }
        rc |= rv;
    }
    if (set_keybag) {
        rv = read_file(set_keybag, &buf, &sz);
        if (rv == 0) {
            rv = fd->ioctl(fd, IOCTL_IMG4_SET_KEYBAG, buf, sz);
            if (rv) {
                fprintf(stderr, "[e] cannot set keybag\n");
            }
            free(buf);
        }
        rc |= rv;
    }


    if (orig) {
        rv = fd->fsync(fd);
        if (rv) {
            fprintf(stderr, "[e] cannot reassemble data\n");
        } else {
            rv = orig->ioctl(orig, IOCTL_MEM_GET_DATAPTR, &buf, &sz);
            if (rv) {
                fprintf(stderr, "[e] cannot retrieve data\n");
            } else {
                rv = write_file(oname, buf, sz);
            }
        }
        rc |= rv;
    } else if (oname && modify) {
        rv = fd->fsync(fd);
        if(rv) {
            fprintf(stderr, "[e] failed to save modified file\n");
        }
        rc |= rv;
    } else if (oname) {
        rv = fd->ioctl(fd, IOCTL_MEM_GET_DATAPTR, &buf, &sz);
        if (rv) {
            fprintf(stderr, "[e] cannot retrieve data\n");
        } else {
            rv = write_file(oname, buf, sz);
        }
        rc |= rv;
    }

    return rc | fd->close(fd);
}