#pragma once

namespace modelcustom::game_frame
{
using Callback = bool (*)();



bool connect(Callback callback);
void disconnect();
bool connected();
}
