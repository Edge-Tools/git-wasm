
#include <git2.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int register_bridge_transport(void);

static int cmd_version(void) {
    int major, minor, rev;
    git_libgit2_version(&major, &minor, &rev);
        printf("git version %d.%d.%d (libgit2/wasm)\n", major, minor, rev);
    return 0;
}

static int die_libgit2(const char *action) {
    const git_error *e = git_error_last();
    fprintf(stderr, "%s failed: %s\n", action, e && e->message ? e->message : "unknown");
    return 1;
}

static int cmd_init(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "usage: git init <dir>\n");
        return 2;
    }
    git_repository *repo = NULL;
    if (git_repository_init(&repo, argv[0], 0) != 0) return die_libgit2("init");
    git_repository_free(repo);
    printf("Initialized empty Git repository in %s/.git/\n", argv[0]);
    return 0;
}

static int clone_progress(const char *str, int len, void *payload) {
    (void)payload;
    fprintf(stderr, "remote: %.*s", len, str);
    return 0;
}

static int cmd_clone(int argc, char **argv) {
        int depth = 0;
    while (argc > 0 && argv[0][0] == '-' && argv[0][1] != 0) {
        if (strcmp(argv[0], "--depth") == 0 && argc >= 2) {
            char *endp;
            long d = strtol(argv[1], &endp, 10);
            if (*endp != 0 || d < 0 || d > INT_MAX) {
                fprintf(stderr, "invalid --depth: %s\n", argv[1]);
                return 2;
            }
            depth = (int)d;
            argc -= 2; argv += 2;
        } else if (strcmp(argv[0], "--") == 0) {
            argc -= 1; argv += 1;
            break;
        } else {
            fprintf(stderr, "unknown clone option: %s\n", argv[0]);
            return 2;
        }
    }

    if (argc < 1) {
        fprintf(stderr, "usage: git clone [--depth <n>] <url> [<dir>]\n");
        return 2;
    }
    const char *url = argv[0];
    const char *dir;
    char autodir[256];
    if (argc >= 2) {
        dir = argv[1];
    } else {
        const char *slash = strrchr(url, '/');
        const char *base = slash ? slash + 1 : url;
        size_t n = strlen(base);
        if (n > 4 && strcmp(base + n - 4, ".git") == 0) n -= 4;
        if (n == 0 || n >= sizeof(autodir)) {
            fprintf(stderr, "cannot infer directory name from %s\n", url);
            return 2;
        }
        memcpy(autodir, base, n);
        autodir[n] = 0;
        dir = autodir;
    }

    git_clone_options opts = GIT_CLONE_OPTIONS_INIT;
    opts.fetch_opts.callbacks.sideband_progress = clone_progress;
    if (depth > 0) opts.fetch_opts.depth = depth;

    git_repository *repo = NULL;
    if (git_clone(&repo, url, dir, &opts) != 0) return die_libgit2("clone");
    git_repository_free(repo);
    printf("Cloned %s into %s\n", url, dir);
    return 0;
}

static int cmd_selftest(void) {
    int fd = open("/dev/git-bridge", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "selftest: /dev/git-bridge not available\n");
        return 1;
    }

        static const char meta[] =
        "{\"method\":\"GET\",\"url\":\"https://test.example/echo\","
        "\"headers\":{\"Accept\":\"*/*\"}}";
    size_t meta_len = sizeof(meta) - 1;
    unsigned char hdr[4] = {
        (unsigned char) meta_len,
        (unsigned char)(meta_len >> 8),
        (unsigned char)(meta_len >> 16),
        (unsigned char)(meta_len >> 24),
    };
    if (write(fd, hdr, 4) != 4) { close(fd); return 1; }
    if (write(fd, meta, meta_len) != (ssize_t)meta_len) { close(fd); return 1; }
    unsigned char zero[4] = {0,0,0,0};
    if (write(fd, zero, 4) != 4) { close(fd); return 1; }

        unsigned char rh[4];
    if (read(fd, rh, 4) != 4) { close(fd); return 1; }
    uint32_t rmeta_len = (uint32_t)rh[0]
                       | ((uint32_t)rh[1] << 8)
                       | ((uint32_t)rh[2] << 16)
                       | ((uint32_t)rh[3] << 24);
    char *rmeta = malloc((size_t)rmeta_len + 1);
    if (!rmeta) { close(fd); return 1; }
    if (read(fd, rmeta, rmeta_len) != (ssize_t)rmeta_len) { free(rmeta); close(fd); return 1; }
    rmeta[rmeta_len] = 0;
    fprintf(stderr, "selftest meta: %s\n", rmeta);
    free(rmeta);

        char buf[4096];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r < 0) { close(fd); return 1; }
        if (r == 0) break;
        fwrite(buf, 1, (size_t)r, stdout);
    }
    fputc('\n', stdout);
    close(fd);
    return 0;
}

static int cmd_help(void) {
    fputs(
        "usage: git <command> [<args>]\n"
        "\n"
        "  --version, version    print libgit2 version\n"
        "  init <dir>            create an empty repository\n"
        "  clone [--depth N] <url> [<dir>]\n"
        "                        clone a remote repository via the bridge\n"
        "  selftest              exercise the /dev/git-bridge channel\n",
        stderr);
    return 0;
}

int main(int argc, char **argv) {
    if (git_libgit2_init() < 0) return die_libgit2("libgit2 init");

    if (register_bridge_transport() < 0) {
        fprintf(stderr, "warning: bridge transport not registered: %s\n",
                git_error_last() ? git_error_last()->message : "?");
    }

    int rc;
    if (argc < 2) {
        rc = cmd_help();
    } else if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "version") == 0) {
        rc = cmd_version();
    } else if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0) {
        rc = cmd_help();
    } else if (strcmp(argv[1], "init") == 0) {
        rc = cmd_init(argc - 2, argv + 2);
    } else if (strcmp(argv[1], "clone") == 0) {
        rc = cmd_clone(argc - 2, argv + 2);
    } else if (strcmp(argv[1], "selftest") == 0) {
        rc = cmd_selftest();
    } else {
        fprintf(stderr, "unknown command: %s\n", argv[1]);
        cmd_help();
        rc = 2;
    }

    git_libgit2_shutdown();
    return rc;
}
