#include "ezfadvance/usb_device.hpp"

#include <cassert>
#include <sstream>
#include <string>

int main()
{
    std::ostringstream output;
    ezfadvance::reportUsbOpenFailure(
        {ezfadvance::UsbOpenStatus::device_not_found, 0}, output, "Device");
    assert(output.str() == "Device VID=0x0E6A PID=0x5088 not found.\n");

    output.str({});
    output.clear();
    ezfadvance::reportUsbOpenFailure(
        {ezfadvance::UsbOpenStatus::success, 0}, output);
    assert(output.str().empty());
}
