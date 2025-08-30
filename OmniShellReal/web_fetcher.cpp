// web_fetcher.cpp
// Implementation of the URL fetching functionality using an enhanced MSHTML virtual browser.

// =================================================================
// 1. CRITICAL: Platform-specific headers MUST come first to ensure
// all Windows API definitions are available before they are used.
// =================================================================

// FIX: Define the minimum Windows version required BEFORE including windows.h.
// This makes modern functions like WinHttpAddHeaders available to the compiler.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 // Target Windows 7 or newer
#endif

#include <windows.h>
#include <wininet.h>
#include <winhttp.h>
// FIX: Replaced includes that had invisible non-ASCII space characters.
#include <mshtml.h>
#include <exdisp.h>
#include <comdef.h>

// =================================================================
// 2. Project and Standard Library Headers
// =================================================================
#include "web_fetcher.h"
#include <vector>
#include <string>
#include <iostream>
#include <fstream> // Required for std::ofstream

// Link against required libraries.
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace web {

    // Helper to convert a multi-byte string (UTF-8) to a wide string.
    static std::wstring to_wstring(const std::string& str) {
        if (str.empty()) return std::wstring();
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
        std::wstring wstrTo(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
        return wstrTo;
    }

    // Helper to convert a wide string to a multi-byte string (UTF-8).
    static std::string to_string(const std::wstring& wstr) {
        if (wstr.empty()) return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

    // =================================================================
    // fetch_url: Delegates to render_url for robust webpage fetching.
    // =================================================================
    FetchResult fetch_url(const std::wstring& url_in) {
        return render_url(url_in);
    }

    // =================================================================
    // fetchApiData: Uses WinHTTP for high-performance, non-browser API communication.
    // =================================================================
    FetchResult fetchApiData(const std::wstring& url_in, const std::string& verb, const std::string& postData) {
        FetchResult result;
        URL_COMPONENTS urlComp;
        wchar_t szHostName[256]{};
        wchar_t szUrlPath[2048]{};

        std::wstring url = url_in;
        if (url.find(L"://") == std::wstring::npos) {
            url = L"https://" + url;
        }

        ZeroMemory(&urlComp, sizeof(urlComp));
        urlComp.dwStructSize = sizeof(urlComp);
        urlComp.lpszHostName = szHostName;
        urlComp.dwHostNameLength = _countof(szHostName);
        urlComp.lpszUrlPath = szUrlPath;
        urlComp.dwUrlPathLength = _countof(szUrlPath);
        urlComp.nScheme = INTERNET_SCHEME_DEFAULT;

        if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.length(), 0, &urlComp)) {
            result.errorMessage = L"[Web] Error: Invalid URL format.";
            return result;
        }

        HINTERNET hSession = WinHttpOpen(L"OmniShell/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) { result.errorMessage = L"Failed to open WinHTTP session"; return result; }

        WinHttpSetTimeouts(hSession, 30000, 30000, 30000, 30000);

        HINTERNET hConnect = WinHttpConnect(hSession, szHostName, urlComp.nPort, 0);
        if (!hConnect) { result.errorMessage = L"Failed to connect"; WinHttpCloseHandle(hSession); return result; }

        std::wstring wVerb = to_wstring(verb);
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, wVerb.c_str(), szUrlPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
        if (!hRequest) { result.errorMessage = L"Failed to open request"; WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return result; }

        LPCWSTR headers = L"Content-Type: application/json\r\nAccept: application/json";
        WinHttpAddRequestHeaders(hRequest, headers, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

        BOOL bResults = WinHttpSendRequest(hRequest,
            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            (LPVOID)postData.c_str(), (DWORD)postData.size(),
            (DWORD)postData.size(), 0);

        if (bResults && WinHttpReceiveResponse(hRequest, NULL))
        {
            DWORD dwStatusCode = 0;
            DWORD dwSize = sizeof(dwStatusCode);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &dwStatusCode, &dwSize, NULL);
            result.statusCode = dwStatusCode;

            do {
                dwSize = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                if (dwSize == 0) break;

                std::vector<char> buffer(dwSize);
                DWORD dwRead = 0;
                if (WinHttpReadData(hRequest, buffer.data(), dwSize, &dwRead)) {
                    result.body.insert(result.body.end(), buffer.begin(), buffer.begin() + dwRead);
                }
            } while (dwSize > 0);

            result.success = (result.statusCode >= 200 && result.statusCode < 300);
        }
        else {
            result.errorMessage = L"Failed to send/receive request.";
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        return result;
    }

    // =================================================================
    // render_url: Uses the MSHTML engine via COM to render a page and get the final HTML.
    // =================================================================
    FetchResult render_url(const std::wstring& url_in) {
        FetchResult result;

        std::wstring url = url_in;
        if (url.find(L"://") == std::wstring::npos) {
            url = L"https://" + url;
        }

        if (FAILED(CoInitialize(NULL))) {
            result.errorMessage = L"Error: Failed to initialize COM library.";
            return result;
        }

        IWebBrowser2* pBrowser = NULL;
        HRESULT hr = CoCreateInstance(CLSID_InternetExplorer, NULL, CLSCTX_LOCAL_SERVER, IID_IWebBrowser2, (void**)&pBrowser);

        if (SUCCEEDED(hr)) {
            pBrowser->put_Visible(VARIANT_FALSE);
            VARIANT vEmpty;
            VariantInit(&vEmpty);
            _bstr_t bstrUrl(url.c_str());
            hr = pBrowser->Navigate(bstrUrl, &vEmpty, &vEmpty, &vEmpty, &vEmpty);
            if (SUCCEEDED(hr)) {
                // FIX: Initialize local variable
                READYSTATE rs = READYSTATE_UNINITIALIZED;
                do {
                    pBrowser->get_ReadyState(&rs);
                    Sleep(100);
                } while (rs != READYSTATE_COMPLETE);

                IDispatch* pDocDisp = NULL;
                hr = pBrowser->get_Document(&pDocDisp);
                if (SUCCEEDED(hr) && pDocDisp) {
                    IHTMLDocument2* pDoc2 = NULL;
                    hr = pDocDisp->QueryInterface(IID_IHTMLDocument2, (void**)&pDoc2);
                    if (SUCCEEDED(hr) && pDoc2) {
                        // FIX: Initialize local variable
                        IHTMLElement* pElement = NULL;
                        hr = pDoc2->get_body(&pElement);
                        if (SUCCEEDED(hr) && pElement) {
                            BSTR html = NULL;
                            pElement->get_outerHTML(&html);
                            std::string html_str = to_string(std::wstring(html, SysStringLen(html)));
                            result.body.assign(html_str.begin(), html_str.end());
                            result.success = true;
                            result.statusCode = 200;
                            SysFreeString(html);
                            pElement->Release();
                        }
                        IHTMLElementCollection* pLinks = NULL;
                        hr = pDoc2->get_links(&pLinks);
                        if (SUCCEEDED(hr) && pLinks) {
                            long count = 0;
                            pLinks->get_length(&count);
                            for (long i = 0; i < count; i++) {
                                IDispatch* pLinkDisp = NULL;
                                VARIANT index{};
                                index.vt = VT_I4;
                                index.lVal = i;
                                hr = pLinks->item(index, index, &pLinkDisp);
                                if (SUCCEEDED(hr) && pLinkDisp) {
                                    IHTMLAnchorElement* pAnchor = NULL;
                                    hr = pLinkDisp->QueryInterface(IID_IHTMLAnchorElement, (void**)&pAnchor);
                                    if (SUCCEEDED(hr) && pAnchor) {
                                        BSTR href = NULL;
                                        pAnchor->get_href(&href);
                                        if (href) {
                                            result.links.push_back(std::wstring(href, SysStringLen(href)));
                                            SysFreeString(href);
                                        }
                                        pAnchor->Release();
                                    }
                                    pLinkDisp->Release();
                                }
                            }
                            pLinks->Release();
                        }
                        pDoc2->Release();
                    }
                    pDocDisp->Release();
                }
            }
            pBrowser->Quit();
            pBrowser->Release();
        }
        CoUninitialize();
        return result;
    }

    // =================================================================
    // NEW: Implementation to find a link and download its content
    // =================================================================
    FetchResult DownloadLink(const std::wstring& pageUrl, const std::wstring& linkIdentifier, const std::wstring& savePath) {
        FetchResult finalResult;

        std::wcout << L"[Web] Scanning page for links: " << pageUrl << std::endl;
        FetchResult pageData = render_url(pageUrl);

        if (!pageData.success) {
            finalResult.errorMessage = L"Failed to scan page for links. " + pageData.errorMessage;
            return finalResult;
        }

        std::wstring targetUrl;
        for (const auto& link : pageData.links) {
            if (link.find(linkIdentifier) != std::wstring::npos) {
                targetUrl = link;
                break;
            }
        }

        if (targetUrl.empty()) {
            finalResult.errorMessage = L"Could not find any link containing the text: " + linkIdentifier;
            return finalResult;
        }

        std::wcout << L"[Web] Found target URL. Downloading: " << targetUrl << std::endl;
        FetchResult fileData = fetchApiData(targetUrl);

        if (!fileData.success) {
            finalResult.errorMessage = L"Failed to download file. " + fileData.errorMessage;
            return finalResult;
        }

        std::wcout << L"[Web] Saving to file: " << savePath << std::endl;
        std::ofstream outFile(savePath, std::ios::binary);
        if (!outFile) {
            finalResult.errorMessage = L"Failed to open save path for writing: " + savePath;
            return finalResult;
        }

        outFile.write(fileData.body.data(), fileData.body.size());
        outFile.close();

        finalResult.success = true;
        finalResult.body = fileData.body;
        finalResult.statusCode = 200;
        finalResult.headers = L"Successfully downloaded " + std::to_wstring(fileData.body.size()) + L" bytes to " + savePath;
        return finalResult;
    }

} // namespace web