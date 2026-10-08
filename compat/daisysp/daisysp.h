/* daisycola: loads daisycola's Utility/delayline.h before DaisySP's daisysp.h, so the original
 * delayline.h is skipped by its include guard. This directory has to come before DaisySP's
 * Source directory on the include path. */
#pragma once
#include <Utility/delayline.h>
#include_next <daisysp.h>
