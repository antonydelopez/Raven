/* Review finding (kit-overlay-raven-timeline-option-review-1): with MarkersLocked, a marker
 * selected together with a clip must stay put when the clip is dragged. */
#include "test.h"

#include <string.h>

static Rid make_window(const char* xml) {
	Rid window = Rwindow_create_from_xml(xml, strlen(xml));
	CHECK(Rwindow_is_valid(window));
	Rdraw_frame();
	return window;
}

/* Press, move in two steps (a drag starts after a few units), release: as tests/test_tools.c. */
static void drag(Rid window, float x0, float y0, float x1, float y1) {
	Rwindow_inject_mouse_move(window, x0, y0);
	Rwindow_inject_mouse_button(window, R_MOUSE_LEFT, R_TRUE);
	Rwindow_inject_mouse_move(window, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
	Rwindow_inject_mouse_move(window, x1, y1);
	Rdraw_frame();
	Rwindow_inject_mouse_button(window, R_MOUSE_LEFT, R_FALSE);
	Rdraw_frame();
}

static void test_timeline_markers_locked_in_selection(void) {
	Rid window, tl;
	int clip = 0, marker = 0, i;
	float x, y, w, h, second = 80.0f;
	CHECK(test_init(0));
	window = make_window("<Window Width=\"900\" Height=\"300\">"
		"<Timeline Name=\"t\" Duration=\"10\" HeaderWidth=\"100\" Snapping=\"false\" MarkersLocked=\"true\">"
		"<Track Name=\"A\"><Clip Start=\"1\" Length=\"2\" Label=\"c\"/></Track>"
		"<Marker Time=\"8\" Label=\"m\"/></Timeline></Window>");
	tl = Rwidget_find(window, "t");
	for (i = 0; i < Rtimeline_get_item_count(tl); i++) {
		int item = Rtimeline_get_item(tl, i);
		if (Rtimeline_get_item_type(tl, item) == R_TIMELINE_CLIP) clip = item;
		if (Rtimeline_get_item_type(tl, item) == R_TIMELINE_MARKER) marker = item;
	}
	CHECK(clip && marker && Rtimeline_get_markers_locked(tl));
	Rwidget_get_rect(tl, &x, &y, &w, &h);
	/* (as Ctrl+A or Ctrl+click leave it; from the API so no modifier stays held for the press) */
	Rtimeline_select(tl, clip, R_FALSE);
	Rtimeline_select(tl, marker, R_TRUE);
	CHECK(Rtimeline_is_selected(tl, clip) && Rtimeline_is_selected(tl, marker));
	/* the clip (1..3 s, first track: 30 units of ruler, 38 a row) is dragged one second later */
	drag(window, x + 100.0f + second * 2.0f, y + 30.0f + 19.0f, x + 100.0f + second * 3.0f, y + 30.0f + 19.0f);
	CHECK(Rtimeline_get_item_start(tl, clip) > 1.5);
	/* the locked marker stays at 8 s */
	CHECK(Rtimeline_get_item_start(tl, marker) == 8.0);
	Rshutdown();
}

int main(void) {
	RUN(test_timeline_markers_locked_in_selection);
	return test_report("timeline_marker_lock");
}
