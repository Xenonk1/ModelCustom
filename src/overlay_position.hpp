#pragma once

#include <Windows.h>
#include <algorithm>

namespace modelcustom::overlay
{


struct PanelPosition
{
    float x = 0, y = 0;
    bool initialized = false;

    void move_to_screen(POINT screen, POINT origin, float scale)
    {
        x = (screen.x - origin.x) / scale;
        y = (screen.y - origin.y) / scale;
        initialized = true;
    }

    POINT resolve(POINT origin, int clientWidth, int clientHeight, int width, int height, float scale)
    {
        const int maxX = std::max(0, clientWidth - width);
        const int maxY = std::max(0, clientHeight - height);
        if (!initialized)
        {
            x = (maxX / 2) / scale;
            y = (maxY / 2) / scale;
            initialized = true;
        }
        x = std::clamp(x, 0.f, maxX / scale);
        y = std::clamp(y, 0.f, maxY / scale);
        return { origin.x + static_cast<LONG>(x * scale), origin.y + static_cast<LONG>(y * scale) };
    }
};
}
