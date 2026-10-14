#pragma once

class FrameChannel;

// Runs one OpenXR session showing the channel's frames on a floating screen.
// Returns when the session ends, fails, or quit becomes true.
void RunXrScreen(FrameChannel &channel, const volatile bool &quit);
