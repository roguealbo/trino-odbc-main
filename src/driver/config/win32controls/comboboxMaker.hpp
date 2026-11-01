#pragma once

#include "../../../util/windowsLean.hpp"
#include <string>
#include <vector>

/*
Create a drop-down list. horizOffset and width default to the original
single-column layout so existing call sites are unaffected.
*/
HWND comboboxMaker(HWND form,
                   int vertOffset,
                   int comboboxId,
                   std::vector<std::string> labelNames,
                   int horizOffset = 165,
                   int width       = 415);

void setCombobox(HWND comboBox, std::string valueStr);
