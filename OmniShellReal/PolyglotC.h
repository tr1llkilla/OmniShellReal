Copyright © 2025 Cadell Richard Anderson

//PolyglotC.h - Class for building Polyglot C code from XML definitions

#pragma once
#include <string>

class PolyglotC {
public:
    static std::string buildFromXml(const std::string& xmlFile);
};
