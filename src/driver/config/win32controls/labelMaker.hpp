#pragma once

#include "../../../util/windowsLean.hpp"
#include <string>

/*
Create a static text label. horizOffset and width default to the original
left-column layout so existing call sites are unaffected.
*/
HWND labelMaker(HWND form,
                int vertOffset,
                int labelId,
                std::string labelText,
                bool visible,
                int horizOffset = 10,
                int width       = 145);
