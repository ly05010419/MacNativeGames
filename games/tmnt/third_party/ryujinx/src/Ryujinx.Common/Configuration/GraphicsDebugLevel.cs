using System.Text.Json.Serialization;

namespace Ryujinx.Common.Configuration
{
    [JsonConverter(typeof(JsonStringEnumConverter<GraphicsDebugLevel>))]
    public enum GraphicsDebugLevel
    {
        None,
        Error,
        Slowdowns,
        All,
    }
}
