//ScriptRunner.h


#pragma once


#include <string>
#include <vector>

class ScriptRunner {
public:
    static std::string runScript(const std::string& filename);

    // Original attachment version
    static bool sendEmail(const std::string& smtp_server, const std::string& port,
        const std::string& sender, const std::string& username, const std::string& password,
        const std::vector<std::string>& recipients,
        const std::string& subject, const std::string& body,
        const std::vector<std::string>& attachments = {});

    // NEW: Streaming attachments across all drives dynamically
    static bool sendEmailWithStreamingAttachments(const std::string& smtp_server,
        const std::string& port,
        const std::string& sender,
        const std::string& username,
        const std::string& password,
        const std::vector<std::string>& recipients,
        const std::string& subject,
        const std::string& body,
        const std::string& targetFilename,
        const std::vector<std::string>& exactAttachments = {}); // Optional known paths
};