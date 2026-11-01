#pragma once

#include "../../../util/windowsLean.hpp"
#include <string>

/*
Create a single-line text entry.

The x and width parameters default to the original single-column layout so
existing call sites are unaffected; the DSN form uses them to lay out a second
column of fields for the non-interactive authentication options.
*/
HWND editMaker(HWND form,
               int vertOffset,
               int editId,
               bool visible,
               int horizOffset = 165,
               int width       = 425);

/*
Create a text entry that masks what is typed. Used for the client secret and
password fields so a credential is not exposed on screen when a DSN is edited
on a shared gateway host.
*/
HWND passwordMaker(HWND form,
                   int vertOffset,
                   int editId,
                   bool visible,
                   int horizOffset = 165,
                   int width       = 425);

std::string getEditText(HWND edit);

void setEditText(HWND edit, std::string text);

void setEditReadOnly(HWND edit);

void setEditWriteable(HWND edit);
