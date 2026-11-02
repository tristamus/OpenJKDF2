#include "sithSoundClassSwim.h"

#include <stddef.h>

// Added: see sithSoundClassSwim.h

static int sithSoundClassSwim_IsStroke2(const char* pName)
{
    static const char aWant[] = "stroke2.wav";
    size_t i;

    if (!pName)
        return 0;
    for (i = 0; aWant[i]; i++)
    {
        char c = pName[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != aWant[i])
            return 0;
    }
    return pName[i] == '\0';
}

int sithSoundClassSwim_NeedsLeftStroke(const char* pLeft, int numLeft, const char* pRight, int numRight)
{
    return numLeft == 1 && numRight == 1
        && sithSoundClassSwim_IsStroke2(pLeft)
        && sithSoundClassSwim_IsStroke2(pRight);
}
