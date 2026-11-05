using System.Collections.Generic;

namespace Ryujinx.Common.Configuration.Hid
{
    public class PlayerInputAssignment
    {
        public PlayerIndex PlayerIndex { get; set; }

        public bool EnableDynamicInputSwap { get; set; }

        public List<AssignedInputDevice> Devices { get; set; } = [];
    }
}
