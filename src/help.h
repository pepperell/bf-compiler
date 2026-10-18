// help.h - the in-app help window (topics on the left, text on the right).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>

namespace help {

enum Topic { START = 0, SYMBOLS, MEMORY_MAP, DEVICES, DEBUGGER, TERMINAL, COMMAND_LINE, LANGUAGE, TOPIC_COUNT };

struct Entry { const wchar_t* title; std::wstring text; };
const std::vector<Entry>& topics();          // built once; the memory map comes from registers.h

// Show (or raise) the help window on the given topic.
void show(HWND owner, HINSTANCE inst, HFONT mono, HFONT ui, Topic topic);
// Dark or light colours for the help window (View > Theme), now and when it is created.
void set_dark(bool dark);

} // namespace help
