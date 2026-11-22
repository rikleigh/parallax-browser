/*
 * The download manager's page for a phone (/dlm on the link server;
 * parallax://downloads serves it too, but the TV uses the menu's Downloads
 * and Files lists: downloads.c, files.c): downloads waiting for a folder, the list with
 * progress and actions (pause, resume, unpack, install, exFAT image), jobs
 * on files already on a drive, a file browser over the console's drives,
 * and the settings. See port/webkit/README.md ("Downloads").
 *
 *   GET  /dlm            the page
 *   GET  /dlm.json       the state (downloads.c's snapshot)
 *   GET  /dlm/fs?op=roots | op=ls&path= | op=partials | op=titleinfo&path=
 *              the drives, a folder, the unfinished downloads in the download
 *              folders, what an image, package or app folder holds (the service)
 *   GET  /dlm/icon?path=  that app's icon0.png (the service copies it out to
 *              its icon cache; only files from there are served)
 *   POST /dlm  do=mkdir&path&name | do=rename&path&name | do=rmpath&path (a
 *              delete job: the same as do=task&kind=delete)
 *              (the service), anything else downloads.c's actions
 *
 * Route threads. downloads.c's actions run on the main loop (this thread
 * waits for them); file operations go to the download service from here.
 * The page uses no alert/confirm/prompt (they would block the browser):
 * deleting asks for a second press.
 */
#define SPIKE_NAME "ps5browser"
#include "common/spike.h"
#include "dlm.h"
#include "dlservice.h"
#include "downloads.h"

#include <string.h>

/* ---- the main loop's actions ---- */

typedef struct {
    GHashTable* params;
    const char* error;
    gboolean done;
    GMutex lock;
    GCond cond;
} MainCall;

static gboolean manageOnMain(gpointer data)
{
    MainCall* call = data;
    const char* error = ps5wk_downloads_manage(call->params);
    g_mutex_lock(&call->lock);
    call->error = error;
    call->done = TRUE;
    g_cond_signal(&call->cond);
    g_mutex_unlock(&call->lock);
    return G_SOURCE_REMOVE;
}

static char* replyJSON(gboolean ok, const char* error)
{
    if (ok)
        return g_strdup("{\"ok\":true}\n");
    GString* out = g_string_new("{\"ok\":false,\"error\":\"");
    for (const char* c = error ? error : "failed"; *c; c++) {
        if (*c == '"' || *c == '\\')
            g_string_append_c(out, '\\');
        if ((unsigned char)*c >= 0x20)
            g_string_append_c(out, *c);
    }
    g_string_append(out, "\"}\n");
    return g_string_free(out, FALSE);
}

static GHashTable* formParams(const char* text)
{
    /* a form's body: '+' is a space */
    char* copy = g_strdup(text ? text : "");
    for (char* c = copy; *c; c++) {
        if (*c == '+')
            *c = ' ';
    }
    GHashTable* params = g_uri_parse_params(copy, -1, "&", G_URI_PARAMS_NONE, NULL);
    g_free(copy);
    return params ? params : g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

static const char* value(GHashTable* params, const char* key)
{
    const char* v = g_hash_table_lookup(params, key);
    return v && *v ? v : NULL;
}

/* The string value of "key" in the service's JSON (its own escaping: \"
 * \\ and \u00XX), or NULL. */
static char* jsonStringField(const char* json, const char* key)
{
    char* quoted = g_strdup_printf("\"%s\":\"", key);
    const char* p = json ? strstr(json, quoted) : NULL;
    g_free(quoted);
    if (!p)
        return NULL;
    p = strchr(p, ':') + 2;
    GString* out = g_string_new(NULL);
    for (; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1] == 'u' && g_ascii_isxdigit(p[2]) && g_ascii_isxdigit(p[3]) && g_ascii_isxdigit(p[4]) && g_ascii_isxdigit(p[5])) {
            g_string_append_c(out, (char)(g_ascii_xdigit_value(p[4]) * 16 + g_ascii_xdigit_value(p[5])));
            p += 5;
        } else if (*p == '\\' && p[1]) {
            g_string_append_c(out, *++p);
        } else
            g_string_append_c(out, *p);
    }
    if (*p != '"') {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

/* The number after "key": in the service's JSON, as text, or NULL. */
static char* jsonNumberField(const char* json, const char* key)
{
    char* quoted = g_strdup_printf("\"%s\":", key);
    const char* p = json ? strstr(json, quoted) : NULL;
    size_t skip = strlen(quoted);
    g_free(quoted);
    if (!p)
        return NULL;
    p += skip;
    size_t n = strspn(p, "-0123456789");
    return n ? g_strndup(p, n) : NULL;
}

static char* serviceReply(char* json, char* error)
{
    char* reply;
    if (json) {
        reply = g_strdup_printf("{\"ok\":true,\"result\":%s}\n", json);
        g_free(json);
    } else
        reply = replyJSON(FALSE, error);
    g_free(error);
    return reply;
}

gboolean ps5wk_dlm_serve(const char* method, const char* path, const char* query, const char* body, int* status, const char** type, char** reply)
{
    gboolean get = !strcmp(method, "GET") || !strcmp(method, "HEAD");
    gboolean post = !strcmp(method, "POST");
    *status = 200;
    *type = "application/json";
    if (!strcmp(path, "/dlm") && get) {
        *type = "text/html; charset=utf-8";
        *reply = g_strdup(ps5wk_dlm_page_html);
        return TRUE;
    }
    if (!strcmp(path, "/dlm.json") && get) {
        *reply = ps5wk_downloads_manager_json();
        return TRUE;
    }
    if (!strcmp(path, "/dlm/fs") && get) {
        GHashTable* params = formParams(query);
        const char* op = value(params, "op");
        char* error = NULL;
        char* json = NULL;
        if (op && !strcmp(op, "roots"))
            json = ps5wk_dlservice_request(8000, &error, "roots", NULL);
        else if (op && !strcmp(op, "ls"))
            json = ps5wk_dlservice_request(8000, &error, "ls", "path", value(params, "path"), NULL);
        else if (op && !strcmp(op, "titleinfo"))
            json = ps5wk_dlservice_request(15000, &error, "titleinfo", "path", value(params, "path"), NULL);
        else if (op && !strcmp(op, "partials")) {
            char* dirs = ps5wk_downloads_scan_dirs();
            json = ps5wk_dlservice_request(8000, &error, "partials", "dirs", dirs, NULL);
            g_free(dirs);
        } else
            error = g_strdup("op=roots, op=ls&path=, op=partials or op=titleinfo&path=");
        *reply = serviceReply(json, error);
        g_hash_table_unref(params);
        return TRUE;
    }
    if (!strcmp(path, "/dlm") && post) {
        GHashTable* params = formParams(body && *body ? body : query);
        const char* action = value(params, "do");
        if (action && !strcmp(action, "rmpath")) {
            /* a delete job (a task), not a request: a big folder would hold
             * the link server for its whole length */
            g_hash_table_replace(params, g_strdup("do"), g_strdup("task"));
            g_hash_table_replace(params, g_strdup("kind"), g_strdup("delete"));
            action = value(params, "do");
        }
        if (action && !strcmp(action, "adopt")) {
            /* the address and sizes come from the service's own look at the
             * .part.ps5dl, not from the page */
            char* error = NULL;
            char* json = ps5wk_dlservice_request(8000, &error, "partial", "path", value(params, "path"), NULL);
            char* url = jsonStringField(json, "url");
            char* total = jsonNumberField(json, "total");
            char* received = jsonNumberField(json, "received");
            if (!json || !url) {
                *status = 409;
                *reply = replyJSON(FALSE, error ? error : "That isn't an unfinished download");
                g_free(json);
                g_free(error);
                g_free(url);
                g_free(total);
                g_free(received);
                g_hash_table_unref(params);
                return TRUE;
            }
            g_hash_table_replace(params, g_strdup("url"), url);
            g_hash_table_replace(params, g_strdup("total"), total ? total : g_strdup("-1"));
            g_hash_table_replace(params, g_strdup("received"), received ? received : g_strdup("0"));
            g_free(json);
            g_free(error);
        }
        if (action && !strcmp(action, "rename") && ps5wk_downloads_in_use(value(params, "path"))) {
            *status = 409;
            *reply = replyJSON(FALSE, "A download or a job is using it");
            g_hash_table_unref(params);
            return TRUE;
        }
        if (action && (!strcmp(action, "mkdir") || !strcmp(action, "rename"))) {
            char* error = NULL;
            const char* op = action;
            char* json = ps5wk_dlservice_request(15000, &error, op, "path", value(params, "path"), "name", value(params, "name"), NULL);
            spike_log("[dl] page: %s %s%s%s: %s", op, value(params, "path") ? value(params, "path") : "", value(params, "name") ? " " : "",
                value(params, "name") ? value(params, "name") : "", json ? "ok" : (error ? error : "?"));
            *reply = serviceReply(json, error);
            g_hash_table_unref(params);
            return TRUE;
        }
        MainCall* call = g_new0(MainCall, 1);
        call->params = params;
        g_mutex_init(&call->lock);
        g_cond_init(&call->cond);
        g_main_context_invoke(NULL, manageOnMain, call);
        gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
        g_mutex_lock(&call->lock);
        while (!call->done && g_cond_wait_until(&call->cond, &call->lock, deadline))
            ;
        gboolean done = call->done;
        const char* error = call->error;
        g_mutex_unlock(&call->lock);
        if (!done) {
            /* still queued: it runs later and leaks its small struct */
            *status = 503;
            *reply = replyJSON(FALSE, "the browser is busy");
            return TRUE;
        }
        spike_log("[dl] page: %s%s%s", action ? action : "?", error ? ": " : "", error ? error : "");
        *status = error ? 409 : 200;
        *reply = replyJSON(!error, error);
        g_hash_table_unref(params);
        g_mutex_clear(&call->lock);
        g_cond_clear(&call->cond);
        g_free(call);
        return TRUE;
    }
    if (!strcmp(path, "/dlm") || !strcmp(path, "/dlm.json") || !strcmp(path, "/dlm/fs")) {
        *status = 405;
        *reply = replyJSON(FALSE, "method not allowed");
        return TRUE;
    }
    return FALSE;
}

#define ICON_CACHE "/data/ps5webkit/dl/icons/"

gboolean ps5wk_dlm_icon(const char* query, char** data, gsize* length)
{
    GHashTable* params = formParams(query);
    char* error = NULL;
    char* json = ps5wk_dlservice_request(30000, &error, "icon", "path", value(params, "path"), NULL);
    char* file = jsonStringField(json, "path");
    gboolean ok = file && g_str_has_prefix(file, ICON_CACHE) && !strstr(file, "..") && g_str_has_suffix(file, ".png")
        && g_file_get_contents(file, data, length, NULL);
    if (!ok)
        spike_log("[dl] page: no icon for %s: %s", value(params, "path") ? value(params, "path") : "?", error ? error : "?");
    g_free(json);
    g_free(error);
    g_free(file);
    g_hash_table_unref(params);
    return ok;
}

/* ---- the page ---- */

const char ps5wk_dlm_page_html[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>Downloads</title>\n"
    "<script>if(location.protocol=='parallax:'||location.hostname=='127.0.0.1'||location.hostname=='localhost')document.documentElement.className='tv'</script>\n"
    "<style>\n"
    ":root{color-scheme:light dark;--bg:#f7f5ff;--fg:#14113d;--muted:#5a5584;--card:#fff;--line:#dcd6f7;--accent:#6d0efd;--on:#fff;--warn:#a04b00;--ok:#0a7d4f;--focus:#d10aa6}\n"
    "@media(prefers-color-scheme:dark){:root{--bg:#07061f;--fg:#f4f2ff;--muted:#b9b5e0;--card:#110f35;--line:#2b2766;--accent:#a77bff;--on:#01001a;--warn:#ffb648;--ok:#5be3a7;--focus:#ff5cd6}}\n"
    ":root.tv{color-scheme:dark;--bg:#07061f;--fg:#f4f2ff;--muted:#b9b5e0;--card:#110f35;--line:#2b2766;--accent:#a77bff;--on:#01001a;--warn:#ffb648;--ok:#5be3a7;--focus:#ff5cd6}\n"
    "*{box-sizing:border-box}\n"
    "html{font-size:clamp(16px,1.15vw,24px)}\n"
    "body{margin:0;background:var(--bg);color:var(--fg);font:1rem/1.45 -apple-system,system-ui,sans-serif}\n"
    "main{max-width:64rem;margin:0 auto;padding:1.2rem 16px 4rem}\n"
    "header{display:flex;align-items:center;gap:.8rem;flex-wrap:wrap}\n"
    "h1{font-size:1.6rem;margin:0;flex:1}h1 span{display:block;font-size:.8rem;font-weight:600;color:var(--muted);letter-spacing:.02em}\n"
    "h2{font-size:1.15rem;margin:1.6rem 0 .4rem}\n"
    "nav{display:flex;gap:.5rem;flex-wrap:wrap;margin:.8rem 0}\n"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:.8rem;padding:.8rem 1rem;margin:.6rem 0}\n"
    ".ask{border:2px solid var(--accent)}\n"
    ".name{font-weight:600;overflow-wrap:anywhere}\n"
    ".sub,.help{color:var(--muted);font-size:.9rem;overflow-wrap:anywhere}\n"
    ".row{display:flex;gap:.6rem;align-items:center;flex-wrap:wrap}\n"
    ".grow{flex:1;min-width:12rem}\n"
    ".state{font-weight:600;white-space:nowrap}.warn{color:var(--warn)}.ok{color:var(--ok)}\n"
    ".seg{display:flex;flex-wrap:wrap;gap:.45rem;margin-top:.5rem}\n"
    "button,input,select{font:inherit;color:inherit}\n"
    "button{min-height:2.4rem;padding:.35rem .9rem;border-radius:.6rem;border:2px solid var(--line);background:transparent;cursor:pointer}\n"
    "button.primary{background:var(--accent);border-color:var(--accent);color:var(--on);font-weight:600}\n"
    "button.on{border-color:var(--accent);background:color-mix(in srgb,var(--accent) 22%,transparent)}\n"
    "button.armed{border-color:var(--warn);color:var(--warn)}\n"
    "button:focus,input:focus,select:focus{outline:5px solid var(--focus);outline-offset:3px}\n"
    "input,select{min-height:2.4rem;padding:.35rem .7rem;border:2px solid var(--line);border-radius:.6rem;background:var(--bg)}\n"
    ".bar{height:.55rem;border-radius:.3rem;background:var(--line);overflow:hidden;margin:.45rem 0}\n"
    ".bar div{height:100%;background:linear-gradient(90deg,#6d0efd,#d10aa6)}\n"
    ".entry{display:flex;gap:.6rem;align-items:center;flex-wrap:wrap;padding:.45rem 0;border-top:1px solid var(--line)}\n"
    ".entry:first-child{border-top:0}\n"
    ".tag{font-size:.75rem;font-weight:700;padding:.1rem .4rem;border-radius:.3rem;border:1px solid var(--line);color:var(--muted)}\n"
    ".empty{color:var(--muted);padding:.4rem 0}\n"
    "#notice{position:sticky;top:0;z-index:2;background:var(--accent);color:var(--on);padding:.5rem 1rem;border-radius:.6rem;margin:.4rem 0}\n"
    "</style></head><body><main>\n"
    "<header><h1><span>Parallax Browser</span>Downloads</h1><button id=\"back\" data-k=\"back\" hidden>Back to the page</button></header>\n"
    "<div id=\"pinbox\" class=\"card\" hidden><p>This PS5 asks for the link server's PIN.</p><div class=\"seg\"><input id=\"pin\" inputmode=\"numeric\" autocomplete=\"off\" placeholder=\"PIN\"><button id=\"pinok\">OK</button></div></div>\n"
    "<div id=\"notice\" hidden></div>\n"
    "<div id=\"asking\"></div>\n"
    "<nav id=\"tabs\"></nav>\n"
    "<section id=\"view\"></section>\n"
    "</main><script>\n"
    "var S=null,pin='',tv=document.documentElement.className=='tv',tab='list',ask={},peekAsked={},deletesSeen=null,partials=null,partialsAt=0,armed='',fs={path:null,data:null,roots:null,error:'',pick:null,newName:null,rename:null,transfer:null};\n"
    "try{pin=sessionStorage.getItem('ps5pin')||''}catch(e){}\n"
    "var q=new URLSearchParams(location.search);if(q.get('pin'))pin=q.get('pin');if(q.get('tab'))tab=q.get('tab');\n"
    "function $(id){return document.getElementById(id)}\n"
    "function esc(t){return String(t==null?'':t).replace(/[&<>\"']/g,function(c){return'&#'+c.charCodeAt(0)+';'})}\n"
    "function hdr(form){var h={'Accept':'application/json'};if(pin)h['X-PS5-PIN']=pin;if(form)h['Content-Type']='application/x-www-form-urlencoded';return h}\n"
    "function enc(o){return Object.keys(o).filter(function(k){return o[k]!=null}).map(function(k){return k+'='+encodeURIComponent(o[k])}).join('&')}\n"
    "function post(o){return fetch('/dlm',{method:'POST',headers:hdr(true),body:enc(o),cache:'no-store'}).then(function(r){return r.json().catch(function(){return{ok:r.ok}})}).then(function(j){if(j.ok===false)say(j.error||'That didn\\u2019t work');load();return j},function(){say('The PS5 did not answer.')})}\n"
    "function getj(u){return fetch(u,{headers:hdr(false),cache:'no-store'}).then(function(r){if(r.status==403){$('pinbox').hidden=false;throw 0}return r.json()})}\n"
    "var noticeTimer=0;function say(t){var n=$('notice');n.textContent=t;n.hidden=!t;clearTimeout(noticeTimer);if(t)noticeTimer=setTimeout(function(){n.hidden=true},6000)}\n"
    "function size(n){if(n==null||n<0)return'?';if(n>=1073741824)return(n/1073741824).toFixed(2)+' GB';if(n>=104857600)return Math.round(n/1048576)+' MB';if(n>=1048576)return(n/1048576).toFixed(1)+' MB';if(n>=1024)return Math.round(n/1024)+' KB';return n+' B'}\n"
    "function eta(s){if(s<60)return Math.max(1,Math.round(s))+' s left';if(s<3600)return Math.round(s/60)+' min left';return Math.floor(s/3600)+' h '+Math.floor(s/60)%60+' min left'}\n"
    "function short(p){if(!S)return p;var u=S.settings.userFolder;if(p==u)return'Downloads (this user)';if(p.indexOf(u+'/')==0)return'Downloads/'+p.slice(u.length+1);\n"
    "  return p.replace(/^\\/data\\//,'Internal: ').replace(/^\\/mnt\\/usb(\\d)/,function(m,d){return'USB '+(+d+1)+':'}).replace(/^\\/mnt\\/ext(\\d)/,function(m,d){return'Extended '+(+d+1)+':'})}\n"
    "function btn(k,text,run,cls){return{k:k,text:text,run:run,cls:cls||''}}\n"
    "var handlers={};\n"
    "function bare(list){return list.map(function(b){handlers[b.k]=b.run;return '<button data-k=\"'+esc(b.k)+'\" class=\"'+b.cls+(armed==b.k?' armed':'')+'\">'+esc(armed==b.k?'Press again: '+b.text:b.text)+'</button>'}).join('')}\n"
    "function buttons(list){return '<div class=\"seg\">'+bare(list)+'</div>'}\n"
    "function confirmFirst(k,run){return function(){if(armed==k){armed='';run()}else{armed=k;render()}}}\n"
    "function afterLabel(a){return{'extract':'unpack it','extract-install':'unpack and install the app','extract-image':'unpack and make an exFAT image','extract-image-ffpkg':'unpack and make a UFS2 image'}[a]||''}\n"
    "/* ---- what an image, package or app folder holds (op titleinfo), and its icon ---- */\n"
    "var info={};function isImg(n){return /\\.(exfat|ffpkg)$/i.test(n||'')}function isPkg(n){return /\\.pkg$/i.test(n||'')}\n"
    "function wantInfo(p){if(info[p])return;info[p]={pending:1};getj('/dlm/fs?op=titleinfo&path='+encodeURIComponent(p)).then(function(j){info[p]=j.ok?j.result:{error:j.error||'?'};render()}).catch(function(){info[p]={error:'no answer'};render()})}\n"
    "function infoHTML(p){var i=info[p];if(!i)return'';if(i.pending)return'<div class=\"sub\">Looking inside\\u2026</div>';if(i.error)return'<div class=\"sub warn\">What it holds isn\\u2019t known: '+esc(i.error)+'</div>';\n"
    "  var t=[i.name,i.id,i.version?'version '+i.version:''].filter(function(x){return x}).join(' \\u00b7 ');var w=i.package&&!i.fake&&i.platform=='PS5'?'A retail package: only fake packages (fPKG) install from here':i.installed?(i.installed==i.version?'This version is installed already':'A version of it is installed'):'';\n"
    "  return'<div class=\"row\">'+(i.icon?'<img alt=\"\" width=\"64\" height=\"64\" style=\"border-radius:12px;margin-right:12px\" src=\"/dlm/icon?path='+encodeURIComponent(p)+(pin?'&pin='+encodeURIComponent(pin):'')+'\">':'')+'<div class=\"grow\"><div class=\"sub\">'+esc(i.label||'')+(i.contentId?' \\u00b7 '+esc(i.contentId):'')+'</div><div class=\"name\">'+esc(t||'No app in it')+'</div>'+(w?'<div class=\"sub warn\">'+esc(w)+'</div>':'')+'</div></div>'}\n"
    "/* ---- the downloads waiting for a folder ---- */\n"
    "/* ---- what an archive holds (the service looked inside: ps5peek.h) ---- */\n"
    "var afterNames={none:'Keep the file',extract:'Unpack','extract-install':'Unpack & install app','extract-image':'Unpack & make exFAT image','extract-image-ffpkg':'Unpack & make UFS2 image'};\n"
    "function peekLine(p){if(!p)return'';var h='';\n"
    "  if(p.titleId){h+='<b>'+esc(p.name||p.titleId)+'</b> \\u00b7 '+esc(p.titleId)+(p.version?' \\u00b7 version '+esc(p.version):'')+(p.guessed?' (ID from its name)':'');\n"
    "    if(p.installed)h+='<br>'+(p.version&&p.installed==p.version?'This version is already installed':'Version '+esc(p.installed)+' is installed')}\n"
    "  else if(p.titles>1)h+=p.titles+' apps';\n"
    "  return h}\n"
    "function peekText(p){if(!p)return'';var t='';\n"
    "  if(p.titleId){t=(p.name||p.titleId)+' \\u00b7 '+p.titleId+(p.version?' \\u00b7 version '+p.version:'');if(p.installed)t+=' \\u00b7 '+(p.version&&p.installed==p.version?'this version is installed':'version '+p.installed+' is installed')}\n"
    "  else if(p.titles>1)t=p.titles+' apps';else if(p.namesHidden)t=p.format;else t=p.format+' \\u00b7 '+p.entries+' file'+(p.entries==1?'':'s');\n"
    "  if(p.refused)t+=' \\u00b7 can\\u2019t be unpacked: '+p.refused;else if(p.password)t+=' \\u00b7 '+(p.namesHidden?'a password over its list of files too':'it has a password')+': asked for when it\\u2019s unpacked';\n"
    "  if(p.emptyFolder)t+=' \\u00b7 its folder '+p.emptyFolder+'/ is empty: if the app wants files put there, install it as a folder (an image is read-only)';return t}\n"
    "function peekFacts(p){return esc(p.format)+' \\u00b7 '+p.entries+' file'+(p.entries==1?'':'s')+(p.sizesKnown?' \\u00b7 '+size(p.unpacked)+' unpacked':'')}\n"
    "function suggestion(p){if(!p)return null;if(p.refused)return{after:'none',why:'it can\\u2019t be unpacked ('+p.refused.replace(/\\.$/,'')+')'};\n"
    "  if(p.titles==1)return{after:'extract-install',why:'it holds an app'};\n"
    "  if(p.titles>1)return{after:'extract',why:'it holds '+p.titles+' apps: unpack it, then install each one from Files'};\n"
    "  return{after:'extract',why:'it holds files, not an app'}}\n"
    "function replaceNote(p){return p&&p.installed&&p.titleId?'Version '+(p.installed=='?'?'(not known)':p.installed)+' of '+(p.name||p.titleId)+' is installed: you\\u2019ll be asked before it\\u2019s replaced.':''}\n"
    "/* ---- the downloads waiting for a folder ---- */\n"
    "function askCard(d){var a=ask[d.id]||(ask[d.id]={dir:S.settings.folder,after:'none',touched:false,remember:false});var arch=d.archive,p=d.peek,sg=suggestion(p);\n"
    "  if(sg&&!a.touched)a.after=sg.after;\n"
    "  var h='<div class=\"card ask\"><div class=\"sub\">Where should this download go?</div><div class=\"name\">'+esc(d.file)+'</div><div class=\"sub\">'+size(d.size)+' from '+esc(d.host)+'</div>';\n"
    "  if(arch){if(d.peeking)h+='<p class=\"help\">Looking inside\\u2026</p>';else if(p)h+='<p class=\"help\">'+(peekLine(p)?peekLine(p)+'<br>':'')+peekFacts(p)+'</p>';else if(d.peekError)h+='<p class=\"help\">What\\u2019s inside will show once it\\u2019s downloaded ('+esc(d.peekError)+')</p>'}\n"
    "  h+='<p class=\"help\">Save to: <b>'+esc(short(a.dir))+'</b></p>';\n"
    "  var b=[btn('ask'+d.id+'-user','Downloads folder',function(){a.dir=S.settings.userFolder;render()},a.dir==S.settings.userFolder?'on':'')];\n"
    "  (S.settings.favorites||[]).forEach(function(f,i){b.push(btn('ask'+d.id+'-fav'+i,'\\u2605 '+short(f),function(){a.dir=f;render()},a.dir==f?'on':''))});\n"
    "  b.push(btn('ask'+d.id+'-browse','Choose another folder\\u2026',function(){fs.pick={id:d.id};tab='files';fs.path=fs.path||null;openFolder(fs.path)}));h+=buttons(b);\n"
    "  if(arch){h+='<p class=\"help\">When it\\u2019s done:'+(sg?' <b>'+esc(afterNames[sg.after])+'</b> is suggested: '+esc(sg.why)+'.':'')+'</p>';\n"
    "    var o=p&&p.refused?['none']:['none','extract','extract-install'];if(p&&p.titleId&&p.titles==1&&p.icon&&S.settings.imagesOK!==false)o.push('extract-image');\n"
    "    h+=buttons(o.map(function(x){return btn('ask'+d.id+'-a-'+x,afterNames[x],function(){a.after=x;a.touched=true;render()},a.after==x?'on':'')}));\n"
    "    if(a.after=='extract-install'&&replaceNote(p))h+='<p class=\"help warn\">'+esc(replaceNote(p))+'</p>'}\n"
    "  h+=buttons([btn('ask'+d.id+'-rem',(a.remember?'\\u2611':'\\u2610')+' Always save here',function(){a.remember=!a.remember;render()}),\n"
    "    btn('ask'+d.id+'-go','Download',function(){post({do:'save',id:d.id,dir:a.dir,after:arch?a.after:'none',remember:a.remember?1:0});delete ask[d.id]},'primary'),\n"
    "    btn('ask'+d.id+'-no','Don\\u2019t download',function(){post({do:'remove',id:d.id})})]);\n"
    "  return h+'</div>'}\n"
    "/* ---- the list ---- */\n"
    "/* unfinished downloads the list lost (a .part.ps5dl in a download folder) */\n"
    "function partialsLost(){if(!partials||!S)return[];var mine={};(S.downloads||[]).forEach(function(d){if(d.file)mine[d.dir+'/'+d.file]=1});return partials.filter(function(x){return !mine[x.path]})}\n"
    "function loadPartials(){partialsAt=Date.now();getj('/dlm/fs?op=partials').then(function(j){partials=j.ok?j.result:[];render()}).catch(function(){})}\n"
    "function partialCard(x,i){var k='pt'+i+'-'+x.name.length,host='';try{host=new URL(x.url).host}catch(e){}\n"
    "  var pc=x.total>0?Math.min(100,x.received*100/x.total):-1;\n"
    "  var h='<div class=\"card\"><div class=\"row\"><div class=\"grow\"><div class=\"name\">'+esc(x.name)+'</div><div class=\"sub\">'+esc(short(x.dir))+(host?' \\u00b7 '+esc(host):'')+'</div></div><div class=\"state\">'+(pc>=0?Math.floor(pc)+'%':'')+'</div></div>';\n"
    "  if(pc>=0)h+='<div class=\"bar\"><div style=\"width:'+pc.toFixed(1)+'%\"></div></div>';\n"
    "  h+='<div class=\"sub\">'+size(x.received)+(x.total>0?' of '+size(x.total):'')+(x.mtime?' \\u00b7 last saved '+new Date(x.mtime*1000).toLocaleString():'')+'</div>';\n"
    "  return h+buttons([btn(k+'-go','Continue',function(){post({do:'adopt',path:x.path}).then(loadPartials)},'primary'),\n"
    "    btn(k+'-del','Delete',confirmFirst(k+'-del',function(){post({do:'partial-delete',path:x.path}).then(function(){setTimeout(loadPartials,500)})}))])+'</div>'}\n"
    "function dlCard(d){var pc=d.size>0?Math.min(100,d.received*100/d.size):-1,st='',cls='',detail='',b=[],k='d'+d.id;\n"
    "  if(d.state=='running'){if(d.retryIn>0){st='Retrying in '+Math.round(d.retryIn)+' s';cls='warn';detail=d.error||''}else if(d.connecting){st='Connecting\\u2026'}else{st=pc>=0?Math.floor(pc)+'%':'';\n"
    "      detail=size(d.received)+(d.size>0?' of '+size(d.size):'')+(d.speed>1?' \\u00b7 '+size(d.speed)+'/s':'')+(d.speed>1&&d.size>d.received?' \\u00b7 '+eta((d.size-d.received)/d.speed):'')+(d.conns>1?' \\u00b7 '+d.conns+' connections':'')}\n"
    "    b.push(btn(k+'-pause','Pause',function(){post({do:'pause',id:d.id})}));b.push(btn(k+'-cancel','Cancel',confirmFirst(k+'-cancel',function(){post({do:'cancel',id:d.id})})))}\n"
    "  else if(d.state=='paused'){st='Paused';detail=size(d.received)+(d.size>0?' of '+size(d.size):'')+(d.error?' \\u00b7 '+d.error:'');b.push(btn(k+'-resume','Resume',function(){post({do:'resume',id:d.id})},'primary'));b.push(btn(k+'-cancel','Cancel',confirmFirst(k+'-cancel',function(){post({do:'cancel',id:d.id})})))}\n"
    "  else if(d.state=='failed'){st='Failed';cls='warn';detail=d.error||'';b.push(btn(k+'-resume','Retry',function(){post({do:'resume',id:d.id})},'primary'));b.push(btn(k+'-rm','Remove',function(){post({do:'remove',id:d.id})}))}\n"
    "  else if(d.state=='cancelled'){st='Cancelled';detail='The partial file was deleted';b.push(btn(k+'-rm','Remove',function(){post({do:'remove',id:d.id})}))}\n"
    "  else if(d.state=='done'){st='Done';cls='ok';detail=size(d.size)+' \\u00b7 '+new Date(d.finished*1000).toLocaleString();pc=-1;\n"
    "    if(d.jobState==1){var jp=d.jobTotal>0?Math.min(100,d.jobDone*100/d.jobTotal):-1;st={extract:'Unpacking',install:'Installing',image:'Making image',verify:'Checking the image',cleanup:'Deleting the unpacked copy',move:'Moving'}[d.jobPhase||d.job]||'Working';st+=jp>=0?' '+Math.floor(jp)+'%':'\\u2026';cls='';pc=jp;\n"
    "      b.push(btn(k+'-jstop','Stop',function(){post({do:'jobstop',id:d.id})}))}\n"
    "    else{if(d.jobState==2&&d.titleId&&/\\.pkg$/i.test(d.file||'')){st='Installing';detail='The console is installing \\u201c'+(d.titleName||d.titleId)+'\\u201d: see its downloads'}else if(d.jobState==2&&d.titleId&&/\\.(exfat|ffpkg)$/i.test(d.file||'')){st='Installed';detail='\\u201c'+(d.titleName||d.titleId)+'\\u201d handed to the image mounter'}else if(d.jobState==2&&d.titleId){st='Installed';detail='\\u201c'+(d.titleName||d.titleId)+'\\u201d is on the home screen'}else if(d.jobState==2&&d.jobOut){detail+=' \\u00b7 made '+short(d.jobOut)}\n"
    "      if(d.jobState==3&&d.replaceVersion){st='Installed already';cls='warn';detail=d.jobError||''}\n"
    "      else if(d.jobState==3&&d.password){st=d.password=='wrong'?'Wrong password':'Needs its password';cls='warn';detail=d.jobError||''}\n"
    "      else if(d.jobState==3){cls='warn';detail={extract:'Unpacking',install:'Installing',image:'Making the image',move:'Moving'}[d.job]+' failed: '+(d.jobError||'')}\n"
    "      if(d.archive){var p=d.peek,sg=d.jobState==2?null:suggestion(p);\n"
    "        if(!p&&!d.peeking&&!d.peekedLocal&&!peekAsked[d.id]){peekAsked[d.id]=1;post({do:'peek',id:d.id})}\n"
    "        if(d.replaceVersion)b.push(btn(k+'-replace','Replace '+(d.replaceVersion=='?'?'the installed app':'version '+d.replaceVersion),confirmFirst(k+'-replace',function(){post({do:'extract-install',id:d.id,replace:1})}),'primary'));\n"
    "        if(d.password&&d.jobState==3)b.push(btn(k+'-pw','Enter the password',function(){var pw=askPassword(d.file,d.password);if(pw)post({do:d.passwordDo,id:d.id,password:pw})},'primary'));\n"
    "        if(d.peeking)detail+=' \\u00b7 looking inside\\u2026';else if(p&&d.jobState!=2&&!d.replaceVersion)detail+=' \\u00b7 '+peekText(p);\n"
    "        if(!(p&&p.refused)){var acts=['extract','extract-install'];if(p&&p.titleId&&p.titles==1&&p.icon&&S.settings.imagesOK!==false)acts.push('extract-image','extract-image-ffpkg');else if(p&&!p.titleId&&!p.titles)acts=['extract'];if(sg&&acts.indexOf(sg.after)>0){acts.splice(acts.indexOf(sg.after),1);acts.unshift(sg.after)}\n"
    "          acts.forEach(function(x){var on=sg&&sg.after==x&&d.jobState!=2&&!d.replaceVersion;b.push(btn(k+'-'+x,afterNames[x]+(on?' (suggested)':''),function(){post({do:x,id:d.id})},on?'primary':''))})}}\n"
    "      if(isImg(d.file)||isPkg(d.file)){var fp=d.dir+'/'+d.file;wantInfo(fp);var fi=info[fp]||{};detail+='';if(!(fi.package&&!fi.fake&&fi.platform=='PS5'))b.push(btn(k+'-inst',d.replaceVersion?'Replace the installed app':isPkg(d.file)?'Install the package':'Install the app',d.replaceVersion?confirmFirst(k+'-inst',function(){post({do:'install',id:d.id,replace:1})}):function(){post({do:'install',id:d.id})},d.jobState==2?'':'primary'))}\n"
    "      b.push(btn(k+'-show','Show in Files',function(){tab='files';openFolder(d.dir)}));b.push(btn(k+'-forget','Remove from list',function(){post({do:'forget',id:d.id})}));\n"
    "      b.push(btn(k+'-del','Delete file',confirmFirst(k+'-del',function(){post({do:'delete',id:d.id})})))}}\n"
    "  var h='<div class=\"card\"><div class=\"row\"><div class=\"grow\"><div class=\"name\">'+esc(d.file||d.url)+'</div><div class=\"sub\">'+esc(short(d.dir))+' \\u00b7 '+esc(d.host)+'</div></div><div class=\"state '+cls+'\">'+esc(st)+'</div></div>';\n"
    "  if(pc>=0)h+='<div class=\"bar\"><div style=\"width:'+pc.toFixed(1)+'%\"></div></div>';\n"
    "  if(d.state!='done'&&d.after)detail+=(detail?' \\u00b7 ':'')+'then '+afterLabel(d.after);\n"
    "  if(d.state=='done'&&(isImg(d.file)||isPkg(d.file)))h+=infoHTML(d.dir+'/'+d.file);\n"
    "  return h+'<div class=\"sub '+(cls=='warn'?'warn':'')+'\">'+esc(detail)+'</div>'+buttons(b)+'</div>'}\n"
    "function emptyOK(f){return !f||confirm('Its folder '+f+'/ is empty: if the app wants files put there, install it as a folder instead (an image is read-only). Make the image anyway?')}\n"
    "function askPassword(name,why){return prompt((why=='wrong'?'That wasn\\u2019t its password. ':'')+'The password for '+name+':')}\n"
    "function taskCard(t){var k=t.id,base=t.src.split('/').pop(),st='',cls='',detail='',b=[];var what={extract:t.then=='install'?'Unpack & install':t.then=='image'?'Unpack & make image':'Unpack',install:'Install',image:'Make image','delete':'Delete',move:'Move',copy:'Copy'}[t.kind];\n"
    "  if(t.state==1){var p=t.total>0?Math.min(100,t.done*100/t.total):-1;st=({extract:'Unpacking',install:'Installing',image:'Making image',verify:'Checking the image',move:'Moving',copy:'Copying',delete:'Deleting'}[t.phase||t.kind]||'Working')+(p>=0?' '+Math.floor(p)+'%':'\\u2026');detail=what;if(t.kind!='delete')b.push(btn(k+'-stop','Stop',function(){post({do:'taskstop',id:t.id})}))}\n"
    "  else{st=t.state==2?'Done':t.replaceVersion?'Installed already':t.password?(t.password=='wrong'?'Wrong password':'Needs its password'):'Failed';cls=t.state==2?'ok':'warn';detail=t.state==2?(t.titleName&&t.kind=='install'&&/\\.pkg$/i.test(t.src||'')?'The console is installing \\u201c'+t.titleName+'\\u201d':t.titleName&&t.kind=='install'&&/\\.(exfat|ffpkg)$/i.test(t.src||'')?'\\u201c'+t.titleName+'\\u201d handed to the image mounter':t.titleName?'\\u201c'+t.titleName+'\\u201d is on the home screen':t.kind=='delete'?'Deleted':t.kind=='move'?'Moved to '+short(t.out||''):t.kind=='copy'?'Copied to '+short(t.out||''):'Made '+short(t.out||''))+(t.error?' \\u00b7 '+t.error:''):(t.error||'');\n"
    "    if(t.replaceVersion)b.push(btn(k+'-replace','Replace '+(t.replaceVersion=='?'?'the installed app':'version '+t.replaceVersion),confirmFirst(k+'-replace',function(){post({do:'task',kind:t.kind=='install'?'install':'extract-install',path:t.src,replace:1})}),'primary'));\n"
    "    if(t.password)b.push(btn(k+'-pw','Enter the password',function(){var pw=askPassword(base,t.password);if(pw){post({do:'taskremove',id:t.id});post({do:'task',kind:t.passwordDo,path:t.src,password:pw})}},'primary'));\n"
    "    b.push(btn(k+'-rm','Clear',function(){post({do:'taskremove',id:t.id})}))}\n"
    "  var h='<div class=\"card\"><div class=\"row\"><div class=\"grow\"><div class=\"name\">'+esc(what+': '+base)+'</div><div class=\"sub\">'+esc(short(t.src))+'</div></div><div class=\"state '+cls+'\">'+esc(st)+'</div></div>';\n"
    "  if(t.state==1&&t.total>0)h+='<div class=\"bar\"><div style=\"width:'+(t.done*100/t.total).toFixed(1)+'%\"></div></div>';\n"
    "  return h+'<div class=\"sub '+cls+'\">'+esc(detail)+'</div>'+buttons(b)+'</div>'}\n"
    "/* ---- files ---- */\n"
    "function openFolder(p){fs.error='';fs.newName=null;fs.rename=null;if(!p){fs.path=null;fs.data=null;getj('/dlm/fs?op=roots').then(function(j){fs.roots=j.ok?j.result:[];if(!j.ok)fs.error=j.error;render()}).catch(function(){});render();return}\n"
    "  fs.path=p;getj('/dlm/fs?op=ls&path='+encodeURIComponent(p)).then(function(j){if(j.ok)fs.data=j.result;else{fs.error=j.error;fs.data=null}render()}).catch(function(){});render()}\n"
    "function parent(p){var i=p.lastIndexOf('/');var r=p.slice(0,i);return /^\\/(data|mnt\\/(usb|ext)\\d)?$/.test(p)||!r||r=='/mnt'?null:r}\n"
    "function filesView(){var h='',b=[];\n"
    "  if(fs.pick){var d=(S.downloads||[]).filter(function(x){return x.id==fs.pick.id})[0];h+='<div class=\"card ask\">Choose a folder for <b>'+esc(d?d.file:'the download')+'</b>, then \\u201cSave here\\u201d.'+buttons([\n"
    "    btn('pick-here','Save here',function(){if(fs.path&&ask[fs.pick.id]){ask[fs.pick.id].dir=fs.path}fs.pick=null;tab='list';render()},fs.path?'primary':''),btn('pick-cancel','Back',function(){fs.pick=null;tab='list';render()})])+'</div>'}\n"
    "  if(fs.transfer){var t=fs.transfer,here=fs.path&&fs.path!=parent(t.src)&&fs.path!=t.src&&fs.path.indexOf(t.src+'/')!=0,verb=t.kind=='move'?'Move':'Copy';\n"
    "    h+='<div class=\"card ask\">'+verb+' <b>'+esc(t.name)+'</b>: open the folder it should go into, then \\u201c'+verb+' here\\u201d.'+buttons([\n"
    "    btn('tr-here',verb+' here',function(){if(!here)return;post({do:'task',kind:t.kind,path:t.src,dest:fs.path});fs.transfer=null;render()},here?'primary':''),\n"
    "    btn('tr-cancel','Cancel',function(){fs.transfer=null;render()})])+'</div>'}\n"
    "  if(!fs.path){h+='<div class=\"card\">';if(!fs.roots)h+='<div class=\"empty\">Looking for drives\\u2026</div>';\n"
    "    (fs.roots||[]).forEach(function(r,i){h+='<div class=\"entry\"><div class=\"grow\"><div class=\"name\">'+esc(r.label)+'</div><div class=\"sub\">'+esc(r.path)+' \\u00b7 '+size(r.free)+' free of '+size(r.size)+(r.fs?' \\u00b7 '+esc(r.fs):'')+'</div></div>'+buttons([btn('root'+i,'Open',function(){openFolder(r.path)})])+'</div>'});\n"
    "    if(S.settings.favorites.length){h+='<div class=\"entry\"><div class=\"grow name\">Favourites</div>'+buttons(S.settings.favorites.map(function(f,i){return btn('rf'+i,'\\u2605 '+short(f),function(){openFolder(f)})}))+'</div>'}\n"
    "    h+='<div class=\"entry\"><div class=\"grow name\">This user\\u2019s Downloads</div>'+buttons([btn('ru','Open',function(){openFolder(S.settings.userFolder)})])+'</div>';\n"
    "    return h+(fs.error?'<div class=\"warn\">'+esc(fs.error)+'</div>':'')+'</div>'}\n"
    "  var fav=S.settings.favorites.indexOf(fs.path)>=0,up=parent(fs.path);\n"
    "  h+='<div class=\"card\"><div class=\"name\">'+esc(short(fs.path))+'</div><div class=\"sub\">'+esc(fs.path)+(fs.data&&fs.data.free!=null?' \\u00b7 '+size(fs.data.free)+' free':'')+'</div>'+buttons([\n"
    "    btn('up',up?'\\u2191 Up':'\\u2191 Drives',function(){openFolder(up)}),btn('fav',(fav?'\\u2605 Favourite':'\\u2606 Add to favourites'),function(){post({do:'favorite',path:fs.path,on:fav?0:1})}),\n"
    "    btn('newdir','New folder',function(){fs.newName='';render();setTimeout(function(){var i=$('newname');if(i)i.focus()},0)}),btn('deffolder','Save downloads here by default',function(){post({do:'settings',folder:fs.path}).then(function(j){if(j&&j.ok)say('New downloads go to '+short(fs.path))})},S.settings.folder==fs.path?'on':'')]);\n"
    "  if(fs.newName!=null)h+='<div class=\"seg\"><input id=\"newname\" data-k=\"newname\" placeholder=\"Folder name\" value=\"'+esc(fs.newName)+'\" oninput=\"fs.newName=this.value\">'+bare([btn('mk','Make it',function(){post({do:'mkdir',path:fs.path,name:fs.newName}).then(function(j){if(j&&j.ok){openFolder(j.result.path)}})},'primary'),btn('mkno','Cancel',function(){fs.newName=null;render()})])+'</div>';\n"
    "  h+='</div><div class=\"card\">';\n"
    "  if(fs.error)h+='<div class=\"warn\">'+esc(fs.error)+'</div>';else if(!fs.data)h+='<div class=\"empty\">Reading the folder\\u2026</div>';else if(!fs.data.entries.length)h+='<div class=\"empty\">This folder is empty.</div>';\n"
    "  (fs.data?fs.data.entries:[]).forEach(function(e,i){var p=fs.path+'/'+e.name,k='e'+i+'-'+e.name.length,b=[];\n"
    "    if(e.dir){b.push(btn(k+'-open','Open',function(){openFolder(p)},'primary'));if(e.app){b.push(btn(k+'-inst','Install app',function(){post({do:'task',kind:'install',path:p})}));if(e.icon&&S.settings.imagesOK!==false){wantInfo(p);var ef=(info[p]||{}).emptyFolder;b.push(btn(k+'-img','Make exFAT image',function(){if(emptyOK(ef))post({do:'task',kind:'image',path:p})}));b.push(btn(k+'-imgu','Make UFS2 image (.ffpkg)',function(){if(emptyOK(ef))post({do:'task',kind:'image-ffpkg',path:p})}))}}}\n"
    "    else if(e.image||e.pkg){wantInfo(p);var fi=info[p]||{};if(!(fi.package&&!fi.fake&&fi.platform=='PS5'))b.push(btn(k+'-inst',e.pkg?'Install the package':'Install the app',function(){post({do:'task',kind:'install',path:p})},'primary'))}\n"
    "    else if(e.archive){b.push(btn(k+'-x','Unpack',function(){post({do:'task',kind:'extract',path:p})}));b.push(btn(k+'-xi','Unpack & install app',function(){post({do:'task',kind:'extract-install',path:p})}));if(S.settings.imagesOK!==false){b.push(btn(k+'-xm','Unpack & make image',function(){post({do:'task',kind:'extract-image',path:p})}));b.push(btn(k+'-xu','Unpack & make UFS2 image',function(){post({do:'task',kind:'extract-image-ffpkg',path:p})}))}}\n"
    "    {b.push(btn(k+'-mv','Move\\u2026',function(){fs.transfer={kind:'move',src:p,name:e.name};render()}));b.push(btn(k+'-cp','Copy\\u2026',function(){fs.transfer={kind:'copy',src:p,name:e.name};render()}));b.push(btn(k+'-ren','Rename',function(){fs.rename={path:p,name:e.name};render();setTimeout(function(){var x=$('rename');if(x)x.focus()},0)}));b.push(btn(k+'-del','Delete',confirmFirst(k+'-del',function(){post({do:'task',kind:'delete',path:p})})))}\n"
    "    h+='<div class=\"entry\"><span class=\"tag\">'+(e.dir?(e.app?'APP':'DIR'):e.image?'IMG':esc((e.name.split('.').pop()||'').slice(0,4).toUpperCase()))+'</span><div class=\"grow\"><div class=\"name\">'+esc(e.name)+'</div><div class=\"sub\">'+(e.dir?(e.app?'App folder':'Folder'):size(e.size))+' \\u00b7 '+new Date(e.mtime*1000).toLocaleString()+'</div>'+(e.image||e.pkg?infoHTML(p):'')+'</div>';\n"
    "    if(fs.rename&&fs.rename.path==p)h+='<div class=\"seg\"><input id=\"rename\" data-k=\"rename\" value=\"'+esc(fs.rename.name)+'\" oninput=\"fs.rename.name=this.value\">'+bare([btn(k+'-rok','Rename',function(){post({do:'rename',path:p,name:fs.rename.name}).then(function(){openFolder(fs.path)})},'primary'),btn(k+'-rno','Cancel',function(){fs.rename=null;render()})])+'</div>';\n"
    "    h+=buttons(b)+'</div>'});\n"
    "  return h+'</div>'}\n"
    "/* ---- settings ---- */\n"
    "function settingsView(){var s=S.settings,h='<div class=\"card\"><div class=\"name\">Connections per download</div><p class=\"help\">More connections are faster on servers that limit each one. Some servers allow only a few; the browser uses fewer when they refuse. Servers that can\\u2019t resume use one.</p>';\n"
    "  h+=buttons([1,2,4,8,12,16].map(function(n){return btn('c'+n,String(n),function(){post({do:'settings',connections:n})},s.connections==n?'on':'')}))+'</div>';\n"
    "  h+='<div class=\"card\"><div class=\"name\">Ask where to save each download</div><p class=\"help\">Off: downloads go straight to '+esc(short(s.folder))+'.</p>'+buttons([btn('ask1','Ask',function(){post({do:'settings',ask:1})},s.ask?'on':''),btn('ask0','Don\\u2019t ask',function(){post({do:'settings',ask:0})},s.ask?'':'on')])+'</div>';\n"
    "  h+='<div class=\"card\"><div class=\"name\">Default folder</div><p class=\"help\">'+esc(s.folder)+'</p>'+buttons([btn('dfu','Use this user\\u2019s Downloads',function(){post({do:'settings',folder:s.userFolder})},s.folder==s.userFolder?'on':''),btn('dfb','Choose in Files\\u2026',function(){tab='files';openFolder(null)})])+'</div>';\n"
    "  h+='<div class=\"card\"><div class=\"name\">After unpacking</div>'+buttons([btn('del0','Keep the archive',function(){post({do:'settings',deleteArchives:0})},s.deleteArchives?'':'on'),btn('del1','Delete the archive (all its parts)',function(){post({do:'settings',deleteArchives:1})},s.deleteArchives?'on':'')])+'</div>';\n"
    "  h+='<p class=\"help\">Unpacks ZIP, 7z (not BCJ2) and RAR (RAR4 and RAR5, also split and multipart sets; keep all parts in one folder). Archives with passwords aren\\u2019t supported. An installed app goes to the drive\\u2019s homebrew folder, where ShadowMountPlus, Kraken Mount or TITAN-CORE adds it to the home screen (without one, kstuff mounts it when it next starts). exFAT and UFS2 images go there too, as TITLEID.exfat or TITLEID.ffpkg (UFS2 needs ShadowMountPlus 1.7), for ShadowMountPlus or TITAN-CORE'+(S.settings.imagesOK===false?' (neither is running now)':'')+'; a downloaded image installs the same way. Packages (.pkg: PS4, and PS5 fake packages) go to the console\\u2019s installer, which copies them from the browser: keep it open until they are copied.</p>';\n"
    "  return h}\n"
    "function render(){if(!S)return;handlers={};var f=document.activeElement&&document.activeElement.getAttribute('data-k');\n"
    "  var asking=(S.downloads||[]).filter(function(d){return d.state=='asking'});$('asking').innerHTML=tab=='files'&&fs.pick?'':asking.map(askCard).join('');\n"
    "  var n=(S.downloads||[]).filter(function(d){return d.state=='running'}).length;\n"
    "  $('tabs').innerHTML=bare([btn('t-list','Downloads'+(n?' ('+n+')':''),function(){tab='list';render()},tab=='list'?'on':''),btn('t-files','Files',function(){tab='files';if(!fs.data&&!fs.roots)openFolder(fs.path);else render()},tab=='files'?'on':''),\n"
    "    btn('t-set','Settings',function(){tab='settings';render()},tab=='settings'?'on':'')]);\n"
    "  var h='';if(tab=='list'){var tasks=S.tasks||[];if(tasks.length)h+='<h2>Jobs</h2>'+tasks.map(taskCard).join('');\n"
    "    var lost=partialsLost();if(lost.length)h+='<h2>Unfinished downloads not in the list</h2>'+lost.map(partialCard).join('');var rest=(S.downloads||[]).filter(function(d){return d.state!='asking'});h+='<h2>Downloads</h2>'+(rest.length?rest.map(dlCard).join(''):'<div class=\"card empty\">No downloads yet. Files you download from pages show up here.</div>')}\n"
    "  else if(tab=='files')h=filesView();else h=settingsView();\n"
    "  $('view').innerHTML=h;\n"
    "  document.querySelectorAll('button[data-k]').forEach(function(b){var k=b.getAttribute('data-k');if(handlers[k])b.onclick=function(ev){if(armed&&armed!=k)armed='';handlers[k](ev)}});\n"
    "  var t=f&&document.querySelector('[data-k=\"'+f.replace(/\"/g,'')+'\"]');if(t)t.focus();else if(tv&&!document.activeElement.getAttribute('data-k')){var first=document.querySelector('#asking button,#view button');if(first)first.focus()}}\n"
    "var loaded=false;function load(){getj('/dlm.json').then(function(j){$('pinbox').hidden=true;if(j.ok===false){say(j.error);return}S=j;\n"
    "  var dz=(j.tasks||[]).filter(function(t){return (t.kind=='delete'||t.kind=='move'||t.kind=='copy')&&t.state!=1}).map(function(t){return t.id}).join();if(deletesSeen!==null&&dz!=deletesSeen&&tab=='files'&&fs.path)openFolder(fs.path);deletesSeen=dz;\n"
    "  if(tab=='list'&&Date.now()-partialsAt>15000)loadPartials();\n"
    "  if(!loaded){loaded=true;if(tab=='files'&&!fs.roots&&!fs.path){openFolder(q.get('path'));return}}render()}).catch(function(){})}\n"
    "function closePage(){if(!tv){history.back();return}fetch('/browser/page',{method:'POST',headers:hdr(true),body:'action=close'}).then(function(r){return r.json()}).then(function(j){if(j.ok===false)history.back()},function(){history.back()})}\n"
    "$('back').hidden=!tv;$('back').onclick=closePage;\n"
    "$('pinok').onclick=function(){pin=$('pin').value.trim();try{sessionStorage.setItem('ps5pin',pin)}catch(e){}load()};\n"
    "load();setInterval(function(){if(!document.hidden&&!(document.activeElement&&document.activeElement.tagName=='INPUT'))load()},1000);\n"
    "</script></body></html>\n";
