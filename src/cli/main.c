/* SPDX-License-Identifier: MIT */
#include "treehound/common.h"
#include "treehound/ipc.h"
#include "treehound/json.h"
#include "treehound/strbuf.h"
#include "treehound/util.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef TH_HAVE_GUI
int th_gui_run(int argc, char **argv);
#endif
static void help(void)
{
    puts("Treehound " TH_VERSION " — indexed filename search and disk usage\n"
         "Usage: treehound [--version|--help]\n"
         "       treehound COMMAND [QUERY] [OPTIONS]\n"
         "Commands: search, status, roots, scan, verify, rebuild, config\n"
         "Options: --json --socket PATH --root ID --under PATH --limit N --offset N\n"
         "         --sort name|path|size|type|mtime --descending --case-sensitive\n"
         "         --files-only --dirs-only --min-size SIZE --max-size SIZE\n"
         "         --after DATE|7d --before DATE|7d --extension EXT --exact --wait\n"
         "Search never scans the filesystem. scan/verify are asynchronous unless --wait.\n"
         "Exit codes: 0 success, 1 no matches, 2 usage, 3 daemon unavailable, 4 failed.\n"
         "Run treehoundd or start the systemd user service before CLI commands.");
}
static bool number(const char *s, int64_t *out)
{
    char *end; errno = 0; long long n = strtoll(s, &end, 10);
    if (errno || end == s || *end || n < 0) return false;
    *out = (int64_t)n; return true;
}
static void print_response(const char *cmd, const th_jval *res)
{
    const th_jval *items = th_json_get(res, !strcmp(cmd, "roots") ? "roots" : "items");
    if (items && items->type == TH_JARR) {
        puts(!strcmp(cmd, "roots") ? "ID\tPath\tLogical bytes\tStatus" : "Name\tPath\tBytes\tType\tModified\tStatus");
        for (size_t i = 0; i < items->n; i++) {
            const th_jval *r = &items->items[i];
            if (!strcmp(cmd, "roots")) printf("%lld\t", (long long)th_json_get_int(r, "id", 0));
            else printf("%s\t", th_json_get_str(r, "name", ""));
            printf("%s\t%lld\t", th_json_get_str(r, "path", ""), (long long)th_json_get_int(r, "size", 0));
            if (strcmp(cmd, "roots")) printf("%s\t%lld\t", th_json_get_str(r, "type", ""), (long long)th_json_get_int(r, "mtime", 0));
            printf("%s\n", th_json_get_str(r, "status", ""));
        }
    } else if (!strcmp(cmd, "status")) {
        printf("Treehound %s: %lld indexed entries; %s; %lld queued; completed %lld/%lld; live monitoring %s\n",
               th_json_get_str(res, "version", ""), (long long)th_json_get_int(res, "entries", 0),
               th_json_get_bool(res, "busy", false) ? "updating" : "idle",
               (long long)th_json_get_int(res, "queued", 0), (long long)th_json_get_int(res, "completed_seq", 0),
               (long long)th_json_get_int(res, "issued_seq", 0), th_json_get_bool(res, "live_indexing", false) ? "on" : "off");
    } else if (!strcmp(cmd, "config")) puts(th_json_get_str(res, "config", ""));
    else printf("Queued sequence %lld\n", (long long)th_json_get_int(res, "seq", 0));
}
int main(int argc, char **argv)
{
    if (argc == 1) {
#ifdef TH_HAVE_GUI
        return th_gui_run(argc, argv);
#else
        help(); return 0;
#endif
    }
    if (!strcmp(argv[1], "--version")) { puts("treehound " TH_VERSION); return 0; }
    if (!strcmp(argv[1], "--help")) { help(); return 0; }
    const char *cmd = argv[1];
    if (strcmp(cmd, "search") && strcmp(cmd, "status") && strcmp(cmd, "roots") && strcmp(cmd, "scan") &&
        strcmp(cmd, "verify") && strcmp(cmd, "rebuild") && strcmp(cmd, "config")) { help(); return TH_EXIT_USAGE; }
    th_strbuf request; th_sb_init(&request); th_jw w; th_jw_init(&w, &request); th_jw_obj_begin(&w);
    th_jw_kv_int(&w, "v", TH_PROTOCOL_VERSION); th_jw_kv_str(&w, "cmd", cmd);
    char *socket_path = th_socket_path(); bool json = false, wait = false, have_query = false;
    int64_t root_id = 0;
    int result = TH_EXIT_USAGE;
    for (int i = 2; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--json")) { json = true; continue; }
        if (!strcmp(arg, "--wait")) { wait = true; continue; }
        if (!strcmp(arg, "--case-sensitive")) { th_jw_kv_bool(&w, "case_sensitive", true); continue; }
        if (!strcmp(arg, "--descending")) { th_jw_kv_bool(&w, "descending", true); continue; }
        if (!strcmp(arg, "--exact")) { th_jw_kv_bool(&w, "exact", true); continue; }
        if (!strcmp(arg, "--files-only") || !strcmp(arg, "--dirs-only")) { th_jw_kv_str(&w, "type", arg[2] == 'f' ? "file" : "dir"); continue; }
        if (strncmp(arg, "--", 2)) { if (strcmp(cmd, "search") || have_query) goto done; have_query = true; th_jw_kv_str(&w, "query", arg); continue; }
        if (++i >= argc) goto done;
        const char *value = argv[i];
        if (!strcmp(arg, "--socket")) { free(socket_path); socket_path = th_xstrdup(value); continue; }
        const char *key = !strcmp(arg, "--root") ? "root_id" : !strcmp(arg, "--limit") ? "limit" : !strcmp(arg, "--offset") ? "offset" : NULL;
        int64_t n;
        if (key) { if (!number(value, &n)) goto done; th_jw_kv_int(&w, key, n); if (!strcmp(key, "root_id")) root_id = n; continue; }
        key = !strcmp(arg, "--min-size") ? "min_size" : !strcmp(arg, "--max-size") ? "max_size" : NULL;
        if (key) { if (!th_parse_size(value, &n)) goto done; th_jw_kv_int(&w, key, n); continue; }
        key = !strcmp(arg, "--after") ? "min_mtime" : !strcmp(arg, "--before") ? "max_mtime" : NULL;
        if (key) { if (!th_parse_time(value, th_now(), &n)) goto done; th_jw_kv_int(&w, key, n); continue; }
        key = !strcmp(arg, "--under") ? "under" : !strcmp(arg, "--sort") ? "sort" : !strcmp(arg, "--extension") ? "extension" : NULL;
        if (key) { th_jw_kv_str(&w, key, value); continue; }
        goto done;
    }
    if (wait && strcmp(cmd, "scan") && strcmp(cmd, "verify") && strcmp(cmd, "rebuild")) goto done;
    th_jw_obj_end(&w);
    th_strbuf err; th_sb_init(&err);
    th_jval *res = th_ipc_call(socket_path, request.data, request.len, 5000, &err);
    if (!res) { fprintf(stderr, "%s\n", err.data ? err.data : "daemon unavailable"); result = TH_EXIT_UNAVAILABLE; }
    else if (!th_json_get_bool(res, "ok", false)) { fprintf(stderr, "%s\n", th_json_get_str(res, "error", "request failed")); result = TH_EXIT_FAILED; }
    else {
        result = TH_EXIT_OK;
        if (wait) {
            int64_t seq = th_json_get_int(res, "seq", 0);
            th_strbuf status; th_sb_init(&status); th_sb_puts(&status, "{\"v\":1,\"cmd\":\"status\"}");
            int64_t deadline = th_mono_ms() + 3600000;
            for (;;) {
                th_jval *st = th_ipc_call(socket_path, status.data, status.len, 5000, &err);
                if (!st || !th_json_get_bool(st, "ok", false)) { th_json_free(st); result = TH_EXIT_FAILED; break; }
                bool complete = th_json_get_int(st, "completed_seq", 0) >= seq;
                th_json_free(st);
                if (complete) break;
                if (th_mono_ms() >= deadline) { result = TH_EXIT_FAILED; break; }
                usleep(100000);
            }
            th_sb_free(&status);
            if (result == TH_EXIT_OK) {
                const char *rq = "{\"v\":1,\"cmd\":\"roots\"}";
                th_jval *rs = th_ipc_call(socket_path, rq, strlen(rq), 5000, &err);
                const th_jval *roots = th_json_get(rs, "roots");
                if (!rs || !th_json_get_bool(rs, "ok", false) || !roots) result = TH_EXIT_FAILED;
                else for (size_t j = 0; j < roots->n; j++) {
                    const th_jval *r = &roots->items[j];
                    if (root_id && th_json_get_int(r, "id", 0) != root_id) continue;
                    const char *state = th_json_get_str(r, "status", "error");
                    if (strcmp(state, "verified") && strcmp(state, "updating")) {
                        fprintf(stderr, "%s: %s (%s)\n", th_json_get_str(r, "path", ""), state, th_json_get_str(r, "error", ""));
                        result = TH_EXIT_FAILED;
                    }
                }
                th_json_free(rs);
            }
        }
        if (json) {
            /* Re-emit byte-safe JSON, including invalid UTF-8 path escapes. */
            th_strbuf output; th_sb_init(&output); th_jw ow; th_jw_init(&ow, &output);
            th_json_write_value(&ow, res); puts(output.data); th_sb_free(&output);
        } else print_response(cmd, res);
        const th_jval *items = th_json_get(res, "items");
        if (!strcmp(cmd, "search") && items && !items->n) result = TH_EXIT_NO_MATCH;
    }
    th_json_free(res); th_sb_free(&err);
done:
    if (result == TH_EXIT_USAGE) fputs("invalid command options; see --help\n", stderr);
    free(socket_path); th_sb_free(&request); return result;
}
