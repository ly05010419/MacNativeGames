/* Unrelated gamepad probes don't link the six-axis fusion. DSU is off in them. */
void runtime_motion_feed(const double a[3], const double g[3], uint64_t t) { (void)a; (void)g; (void)t; }
void runtime_motion_live(int on) { (void)on; }
void runtime_motion_deadzone_default(double value) { (void)value; }
int runtime_motion_axes(const char *axes) { (void)axes; return 0; }
void runtime_feedback_motion_source_changed(void) {}
