/* Included by the Mac settings panel after its row helpers. */
static RuntimeDSUSettings settings_dsu_config(void) { RuntimeDSUSettings s; runtime_dsu_settings(&s); return s; }
static RuntimeDSUStatus settings_dsu_snapshot(void) { RuntimeDSUStatus s; runtime_dsu_status(&s); return s; }
static void settings_dsu_change(int field, int value, NSString *endpoint) {
 RuntimeDSUSettings s=settings_dsu_config();
 if(field==0)s.enabled=value;else if(field==1)s.slot=value;
 BOOL fits=!endpoint||[endpoint lengthOfBytesUsingEncoding:NSUTF8StringEncoding]<sizeof s.endpoint;
 if(endpoint&&fits)snprintf(s.endpoint,sizeof s.endpoint,"%s",endpoint.UTF8String);
 int result=fits?runtime_dsu_apply(&s):-1;
 if(result){NSAlert *alert=[NSAlert new];alert.messageText=settings_text("DSU 设置未保存","DSU settings were not saved");
  alert.informativeText=result==-2?settings_text("本次启动由环境变量指定 DSU。移除 TOTK_DSU 和 TOTK_DSU_SLOT 后重启即可在这里修改。","DSU is set by the launch environment. Remove TOTK_DSU and TOTK_DSU_SLOT and relaunch to edit here."):
   settings_text("请输入主机:端口（端口 1–65535）。IPv6 使用 [地址]:端口，例如 [::1]:26760。原设置保留。","Enter host:port (port 1–65535). Use [address]:port for IPv6, e.g. [::1]:26760. The previous settings are kept.");[alert runModal];}
}
static TOTKSettingRow *settings_dsu_info(NSString *title, NSString *(^text)(void)) {
 TOTKSettingRow *r=[TOTKSettingRow new];r.title=title;r.text=text;r.buttonTitle=@"";return r;
}
static void settings_dsu_rows(NSMutableArray<TOTKSettingRow *> *rows) {
 RuntimeDSUSettings s=settings_dsu_config();
 NSString *title=settings_text("DSU 输入","DSU input");
 if(s.locked)[rows addObject:settings_fixed([title stringByAppendingString:settings_text("（环境变量指定）"," (set by environment)")],settings_on_off(s.enabled))];
 else [rows addObject:settings_row(title,nil,nil,^int{return settings_dsu_config().enabled;},^(int v){settings_dsu_change(0,v,nil);})];
 TOTKSettingRow *server=settings_dsu_info(settings_text("DSU 服务器","DSU server"),^NSString *{return @(settings_dsu_config().endpoint);});
 if(!s.locked){server.buttonTitle=settings_text("编辑…","Edit…");server.buttonAction=^{
  NSAlert *alert=[NSAlert new];alert.messageText=settings_text("DSU 服务器","DSU server");
  alert.informativeText=settings_text("输入主机:端口，例如 127.0.0.1:26760。保存后立即应用。","Enter host:port, e.g. 127.0.0.1:26760. Applies immediately.");
  NSTextField *field=[[NSTextField alloc] initWithFrame:NSMakeRect(0,0,300,24)];field.stringValue=@(settings_dsu_config().endpoint);alert.accessoryView=field;
  [alert addButtonWithTitle:settings_text("保存","Save")];[alert addButtonWithTitle:settings_text("取消","Cancel")];
  if([alert runModal]==NSAlertFirstButtonReturn)settings_dsu_change(-1,0,[field.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]);
 };}[rows addObject:server];
 NSArray *slots=@[@"0",@"1",@"2",@"3"];
 [rows addObject:s.locked?settings_fixed(settings_text("DSU 手柄槽位","DSU controller slot"),slots[s.slot]):
  settings_row(settings_text("DSU 手柄槽位","DSU controller slot"),slots,@[@0,@1,@2,@3],^int{return settings_dsu_config().slot;},^(int v){settings_dsu_change(1,v,nil);})];
 TOTKSettingRow *statusRow=settings_dsu_info(settings_text("DSU 状态","DSU status"),^NSString *{
  RuntimeDSUStatus status=settings_dsu_snapshot();
  if(*status.error)return [NSString stringWithFormat:settings_text("DSU · 连接失败：%s","DSU · Connection failed: %s"),status.error];
  if(!settings_dsu_config().enabled)return settings_text("DSU · 关闭","DSU · Off");
  if(!status.connected)return settings_text("DSU · 等待连接","DSU · Waiting for connection");
  NSString *input=[NSString stringWithFormat:settings_text("DSU · 已连接 · 按键 %05x · L%u,%u R%u,%u","DSU · Connected · Keys %05x · L%u,%u R%u,%u"),status.buttons,status.axes[0],status.axes[1],status.axes[2],status.axes[3]];
  if(!status.sensor)return [input stringByAppendingString:settings_text(" · 等待体感样本"," · Waiting for motion")];
  return [input stringByAppendingFormat:settings_text(" · 加速度 %.1f,%.1f,%.1fg · 陀螺仪 %.1f,%.1f,%.1f°/s"," · Acc %.1f,%.1f,%.1fg · Gyro %.1f,%.1f,%.1f°/s"),status.acc[0],status.acc[1],status.acc[2],status.gyro[0],status.gyro[1],status.gyro[2]];
 });
 statusRow.fullWidthText=YES;[rows addObject:statusRow];
}
