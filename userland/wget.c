/*
 * userland/wget.c - File retriever using HTTP / HTTPS via netd gateway.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <intent.h>
#include <json.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: wget <url> [-O output_file]\n");
        return 1;
    }

    const char* url = NULL;
    const char* out_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-O") == 0 && i + 1 < argc) {
            out_file = argv[++i];
        } else if (!url) {
            url = argv[i];
        }
    }

    if (!url) {
        printf("Error: missing URL\n");
        return 1;
    }

    if (!out_file) {
        /* Derive filename from URL */
        const char* slash = strrchr(url, '/');
        out_file = (slash && *(slash + 1)) ? slash + 1 : "index.html";
    }

    printf("-- Connecting to network gateway for %s...\n", url);

    int ifd = intent_connect();
    if (ifd < 0) {
        fprintf(stderr, "wget: failed to connect to intent bus (is netd running?)\n");
        return 1;
    }

    char args[512];
    snprintf(args, sizeof(args), "{\"url\":\"%s\",\"method\":\"GET\"}", url);

    char res[262144];
    int r = intent_call(ifd, "net.http", args, res, sizeof(res), 10000);
    intent_close(ifd);

    if (r < 0) {
        fprintf(stderr, "wget: network request failed: %s\n", res);
        return 1;
    }

    json_doc_t doc;
    if (json_parse(&doc, res, strlen(res)) != JSON_ERR_OK || !doc.root) {
        fprintf(stderr, "wget: invalid response from netd\n");
        return 1;
    }

    int status = 0;
    json_obj_get_int(doc.root, "status", &status);
    const char* body = json_obj_get_str(doc.root, "body");
    int body_len = 0;
    json_obj_get_int(doc.root, "body_len", &body_len);

    printf("HTTP response: %d\n", status);
    if (status < 200 || status >= 300) {
        fprintf(stderr, "wget: HTTP error %d\n", status);
        return 1;
    }

    if (strcmp(out_file, "-") == 0) {
        printf("%s", body ? body : "");
        fflush(stdout);
    } else {
        FILE* fp = fopen(out_file, "wb");
        if (!fp) {
            fprintf(stderr, "wget: cannot open output file '%s'\n", out_file);
            return 1;
        }
        if (body && body_len > 0) {
            fwrite(body, 1, body_len, fp);
        }
        fclose(fp);
        printf("Saved %d bytes to '%s'\n", body_len, out_file);
    }

    return 0;
}
