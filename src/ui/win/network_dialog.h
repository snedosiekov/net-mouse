#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

#include "core/udp.h"

namespace nm::ui {

// Modal "Network interfaces" dialog: one checkbox per interface plus "All interfaces".
// Returns the new selection (empty = all interfaces) or nullopt if cancelled.
std::optional<std::vector<std::string>> showNetworkDialog(HWND owner, const std::vector<UdpMulticast::Choice>& choices,
                                                          bool allSelected, HFONT font, UINT dpi);

}  // namespace nm::ui
