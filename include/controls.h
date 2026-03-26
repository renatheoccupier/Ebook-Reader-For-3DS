#pragma once
#include "settings.h"

enum rDirec{rUp, rDown, rLeft, rRight};

extern const int rKeys[4][4];

inline int rKey(rDirec d)
{ return rKeys[settings::layout][d]; }

inline int rDpadKey(rDirec d)
{ return rKey(d) & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT); }

inline int rFaceKey(rDirec d)
{ return rKey(d) & ~(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT); }
