#pragma once
#ifndef MEDIAACCESS_VERSION_H
#define MEDIAACCESS_VERSION_H

#define APP_VERSION "1.40"
#define APP_VERSION_MAJOR 1
#define APP_VERSION_MINOR 40

// This will be set during build from git commit
#ifndef BUILD_COMMIT
#define BUILD_COMMIT ""
#endif

#define GITHUB_REPO "reaperaccessible/MediaAccess"
#define GITHUB_API_URL "https://api.github.com/repos/reaperaccessible/MediaAccess/releases"

#endif // MEDIAACCESS_VERSION_H
