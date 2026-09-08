#include "sunone_aimbot_2/overlay/ui_sections.h"

static_assert(MediusRuntime::controlBaudRate() == 4000000u, "Medius control baud changed unexpectedly");
static_assert(!MediusRuntime::customBaudSupported(), "Update the UI when Medius gains custom baud support");

int main()
{
    return 0;
}
