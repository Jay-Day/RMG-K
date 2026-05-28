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

#include <quickjs.h>
#include <curl/curl.h>

#include <string>
#include <vector>
#include <utility>

namespace RMGScript {

static size_t CurlWriteCallback(void* data, size_t size, size_t nmemb, std::string* out)
{
    out->append(static_cast<char*>(data), size * nmemb);
    return size * nmemb;
}

// fetch(url [, options]) -> { status, ok, body, headers }
//
// options: {
//   method:  string          (default "GET")
//   body:    string          (request body)
//   headers: { key: value }  (extra request headers)
// }
static JSValue JS_Fetch(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "fetch(url[, options])");

    const char* url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;

    std::string method = "GET";
    std::string reqBody;
    std::vector<std::pair<std::string, std::string>> reqHeaders;

    // Parse options object
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue opt = argv[1];

        JSValue mval = JS_GetPropertyStr(ctx, opt, "method");
        if (!JS_IsUndefined(mval)) {
            const char* m = JS_ToCString(ctx, mval);
            if (m) { method = m; JS_FreeCString(ctx, m); }
        }
        JS_FreeValue(ctx, mval);

        JSValue bval = JS_GetPropertyStr(ctx, opt, "body");
        if (!JS_IsUndefined(bval)) {
            const char* b = JS_ToCString(ctx, bval);
            if (b) { reqBody = b; JS_FreeCString(ctx, b); }
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
                    if (key && val) reqHeaders.emplace_back(key, val);
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

    // ── libcurl request ──────────────────────────────────────────────────────

    CURL* curl = curl_easy_init();
    if (!curl) {
        JS_FreeCString(ctx, url);
        return JS_ThrowInternalError(ctx, "fetch: curl_easy_init failed");
    }

    std::string respBody;
    std::string respHeaders;
    long httpCode = 0;

    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &respBody);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA,     &respHeaders);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST,          1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    reqBody.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)reqBody.size());
    } else if (method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!reqBody.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    reqBody.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)reqBody.size());
        }
    }

    struct curl_slist* headerList = nullptr;
    for (auto& [k, v] : reqHeaders) {
        headerList = curl_slist_append(headerList, (k + ": " + v).c_str());
    }
    if (headerList)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    const char* curlErr = (res != CURLE_OK) ? curl_easy_strerror(res) : nullptr;

    if (headerList) curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    JS_FreeCString(ctx, url);

    if (curlErr)
        return JS_ThrowInternalError(ctx, "fetch: %s", curlErr);

    // ── Build response object ─────────────────────────────────────────────────

    JSValue resp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, resp, "status",  JS_NewInt32(ctx, (int)httpCode));
    JS_SetPropertyStr(ctx, resp, "ok",      JS_NewBool(ctx, httpCode >= 200 && httpCode < 300));
    JS_SetPropertyStr(ctx, resp, "body",    JS_NewStringLen(ctx, respBody.c_str(), respBody.size()));
    JS_SetPropertyStr(ctx, resp, "headers", JS_NewStringLen(ctx, respHeaders.c_str(), respHeaders.size()));
    // json() helper: parses body as JSON
    JS_SetPropertyStr(ctx, resp, "json",
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
    return resp;
}

void RegisterNetworkModule(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "fetch",
        JS_NewCFunction(ctx, JS_Fetch, "fetch", 1));
    JS_FreeValue(ctx, global);
}

} // namespace RMGScript
