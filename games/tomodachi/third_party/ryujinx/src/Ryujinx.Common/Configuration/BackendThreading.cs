using System.Text.Json.Serialization;

namespace Ryujinx.Common.Configuration
{
    [JsonConverter(typeof(JsonStringEnumConverter<BackendThreading>))]
    public enum BackendThreading
    {
        Auto,
        Off,
        On,
    }
}
