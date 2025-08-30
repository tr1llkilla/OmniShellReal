Copyright © 2025 Cadell Richard Anderson

// CloudError.h

#pragma once

namespace onecloud {

    // The single, authoritative definition for all cloud-related errors.
    enum class CloudError {
        Success = 0,
        FileExists = -1,
        ContainerNotFound = -2,
        IOError = -3,
        InvalidContainerFormat = -4,
        FileNotFound = -5,
        KeyDerivationFailed = -10,
        EncryptionFailed = -11,
        DecryptionFailed = -12,
        InvalidPassword = -13,
        OutOfMemory = -14,
        AccessDenied = -15,
        Unknown = -100
    };

} // namespace onecloud
