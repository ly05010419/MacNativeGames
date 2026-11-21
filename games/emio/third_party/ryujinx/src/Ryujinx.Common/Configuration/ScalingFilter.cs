using System.Text.Json.Serialization;

namespace Ryujinx.Common.Configuration
{
    [JsonConverter(typeof(JsonStringEnumConverter<ScalingFilter>))]
    public enum ScalingFilter
    {
        Bilinear,
        Nearest,
        Fsr,
        Area,
    }
}
