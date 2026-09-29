#pragma once

#define AP_SSID       "YOUR_AP_SSID"
#define AP_PASSWORD   "YOUR_AP_PASSWORD"

// Deliberately empty. provision() treats an empty password as "not
// provisioned" and leaves the control API UNAUTHENTICATED with a loud warning,
// which is the honest state for a build with no secrets. This file used to ship
// a fixed non-empty placeholder instead, so a build that was never given a real
// password could still come up reporting auth.required: true while checking
// against a value anyone can read in git (FW-16).
// Copy this file to config_secrets.h and set a password of your own; auth.c
// also refuses that old placeholder by name, so a stale copy of it cannot
// silently become the real one.
#define CAMERA_CONTROL_PASSWORD ""
