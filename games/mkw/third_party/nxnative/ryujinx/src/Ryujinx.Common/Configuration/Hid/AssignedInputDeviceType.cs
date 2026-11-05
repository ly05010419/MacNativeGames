using System.Text.Json.Serialization;

namespace Ryujinx.Common.Configuration.Hid
{
    [JsonConverter(typeof(JsonStringEnumConverter<AssignedInputDeviceType>))]
    public enum AssignedInputDeviceType
    {
        Keyboard,
        Controller,
    }
}
