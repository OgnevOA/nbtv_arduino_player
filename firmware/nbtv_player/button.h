// Atom Lite top button: single / double / long-press gestures.
#pragma once

enum ButtonEvent { BTN_NONE, BTN_SINGLE, BTN_DOUBLE, BTN_LONG };

void        button_init();
ButtonEvent button_poll();     // call frequently; returns at most one event
bool        button_down_now(); // raw state (for boot-hold provisioning)
