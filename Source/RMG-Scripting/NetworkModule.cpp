/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "NetworkModule.hpp"
#include "ScriptEngine.hpp"

#include <quickjs.h>
#include <curl/curl.h>

#include <string>
#include <thread>
#include <vector>
#include <utility>

namespace RMGScript {

static size_t CurlWriteCallback(void* data, size_t size, size_t nmemb, std::string* out)
{
    out->append(static_cast<char*>(data), size * nmemb);
    return size * nmemb;
}

struct FetchRequest {
    std::string url;
    std::string method = "GET";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct FetchResponse {
    long        status = 0;
    bool        ok     = false;
    std::string body;
    std::string headers;
    std::string error; // non-empty on curl failure
};

// Parse fetch(url [, options]) arguments into a FetchRequest.
static bool ParseFetchArgs(JSContext* ctx, int argc, JSValue* argv, FetchRequest& req)
{
    if (argc < 1) {
        JS_ThrowTypeError(ctx, "fetch(url[, options])");
        return false;
    }

    const char* url = JS_ToCString(ctx, argv[0]);
    if (!url) return false;
    req.url = url;
    JS_FreeCString(ctx, url);

    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue opt = argv[1];

        JSValue mval = JS_GetPropertyStr(ctx, opt, "method");
        if (!JS_IsUndefined(mval)) {
            const char* m = JS_ToCString(ctx, mval);
            if (m) { req.method = m; JS_FreeCString(ctx, m); }
        }
        JS_FreeValue(ctx, mval);

        JSValue bval = JS_GetPropertyStr(ctx, opt, "body");
        if (!JS_IsUndefined(bval)) {
            const char* b = JS_ToCString(ctx, bval);
            if (b) { req.body = b; JS_FreeCString(ctx, b); }
        }
        JS_FreeValue(ctx, bval);

        JSValue hval = JS_GetPropertyStr(ctx, opt, "headers");
        if (JS_IsObject(hval)) {
            JSPropertyEnum* props = nullptr;
            uint32_t len = 0;
            if (JS_GetOwnPropertyNames(ctx, &props, &len, hval,
                                       JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < len; i++) {
                    const char* key = JS_AtomToCString(ctx, props[i].atom);
                    JSValue vv = JS_GetProperty(ctx, hval, props[i].atom);
                    const char* val = JS_ToCString(ctx, vv);
                    if (key && val) req.headers.emplace_back(key, val);
                    JS_FreeCString(ctx, val);
                    JS_FreeValue(ctx, vv);
                    JS_FreeCString(ctx, key);
                    JS_FreeAtom(ctx, props[i].atom);
                }
                js_free(ctx, props);
            }
        }
        JS_FreeValue(ctx, hval);
    }

    return true;
}

// Execute a FetchRequest synchronously (blocks the calling thread).
static FetchResponse DoFetch(const FetchRequest& req)
{
    FetchResponse resp;

    CURL* curl = curl_easy_init();
    if (!curl) { resp.error = "curl_easy_init failed"; return resp; }

    curl_easy_setopt(curl, CURLOPT_URL,            req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,  1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,  5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,         10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,       &resp.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,  CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA,      &resp.headers);

    if (req.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST,          1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    req.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
    } else if (req.method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());
        if (!req.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    req.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
        }
    }

    struct curl_slist* headerList = nullptr;
    for (auto& [k, v] : req.headers)
        headerList = curl_slist_append(headerList, (k + ": " + v).c_str());
    if (headerList)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
        resp.ok = (resp.status >= 200 && resp.status < 300);
    } else {
        resp.error = curl_easy_strerror(rc);
    }

    if (headerList) curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    return resp;
}

// fetch(url [, options]) → { status, ok, body, headers, json() }
// Blocking — use fetchAsync() inside on_frame to avoid stalling emulation.
static JSValue JS_Fetch(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    FetchRequest req;
    if (!ParseFetchArgs(ctx, argc, argv, req)) return JS_EXCEPTION;

    FetchResponse resp = DoFetch(req);

    if (!resp.error.empty())
        return JS_ThrowInternalError(ctx, "fetch: %s", resp.error.c_str());

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "status",  JS_NewInt32(ctx, (int)resp.status));
    JS_SetPropertyStr(ctx, obj, "ok",      JS_NewBool(ctx, resp.ok));
    JS_SetPropertyStr(ctx, obj, "body",    JS_NewStringLen(ctx, resp.body.c_str(), resp.body.size()));
    JS_SetPropertyStr(ctx, obj, "headers", JS_NewStringLen(ctx, resp.headers.c_str(), resp.headers.size()));
    JS_SetPropertyStr(ctx, obj, "json",
        JS_NewCFunction(ctx,
            [](JSContext* c, JSValue thisVal, int, JSValue*) -> JSValue {
                JSValue bodyStr = JS_GetPropertyStr(c, thisVal, "body");
                size_t len;
                const char* s = JS_ToCStringLen(c, &len, bodyStr);
                JSValue parsed = s ? JS_ParseJSON(c, s, len, "<fetch>") : JS_UNDEFINED;
                JS_FreeCString(c, s);
                JS_FreeValue(c, bodyStr);
                return parsed;
            },
            "json", 0));
    return obj;
}

// fetchAsync(url [, options])
// Fires the request on a background thread and returns immediately.
// Use this inside on_frame callbacks to avoid blocking emulation.
static JSValue JS_FetchAsync(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    FetchRequest req;
    if (!ParseFetchArgs(ctx, argc, argv, req)) return JS_EXCEPTION;

    ScriptOutputCallbackFunc cb;
    ScriptEngine* engine = GetEngineFromContext(ctx);
    if (engine) cb = engine->CopyOutputCallback();

    std::thread([req = std::move(req), cb]() {
        FetchResponse resp = DoFetch(req);
        if (!resp.error.empty() && cb)
            cb("[fetchAsync error] " + resp.error);
    }).detach();

    return JS_UNDEFINED;
}

void RegisterNetworkModule(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "fetch",
        JS_NewCFunction(ctx, JS_Fetch,      "fetch",      1));
    JS_SetPropertyStr(ctx, global, "fetchAsync",
        JS_NewCFunction(ctx, JS_FetchAsync, "fetchAsync", 1));
    JS_FreeValue(ctx, global);
}

} // namespace RMGScript
