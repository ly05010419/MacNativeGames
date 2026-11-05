enum {
    SVC_HOST_GLYPH_LOOKUP_0 = 0x82,
    SVC_HOST_GLYPH_LOOKUP_1 = 0x83,
    SVC_HOST_GLYPH_LOOKUP_2 = 0x84,
    SVC_HOST_GLYPH_LOOKUP_3 = 0x85,
    SVC_HOST_GLYPH_LOOKUP_4 = 0x86,
    SVC_HOST_GLYPH_LOOKUP_5 = 0x87,
    SVC_HOST_GLYPH_LOOKUP_6 = 0x88,
    SVC_HOST_GLYPH_LOOKUP_7 = 0x89,
    SVC_HOST_GLYPH_LOOKUP_8 = 0x8a,
    SVC_HOST_GLYPH_LOOKUP_9 = 0x8b,
    SVC_HOST_GLYPH_LOOKUP_10 = 0x8c,
    SVC_HOST_GLYPH_LOOKUP_11 = 0x8d,
    SVC_HOST_GLYPH_LOOKUP_12 = 0x8e,
    SVC_HOST_GLYPH_LOOKUP_13 = 0x8f,
    SVC_HOST_GLYPH_LOOKUP_14 = 0x90,
    SVC_HOST_GLYPH_LOOKUP_15 = 0x91,
    SVC_HOST_GLYPH_LOOKUP_16 = 0x92,
};
// Glyph lookup call sites (call-site form: original is bl 0x123270; the host forwards then substitutes on NULL)
// 0x82 SVC_HOST_GLYPH_LOOKUP_0 0x11f88c（bl 0x123270 调用点：宿主以原参数代调原 lookup 函数，返回 NULL 时交可写零块；调用点形。仅宿主。）
// 0x83 SVC_HOST_GLYPH_LOOKUP_1 0x11f8d4（bl 0x123270 调用点：宿主以原参数代调原 lookup 函数，返回 NULL 时交可写零块；调用点形。仅宿主。）
// 0x84 SVC_HOST_GLYPH_LOOKUP_2 0x11f8ec（bl 0x123270 调用点：宿主以原参数代调原 lookup 函数，返回 NULL 时交可写零块；调用点形。仅宿主。）
// 0x85 SVC_HOST_GLYPH_LOOKUP_3 0x11fa18（bl 0x123270 调用点：宿主以原参数代调原 lookup 函数，返回 NULL 时交可写零块；调用点形。仅宿主。）
// 0x86 SVC_HOST_GLYPH_LOOKUP_4 0x11fa7c（bl 0x123270 调用点：宿主以原参数代调原 lookup 函数，返回 NULL 时交可写零块；调用点形。仅宿主。）
// 0x87 SVC_HOST_GLYPH_LOOKUP_5 0x11fac8（bl 0x123270 调用点：宿主以原参数代调原 lookup 函数，返回 NULL 时交可写零块；调用点形。仅宿主。）
// 0x88 SVC_HOST_GLYPH_LOOKUP_6 0x11fb38（bl 0x123270 调用点：宿主以原参数代调原 lookup 函数，返回 NULL 时交可写零块；调用点形。仅宿主。）
// 0x89 SVC_HOST_GLYPH_LOOKUP_7 0x11fba8（bl 0x123270 调用点：宿主以原参数代调原 lookup 函数，返回 NULL 时交可写零块；调用点形。仅宿主。）
// 0x8a SVC_HOST_GLYPH_LOOKUP_8 0x11fc14（bl 0x123270 调用点：宿主以原参数代调原 lookup 函数，返回 NULL 时交可写零块；调用点形。仅宿主。）
// 0x8b SVC_HOST_GLYPH_LOOKUP_9 0x11fc40（bl 0x123270 调用点：宿主以原参数代调原 lookup 函数，返回 NULL 时交可写零块；调用点形。仅宿主。）
// 0x8c SVC_HOST_GLYPH_LOOKUP_10 0x11fc64（bl 0x123270 调用点：宿主以原参数代调原 lookup 函数，返回 NULL 时交可写零块；调用点形。仅宿主。）
// 0x8d SVC_HOST_GLYPH_LOOKUP_11 0x11fc90（bl 0x123270 调用点：宿主以原参数代调原 lookup 函数，返回 NULL 时交可写零块；调用点形。仅宿主。）
// 0x8e SVC_HOST_GLYPH_LOOKUP_12 0x11fda0（bl 0x123270 调用点：宿主以原参数代调原 lookup 函数，返回 NULL 时交可写零块；调用点形。仅宿主。）
// 0x8f SVC_HOST_GLYPH_LOOKUP_13 0x11ff20（bl 0x123270 调用点：宿主以原参数代调原 lookup 函数，返回 NULL 时交可写零块；调用点形。仅宿主。）
// 0x90 SVC_HOST_GLYPH_LOOKUP_14 0x11ff4c（bl 0x123270 调用点：宿主以原参数代调原 lookup 函数，返回 NULL 时交可写零块；调用点形。仅宿主。）
// 0x91 SVC_HOST_GLYPH_LOOKUP_15 0x11ff70（bl 0x123270 调用点：宿主以原参数代调原 lookup 函数，返回 NULL 时交可写零块；调用点形。仅宿主。）
// 0x92 SVC_HOST_GLYPH_LOOKUP_16 0x11ff9c（bl 0x123270 调用点：宿主以原参数代调原 lookup 函数，返回 NULL 时交可写零块；调用点形。仅宿主。）
// Host-call numbers introduced by registered cuts (structck checks cut svc numbers against this table).
// Every row: // <main offset> SVC_HOST_<NAME> <original target>（<form and contract description>）

