#pragma once
#include <string>
#include <vector>
// Maps the owner's PE data, binds imports, tests lifted code and attempts CRT
// entry until the first unsupported host service. Returns an honest status.
std::string connectGuestRuntime(const char* executable);
void requestGuestRuntimeStop();
void quiesceGuestFilesForExit(int timeoutMs);  // flush saves before quitting (see guest_runtime.cpp)
void setGuestPaused(bool paused);
void setGuestDisplaySize(unsigned width,unsigned height);
void setGuestKey(unsigned scan,bool down);
void clearGuestInput();
bool guestDrivingControls();
// Physical gamepad seen by the game as a DirectInput joystick: axes X, Y, Z
// (triggers), Rx, Ry, Rz in -1..1; buttons A B X Y LB RB Back Start LS RS as bits.
void setGuestPad(const float axes[6],uint32_t buttons,int hatX,int hatY);
void setGuestPadConnected(bool connected);
// True while the game has the joystick acquired (then the pad stops emulating keys).
bool guestGamepadInUse();
// Launcher option; only before the guest boots.
void setGuestWidescreen(bool enabled);
void setGuestOnlineServer(const std::string& host);
/** Touch as mouse: x/y as a fraction of the game view; phase 0 down, 1 move, 2 up (left button). */
void setGuestPointer(float x,float y,int phase);
/** Posts WM_CHAR messages (the game's text fields read those); 8 is backspace. */
void typeGuestText(const std::vector<uint32_t>& characters);
void setGuestLanguage(const char* language);
/** R36S virtual mouse: while on, the game sees a pointer at (x,y) (fractions of the game view); off parks it off-screen. */
void setGuestMouseActive(bool on,float x,float y);
/** Pointer move (event 1), left button down (0) or up (2) with the pointer at (x,y); leftDown is the button state after it. */
void setGuestMouse(float x,float y,int event,bool leftDown);
void setGuestResolution(unsigned width,unsigned height);
void setGuestFrameLimit(unsigned framesPerSecond);

/** Number of FMV (.vp6) files the game has open right now (0 when no movie plays). */
int guestMoviesOpen();
