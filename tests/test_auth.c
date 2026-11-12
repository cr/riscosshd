/* SPDX-License-Identifier: MIT */
/* Unit test for src/auth.c: parsing, duplicates, comments, add, remove.
 * Usage: test_auth <ed25519.pub> <rsa.pub> <workfile>
 * Built natively against the host's libssh by "make check". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libssh/libssh.h>
#include "auth.h"

static int failures;
#define CHECK(cond, what) do { if (cond) printf("ok   %s\n", what); \
                               else { printf("FAIL %s\n", what); failures++; } } while (0)

static const char *slurp(const char *path)
{
    static char buf[8192];
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f)
        return "";
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (n && buf[n - 1] == '\n')
        buf[n - 1] = '\0';
    return buf;
}

/* One line through auth_add_keys, as the Keys window adds it: 0, or -1
 * with the reason in err. */
static int add_line(const char *line, char *err, size_t errlen)
{
    char buf[2048];
    int added, dup, bad;
    snprintf(buf, sizeof buf, "%s", line);
    return auth_add_keys(buf, &added, &dup, &bad, err, errlen) < 0 || added == 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
    char key1[2048], key2[2048], err[128], desc[160];
    FILE *f;
    int n;

    if (argc != 4) {
        fprintf(stderr, "usage: test_auth ed25519.pub rsa.pub workfile\n");
        return 2;
    }
    snprintf(key1, sizeof key1, "%s", slurp(argv[1]));
    snprintf(key2, sizeof key2, "%s", slurp(argv[2]));

    /* A file with a comment line, a bad line, a key, and that key again. */
    f = fopen(argv[3], "w");
    fprintf(f, "# comment\nnot a key at all\n%s\n%s\n", key1, key1);
    fclose(f);
    n = auth_load_keys(argv[3]);
    CHECK(n == 1, "load: one key from a file with junk and a duplicate");

    CHECK(add_line(key1, err, sizeof err) < 0 && strstr(err, "already"),
          "add: duplicate refused");
    CHECK(add_line("ssh-ed25519 AAAA", err, sizeof err) < 0, "add: garbage refused");
    CHECK(add_line(key2, err, sizeof err) == 0 && auth_count() == 2, "add: second key accepted");

    auth_describe_one(1, desc, sizeof desc);
    CHECK(strstr(desc, "  ") == NULL && desc[0] != ' ', "describe: no doubled or leading space");
    {
        const char *at = strchr(key2, ' ');
        at = at ? strchr(at + 1, ' ') : NULL;         /* comment starts after the second space */
        CHECK(at == NULL || strstr(desc, at + 1) != NULL, "describe: comment carried through");
    }

    CHECK(auth_remove_key(0, err, sizeof err) == 0 && auth_count() == 1, "remove: first key");
    {
        const char *data = slurp(argv[3]);
        CHECK(strstr(data, key2) && !strstr(data, key1), "remove: file rewritten with the survivor only");
    }
    CHECK(auth_remove_key(5, err, sizeof err) < 0, "remove: bad index refused");

    auth_free();
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
