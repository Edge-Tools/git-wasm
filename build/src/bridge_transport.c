
#define _GNU_SOURCE
#include <git2.h>
#include <git2/sys/errors.h>
#include <git2/sys/transport.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BRIDGE_DEV "/dev/git-bridge"

typedef struct {
    git_smart_subtransport parent;
    int fd;
        git_smart_subtransport_stream *current;
} bridge_subtransport;

typedef struct {
    git_smart_subtransport_stream parent;
    bridge_subtransport *owner;
    char *url;                     git_smart_service_t action;

        char *req_buf;
    size_t req_len;
    size_t req_cap;

        int sent;
    char *resp_body;
    size_t resp_len;
    size_t resp_pos;
} bridge_stream;

static int read_exact(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_exact(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static uint32_t le32_read(const unsigned char *p) {
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void le32_write(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char) v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static int json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (o + 6 >= cap) return -1;
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            out[o++] = '\\'; out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    return 0;
}

static char *build_meta(const char *method, const char *url,
                        const char *content_type, size_t *out_len) {
    char esc_url[4096];
    if (json_escape(url, esc_url, sizeof(esc_url)) < 0) return NULL;

    char *json = NULL;
    int n;
    if (content_type) {
        char esc_ct[256];
        if (json_escape(content_type, esc_ct, sizeof(esc_ct)) < 0) return NULL;
        n = asprintf(&json,
            "{\"method\":\"%s\",\"url\":\"%s\","
            "\"headers\":{\"Content-Type\":\"%s\",\"Accept\":\"*/*\","
            "\"User-Agent\":\"git/2.x git-wasm/libgit2\"}}",
            method, esc_url, esc_ct);
    } else {
        n = asprintf(&json,
            "{\"method\":\"%s\",\"url\":\"%s\","
            "\"headers\":{\"Accept\":\"*/*\","
            "\"User-Agent\":\"git/2.x git-wasm/libgit2\"}}",
            method, esc_url);
    }
    if (n < 0) return NULL;
    *out_len = (size_t)n;
    return json;
}

static long json_find_status(const char *json) {
    const char *p = strstr(json, "\"status\"");
    if (!p) return -1;
    p += 8;
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    return strtol(p, NULL, 10);
}

static int json_has_error(const char *json) {
    return strstr(json, "\"error\"") != NULL;
}

static int ensure_open(bridge_subtransport *bt) {
    if (bt->fd >= 0) return 0;
    bt->fd = open(BRIDGE_DEV, O_RDWR);
    if (bt->fd < 0) {
        git_error_set_str(GIT_ERROR_NET,
            "cannot open " BRIDGE_DEV
            " — no bridge host is installed. The JS host must register a "
            "character device at " BRIDGE_DEV " backed by SharedArrayBuffer "
            "+ Atomics.wait. See README.md.");
        return -1;
    }
    return 0;
}

static int do_request(bridge_stream *s) {
    if (ensure_open(s->owner) < 0) return -1;

    const char *method;
    const char *ctype = NULL;
    const char *suffix;
    switch (s->action) {
        case GIT_SERVICE_UPLOADPACK_LS:
            method = "GET";  suffix = "/info/refs?service=git-upload-pack";
            break;
        case GIT_SERVICE_UPLOADPACK:
            method = "POST"; ctype = "application/x-git-upload-pack-request";
            suffix = "/git-upload-pack";
            break;
        case GIT_SERVICE_RECEIVEPACK_LS:
            method = "GET";  suffix = "/info/refs?service=git-receive-pack";
            break;
        case GIT_SERVICE_RECEIVEPACK:
            method = "POST"; ctype = "application/x-git-receive-pack-request";
            suffix = "/git-receive-pack";
            break;
        default:
            git_error_set_str(GIT_ERROR_NET, "unsupported smart service");
            return -1;
    }

    char *full_url = NULL;
    if (asprintf(&full_url, "%s%s", s->url, suffix) < 0) return -1;

    size_t meta_len;
    char *meta = build_meta(method, full_url, ctype, &meta_len);
    free(full_url);
    if (!meta) return -1;

        size_t total = 4 + meta_len + 4 + s->req_len;
    unsigned char *frame = malloc(total);
    if (!frame) {
        free(meta);
        git_error_set_oom();
        return -1;
    }
    le32_write(frame, (uint32_t)meta_len);
    memcpy(frame + 4, meta, meta_len);
    le32_write(frame + 4 + meta_len, (uint32_t)s->req_len);
    if (s->req_len) memcpy(frame + 4 + meta_len + 4, s->req_buf, s->req_len);
    int ok = write_exact(s->owner->fd, frame, total) == 0;
    free(frame);
    free(meta);
    if (!ok) {
        git_error_set_str(GIT_ERROR_NET, "bridge write failed");
        return -1;
    }

        unsigned char rhdr[4];
    if (read_exact(s->owner->fd, rhdr, 4) < 0) {
        git_error_set_str(GIT_ERROR_NET, "bridge read failed (meta header)");
        return -1;
    }
    uint32_t rmeta_len = le32_read(rhdr);
    char *rmeta = malloc((size_t)rmeta_len + 1);
    if (!rmeta) return -1;
    if (read_exact(s->owner->fd, rmeta, rmeta_len) < 0) {
        free(rmeta);
        git_error_set_str(GIT_ERROR_NET, "bridge read failed (meta body)");
        return -1;
    }
    rmeta[rmeta_len] = 0;

    size_t body_cap = 65536;
    char *body = malloc(body_cap);
    if (!body) { free(rmeta); return -1; }
    size_t body_len = 0;
    for (;;) {
        if (body_cap - body_len < 4096) {
            size_t nc = body_cap * 2;
            char *nb = realloc(body, nc);
            if (!nb) {
                free(rmeta); free(body);
                git_error_set_str(GIT_ERROR_NET, "bridge read: out of memory");
                return -1;
            }
            body = nb;
            body_cap = nc;
        }
        ssize_t r = read(s->owner->fd, body + body_len, body_cap - body_len);
        if (r < 0) {
            free(rmeta); free(body);
            git_error_set_str(GIT_ERROR_NET, "bridge read failed (body)");
            return -1;
        }
        if (r == 0) break;          body_len += (size_t)r;
    }
    uint32_t rbody_len = (uint32_t)body_len;

    if (json_has_error(rmeta)) {
        char emsg[512];
        snprintf(emsg, sizeof(emsg), "bridge: %s", rmeta);
        git_error_set_str(GIT_ERROR_NET, emsg);
        free(rmeta); free(body);
        return -1;
    }
    long status = json_find_status(rmeta);
    if (status < 200 || status >= 300) {
        char emsg[512];
        snprintf(emsg, sizeof(emsg), "bridge: HTTP %ld for %s", status, s->url);
        git_error_set_str(GIT_ERROR_NET, emsg);
        free(rmeta); free(body);
        return -1;
    }
    free(rmeta);

    s->resp_body = body;
    s->resp_len = rbody_len;
    s->resp_pos = 0;
    s->sent = 1;
    return 0;
}

static int bridge_stream_read(git_smart_subtransport_stream *stream,
                              char *buffer, size_t buf_size, size_t *bytes_read) {
    bridge_stream *s = (bridge_stream *)stream;
    if (!s->sent) {
        if (do_request(s) < 0) return -1;
    }
    size_t left = s->resp_len - s->resp_pos;
    size_t n = left < buf_size ? left : buf_size;
    if (n) memcpy(buffer, s->resp_body + s->resp_pos, n);
    s->resp_pos += n;
    *bytes_read = n;
    return 0;
}

static int bridge_stream_write(git_smart_subtransport_stream *stream,
                               const char *buffer, size_t len) {
    bridge_stream *s = (bridge_stream *)stream;
    if (s->req_len + len > s->req_cap) {
        size_t nc = s->req_cap ? s->req_cap : 4096;
        while (nc < s->req_len + len) nc *= 2;
        char *nb = realloc(s->req_buf, nc);
        if (!nb) return -1;
        s->req_buf = nb;
        s->req_cap = nc;
    }
    memcpy(s->req_buf + s->req_len, buffer, len);
    s->req_len += len;
    return 0;
}

static void bridge_stream_free(git_smart_subtransport_stream *stream) {
    bridge_stream *s = (bridge_stream *)stream;
        if (s->owner && s->owner->current == stream) s->owner->current = NULL;
    free(s->req_buf);
    free(s->resp_body);
    free(s->url);
    free(s);
}

static int bridge_action(git_smart_subtransport_stream **out,
                         git_smart_subtransport *transport,
                         const char *url,
                         git_smart_service_t action) {
    bridge_subtransport *bt = (bridge_subtransport *)transport;

    if (bt->current) {
        bridge_stream_free(bt->current);
        bt->current = NULL;
    }

    bridge_stream *s = calloc(1, sizeof(*s));
    if (!s) return -1;
    s->parent.subtransport = transport;
    s->parent.read = bridge_stream_read;
    s->parent.write = bridge_stream_write;
    s->parent.free = bridge_stream_free;
    s->owner = bt;
    s->url = strdup(url);
    if (!s->url) { free(s); return -1; }
    s->action = action;

    bt->current = (git_smart_subtransport_stream *)s;
    *out = bt->current;
    return 0;
}

static int bridge_close(git_smart_subtransport *transport) {
    bridge_subtransport *bt = (bridge_subtransport *)transport;
    if (bt->current) {
        bridge_stream_free(bt->current);
        bt->current = NULL;
    }
    if (bt->fd >= 0) {
        close(bt->fd);
        bt->fd = -1;
    }
    return 0;
}

static void bridge_free(git_smart_subtransport *transport) {
    bridge_close(transport);
    free(transport);
}

static int bridge_subtransport_cb(git_smart_subtransport **out,
                                  git_transport *owner, void *param) {
    (void)owner; (void)param;
    bridge_subtransport *bt = calloc(1, sizeof(*bt));
    if (!bt) return -1;
    bt->parent.action = bridge_action;
    bt->parent.close = bridge_close;
    bt->parent.free = bridge_free;
    bt->fd = -1;
    *out = (git_smart_subtransport *)bt;
    return 0;
}

int register_bridge_transport(void) {
        static git_smart_subtransport_definition def = {
        bridge_subtransport_cb, 1, NULL
    };
    int rc = git_transport_register("http",  git_transport_smart, &def);
    if (rc < 0) return rc;
    return  git_transport_register("https", git_transport_smart, &def);
}
