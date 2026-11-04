// The GUI lines' parser built as the firmware builds it, optimised with -ffast-math, on its own: in the
// tests' executable the linker could keep the parser built without it.
#include <cstdio>
#include <initializer_list>
#include "gui_lines.h"

int main()
{
    int accepted = 0;
    for (const char* bad : { "v 12 nan", "v 12 inf", "v 12 -inf", "g 3 nan", "n 60 nan", "b inf", "b nan", "m nan", "m -inf" })
        if (guiLines::parse (bad).kind != guiLines::Line::Kind::unknown)
        {
            std::printf ("FAIL fast-math: \"%s\" accepted\n", bad);
            ++accepted;
        }
    return accepted ? 1 : 0;
}
