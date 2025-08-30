Copyright © 2025 Cadell Richard Anderson

#pragma once
// web_fetcher.h
// A simple, self-contained module for fetching content from a URL using WinHTTP
// and optionally rendering it with the MSHTML COM engine (virtual browser).
#include <string>
#include <vector>

#if defined(_WIN32)
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
#include <windows.h>
#include <winhttp.h>
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
