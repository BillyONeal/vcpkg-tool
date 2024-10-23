#pragma once

#include <vcpkg/base/files.h>

#include <vector>

namespace vcpkg
{
    struct OverlayPortPaths
    {
        std::vector<Path> overlay_port_dirs;
        std::vector<Path> overlay_ports;

        bool empty() const noexcept { return overlay_port_dirs.empty() && overlay_ports.empty(); }
    };
}
