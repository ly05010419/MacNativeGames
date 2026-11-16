using System.Text.Json.Serialization;

namespace Ryujinx.Common.Logging
{
    [JsonConverter(typeof(JsonStringEnumConverter<LogLevel>))]
    public enum LogLevel
    {
        Debug,
        Stub,
        Info,
        Warning,
        Error,
        Guest,
        AccessLog,
        NetLog,
        Notice,
        Trace,
    }
}
