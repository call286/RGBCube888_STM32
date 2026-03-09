#pragma once

void espAtBridgeInit();
void espAtBridgeHandle();
void espAtBridgeSetEnabled(bool enabled);
bool espAtBridgeIsEnabled();
bool espAtBridgeSendLine(const char *line);
