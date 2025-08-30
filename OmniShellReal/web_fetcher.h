Copyright © 2025 Cadell Richard Anderson

#pragma once
// web_fetcher.h
// A simple, self-contained module for fetching content from a URL using WinHTTP
// and optionally rendering it with the MSHTML COM engine (virtual browser).
#include <string>
#include <vector>

#if defined(_WIN32)
// FIX: Define minimum Windows version before including windows.h
// This ensures modern functions like WinHttpAddHeaders are available.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 // Windows 7 or newer
#endif
#include <windows.h> // Required for WinHTTP types
#include <winhttp.h> // FIX #1: Added this include. It declares WinHttpAddHeaders and other functions.
#endif

namespace web {

    struct FetchResult {
        bool success = false;
        unsigned long statusCode = 0;
        std::vector<char> body;
        std::wstring headers;
        std::vector<std::wstring> links;
        std::wstring errorMessage;
    };

    // FIX #2: Added the declaration for fetch_url so other files like CommandRouter.cpp can see it.
    FetchResult fetch_url(const std::wstring& url_in);

    // Renders a URL using the MSHTML COM engine to handle JavaScript.
    FetchResult render_url(const std::wstring& url_in);

    // Fetches data from an API endpoint using WinHTTP (no JS execution).
    FetchResult fetchApiData(const std::wstring& url,
        const std::string& verb = "GET",
        const std::string& postData = "");

    // Scans a webpage, finds a link containing specific text, and downloads the linked file.
    FetchResult DownloadLink(const std::wstring& pageUrl,
        const std::wstring& linkIdentifier,
        const std::wstring& savePath);

} // namespace web
