#pragma once

#include "../../../util/windowsLean.hpp"
#include <string>

/*
Create a push button. vertOffset defaults to the original position so existing
call sites are unaffected; the DSN form passes a larger value now that the form
is taller.
*/
HWND buttonMaker(HWND form,
                 int horizOffset,
                 int buttonId,
                 std::string buttonText,
                 int vertOffset = 300);
