/* store-test.c: apps/common/ps5update.c on the Mac (store-test.sh builds and
 * runs it): its JSON reading and the ProsperoStore check, which must fail
 * closed. store-test <empty work folder>; prints PASS/FAIL lines, exits 1 on
 * a failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../apps/common/ps5update.h"

static int fails;
static char work[512];

static void check(int ok, const char* what, const char* detail)
{
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    if (!ok)
        fails++;
}

static void json(const char* text, const char* key, int expect, const char* value, const char* what)
{
    char out[64] = "";
    int r = ps5update_json_string(text, key, out, sizeof out);
    char detail[200];
    snprintf(detail, sizeof detail, "%d \"%s\"", r, out);
    check(r == expect && (!value || !strcmp(out, value)), what, detail);
}

static void put(const char* relative, const char* text)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", work, relative);
    FILE* f = fopen(path, "w");
    if (!f) {
        perror(path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

static void drop(const char* relative)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", work, relative);
    chmod(path, 0644);
    unlink(path);
}

static void store(int managed, int busy, const char* what)
{
    char root[600], folder[600];
    snprintf(root, sizeof root, "%s/store", work);
    snprintf(folder, sizeof folder, "%s/homebrew/PPSA99207", work);
    ps5update_store s;
    ps5update_store_check(root, "PPSA99207", folder, &s);
    check(s.managed == managed && s.busy == busy, what, s.note);
}

static void receipt(const char* location, const char* extra)
{
    char text[2048];
    snprintf(text, sizeof text, "{\"titleId\": \"PPSA99207\", \"location\": \"%s\", \"contentVersion\": \"01.002.000\"%s}", location, extra);
    put("store/receipts/PPSA99207.json", text);
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: store-test <empty folder>\n");
        return 2;
    }
    snprintf(work, sizeof work, "%s", argv[1]);

    /* JSON */
    json("{\"a\":\"b\"}", "a", 1, "b", "a plain value");
    json(" {\n \"x\": [1, {\"a\": \"nested\"}], \"a\" : \"top\" } \n", "a", 1, "top", "the top-level key, not a nested one");
    json("{\"x\": {\"a\": \"nested\"}}", "a", 0, "", "only nested: not there");
    json("{\"a\":\"q\\\"b\\\\s\\/\"}", "a", 1, "q\"b\\s/", "escapes decoded");
    json("{\"a\":\"\\u00e9\\ud83d\\ude00\"}", "a", 1, "\xc3\xa9\xf0\x9f\x98\x80", "\\u escapes and a surrogate pair");
    json("{\"a\":\"b\",\"a\":\"c\"}", "a", -1, NULL, "a key twice: refused");
    json("{\"a\":1}", "a", -1, NULL, "not a string: refused");
    json("{\"a\":\"b\"", "a", -1, NULL, "unterminated object");
    json("{\"a\":\"b\"} x", "a", -1, NULL, "trailing text");
    json("{\"a\":\"b\",}", "a", -1, NULL, "trailing comma");
    json("[{\"a\":\"b\"}]", "a", -1, NULL, "not an object");
    json("{\"x\":\"\\ud800\",\"a\":\"b\"}", "a", -1, NULL, "a lone surrogate");
    json("{\"a\":\"\\u0000\"}", "a", -1, NULL, "a NUL");
    json("{\"a\":\"0123456789012345678901234567890123456789012345678901234567890123456789\"}", "a", -1, NULL, "too long for the buffer");
    json("{\"n\":-1.5e+3,\"t\":true,\"f\":false,\"z\":null,\"e\":{},\"l\":[],\"a\":\"ok\"}", "a", 1, "ok", "every kind of value");
    json("{\"n\":01,\"a\":\"b\"}", "a", -1, NULL, "a leading zero");

    char line[32];
    check(!ps5update_line_value("version=0.3.0\ntag=v0.3.0\n", "tag", line, sizeof line) && !strcmp(line, "v0.3.0"), "key=value", line);
    check(ps5update_line_value("versions=1\n", "version", line, sizeof line) == -1, "key=value: a longer key isn't it", NULL);

    /* The store */
    char path[1024];
    snprintf(path, sizeof path, "%s/homebrew/PPSA99207/sce_sys", work);
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s' '%s/store/receipts' && ln -s '%s/homebrew' '%s/linked'", path, work, work, work);
    if (system(cmd))
        return 2;
    put("homebrew/PPSA99207/sce_sys/param.json", "{\"titleId\":\"PPSA99207\",\"contentVersion\":\"01.002.000\",\"x\":{\"titleId\":\"NOPE\"}}");
    char id[16], version[16];
    snprintf(path, sizeof path, "%s/homebrew/PPSA99207", work);
    check(!ps5update_param(path, id, sizeof id, version, sizeof version) && !strcmp(id, "PPSA99207") && !strcmp(version, "01.002.000"), "param.json", id);

    char location[700];
    store(0, 0, "no receipt, no journal: not the store's");
    snprintf(location, sizeof location, "%s/homebrew", work);
    receipt(location, "");
    store(1, 0, "a receipt naming this folder: the store's");
    snprintf(location, sizeof location, "%s//homebrew/", work);
    receipt(location, "");
    store(1, 0, "the same folder spelt with // and a trailing /");
    snprintf(location, sizeof location, "%s/linked", work);
    receipt(location, "");
    store(1, 0, "the same folder through a link");
    snprintf(location, sizeof location, "%s/homebrew", work);
    put("store/receipts/PPSA99207.json", "{\"titleId\":\"PPSA99207\",\"location\":\"");
    store(1, 0, "a truncated receipt: the store's");
    put("store/receipts/PPSA99207.json", "{\"titleId\":\"PPSA99207\",\"contentVersion\":\"01.002.000\"}");
    store(1, 0, "a receipt with no location: the store's");
    snprintf(cmd, sizeof cmd, "{\"titleId\":\"PPSA99207\",\"location\":\"%s/homebrew\",\"contentVersion\":\"01.001.000\"}", work);
    put("store/receipts/PPSA99207.json", cmd);
    store(1, 0, "a receipt for this folder at another version: still the store's");
    snprintf(cmd, sizeof cmd, "{\"titleId\":\"PPSA99207\",\"location\":\"%s/homebrew\",\"files\":[", work);
    {
        snprintf(path, sizeof path, "%s/store/receipts/PPSA99207.json", work);
        FILE* f = fopen(path, "w");
        fputs(cmd, f);
        for (int i = 0; i < 40000; i++)
            fprintf(f, "%s\"sce_sys/file-%05d.bin\"", i ? "," : "", i);
        fputs("]}", f);
        fclose(f);
    }
    store(1, 0, "a receipt with a big file list (over 16 KB): the store's");
    {
        FILE* f = fopen(path, "w");
        fputs(cmd, f);
        for (int i = 0; i < 200000; i++)
            fprintf(f, "%s\"sce_sys/file-%06d.bin\"", i ? "," : "", i);
        fputs("]}", f);
        fclose(f);
    }
    store(1, 0, "a receipt over the 1 MB limit: the store's");
    receipt("/mnt/usb0/homebrew", "");
    store(0, 0, "a receipt naming another folder: not the store's");
    put("store/receipts/PPSA99207.json", "{\"titleId\":\"PPSA00001\",\"location\":\"/data/homebrew\"}");
    store(1, 0, "a receipt for another title under this name: the store's");
    snprintf(location, sizeof location, "%s/homebrew", work);
    receipt(location, "");
    snprintf(path, sizeof path, "%s/store/receipts/PPSA99207.json", work);
    if (getuid() != 0) {
        chmod(path, 0);
        store(1, 0, "an unreadable receipt: the store's");
        chmod(path, 0644);
    }
    drop("store/receipts/PPSA99207.json");

    put("store/journal.json", "{\"op\":\"update\",\"titleId\":\"PPSA99207\",\"stage\":\"swap\"}");
    store(0, 1, "a journal naming the browser: busy");
    put("store/journal.json", "{\"op\":\"install\",\"items\":[{\"titleId\":\"PPSA99207\"}]}");
    store(0, 1, "a journal naming it nested: busy");
    put("store/journal.json", "{\"op\":\"update\",\"titleId\":\"PPSA12345\"}");
    store(0, 0, "a journal about another app: not busy");
    put("store/journal.json", "{\"op\":\"upd");
    store(0, 1, "a damaged journal: busy");
    drop("store/journal.json");

    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
