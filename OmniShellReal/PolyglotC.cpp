// =================================================================
// PolyglotC.cpp
// =================================================================
#include "PolyglotC.h"
#include "ShellExecutor.h"
#include "tinyxml2.h"
#include <sstream>
#include <iostream>

std::string PolyglotC::buildFromXml(const std::string& xmlFile) {
    using namespace tinyxml2;
    XMLDocument doc;
    if (doc.LoadFile(xmlFile.c_str()) != XML_SUCCESS) {
        return "Error: Could not load build file " + xmlFile;
    }

    std::ostringstream result;
    XMLElement* root = doc.FirstChildElement("PolyglotBuild");
    if (!root) return "Error: Malformed build file.";

    for (XMLElement* mod = root->FirstChildElement("Module"); mod; mod = mod->NextSiblingElement("Module")) {
        const char* lang = mod->FirstChildElement("Language")->GetText();
        const char* src = mod->FirstChildElement("Source")->GetText();

        std::string compilerFlags = "";
        XMLElement* cflags = mod->FirstChildElement("CompilerFlags");
        if (cflags) compilerFlags = cflags->GetText();

        std::string linkerFlags = "";
        XMLElement* lflags = mod->FirstChildElement("LinkerFlags");
        if (lflags) linkerFlags = lflags->GetText();

        if (lang && src) {
            std::string langStr = lang;
            std::string srcStr = src;
            std::string command;
            if (langStr == "cpp") {
                command = "cl.exe " + compilerFlags + " " + srcStr + " /link " + linkerFlags;
                result << ShellExecutor::run(command) << "\n";
            }
            else if (langStr == "zig") {
                command = "zig build-exe " + compilerFlags + " " + srcStr + " " + linkerFlags;
                result << ShellExecutor::run(command) << "\n";
            }
        }
    }
    return result.str();
}