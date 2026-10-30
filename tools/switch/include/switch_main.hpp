#pragma once

#include "gfx/gfx.hpp"

/**
 * Entry point callback receiving the composed command-line arguments.
 * Argument strings remain valid for the duration of the call.
 */
using SwitchRunFunction = int (*)(int, const char **);

/**
 * Runs the game with Homebrew Menu arguments followed by whitespace-separated arguments from args.txt.
 * The argument strings stay alive until run returns.
 */
int SwitchMain(int argc, const char **argv, SwitchRunFunction run);

/**
 * Returns the fixed output dimensions for the current console operation mode.
 */
void SwitchDisplaySize(int &width, int &height);

/**
 * Logs the operation mode, performance mode and selected APM performance configuration.
 */
void SwitchLogClocks();

/**
 * Chooses a render target scale from the current console operation mode.
 * The game keeps its logical PAL resolution while using the display's available pixels.
 */
void SwitchConfigureRenderer(gfx::RendererConfig &config);

/**
 * Updates the output size and render scale after docking or undocking outside an active frame.
 */
void SwitchUpdateDisplay(float logical_height);

/**
 * Places the game and active render threads on separate available CPU cores.
 * Leaves their scheduler settings alone when the process has fewer than two cores.
 */
void SwitchConfigureThreads();

/**
 * Shows missing-data instructions on the console until + is pressed or the applet exits.
 */
void SwitchShowMissingData();
