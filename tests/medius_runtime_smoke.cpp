#include "sunone_aimbot_2/mouse/MediusRuntime.h"

#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    if (MediusRuntime::supportedAbiLabel() != "6")
        return 1;
    if (MediusRuntime::controlBaudRate() != 4000000u)
        return 2;
    if (MediusRuntime::customBaudSupported())
        return 3;

    MediusRuntime& runtime = MediusRuntime::instance();
    if (!runtime.setPreferredPort("COM7") || runtime.preferredPort() != "COM7")
        return 4;
    if (!runtime.setPreferredPort("\\\\.\\COM12") || runtime.preferredPort() != "COM12")
        return 5;
    if (runtime.setPreferredPort("INVALID"))
        return 6;
    if (runtime.setControlBaudRate(115200u))
        return 7;
    if (!runtime.setControlBaudRate(4000000u))
        return 8;
    if (!runtime.setPreferredPort("AUTO") || runtime.preferredPort() != "AUTO")
        return 9;

    std::cout << "Supported ABI: " << MediusRuntime::supportedAbiLabel() << '\n';
    std::cout << "Supported versions: " << MediusRuntime::supportedVersionLabel() << '\n';
    std::cout << "Control baud: " << MediusRuntime::controlBaudRate() << '\n';
    std::cout << "Port policy: PASS\n";

    if (argc > 1 && std::string(argv[1]) == "--update-integration")
    {
        runtime.requestForceUpdateCheck();
        (void)runtime.connectMouse(); // A hosted runner normally has no Medius box; update/probe must still work.
        const auto status = runtime.status();
        if (!status.updateChecked)
            return 20;
        if (!status.latestKnown)
            return 21;
        if (status.latestSupported && !status.runtimeLoaded)
            return 22;
        if (status.latestSupported && status.runtimeAbi != status.latestAbi)
            return 23;

        std::cout << "Latest: v" << status.latestVersion << " / ABI " << status.latestAbi
                  << " / " << (status.latestSupported ? "supported" : "unsupported") << '\n';
        std::cout << "Runtime loaded: " << (status.runtimeLoaded ? "yes" : "no") << '\n';
        std::cout << "Updater integration: PASS\n";
        runtime.disconnectMouse();
    }

    return 0;
}
