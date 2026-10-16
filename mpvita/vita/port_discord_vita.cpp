// PS Vita: no Discord client to talk to.
#include "port_discord.h"
namespace PortDiscord {
bool Supported() { return false; }
EStatus Status() { return kStatus_Off; }
std::string LastError() { return {}; }
void Configure(bool, const std::string&) {}
bool Enabled() { return false; }
void SetMenu() {}
void SetGame(const GameInfo&) {}
std::string CurrentText() { return {}; }
} // namespace PortDiscord
