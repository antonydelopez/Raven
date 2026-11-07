/* Kit gallery: tabs of widgets, text, a timeline, glass and motion, with live reload of gallery.xml. */
#include "../common/example.h"

#ifndef RAVEN_EXAMPLE_DIR
#define RAVEN_EXAMPLE_DIR "."
#endif

static Rid window, reload_label;

static void on_action(Rid sender, const char* action, void* user_data) {
	(void)user_data;
	if (strcmp(action, "theme.toggle") == 0)
		Rtheme_apply(Rcheckbox_is_checked(sender) ? R_THEME_GLASS_DARK : R_THEME_GLASS_LIGHT);
}

static void on_reload(const char* path, int ok, void* user_data) {
	(void)user_data;
	fprintf(stderr, "[gallery] reload %s: %s\n", path, ok ? "ok" : "FAILED");
}

int main(void) {
	if (!example_init("gallery")) return 1;
	Rtheme_apply(R_THEME_GLASS_DARK);
	Rxml_set_reload_callback(on_reload, NULL);
	window = Rwindow_create_from_xml_file(RAVEN_EXAMPLE_DIR "/gallery.xml");
	if (window == R_INVALID_ID) { Rshutdown(); return 1; }
	reload_label = Rwidget_find(window, "reload");
	{ const char* t = getenv("RAVEN_GALLERY_TAB"); Rid tv = Rwidget_find(window, "tabs"); if (t && tv != R_INVALID_ID) Rtab_view_set_active(tv, atoi(t)); }
	(void)reload_label;
	Raction_set_default_handler(on_action, NULL);
	if (!example_automated(window)) while (Rwait_frame(-1.0)) {}
	Rshutdown();
	return 0;
}
