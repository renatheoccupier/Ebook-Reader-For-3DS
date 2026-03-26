#pragma once
#include "settings.h"

enum rDirec{rUp, rDown, rLeft, rRight};

extern const u32 rKeys[4][4];

inline u32 rKey(rDirec d)
{ return rKeys[settings::layout][d]; }

inline u32 rDpadKey(rDirec d)
{ return rKey(d) & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT); }

inline u32 rFaceKey(rDirec d)
{ return rKey(d) & ~(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT); }
