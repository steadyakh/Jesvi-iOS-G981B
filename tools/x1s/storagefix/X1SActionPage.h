/*
 * Settings > Action Button, drawn as an iPhone 15 Pro's page (included into X1SPanes.c, Preferences only): a dark
 * page, a glowing pill with the action's symbol in the middle, page dots under it, swipe left and right between
 * the actions, and the action's name and what it does below. The choice goes to com.x1s.actionbutton "action",
 * which X1SAction (SpringBoard) reads when the side button is held.
 */
typedef struct { double x, y, w, h; } x1s_rect;
struct x1s_super { id receiver; Class super_class; };
extern id objc_msgSendSuper(struct x1s_super *, SEL, ...);
extern Class objc_allocateClassPair(Class, const char *, size_t);
extern void objc_registerClassPair(Class);
extern signed char class_addMethod(Class, SEL, IMP, const char *);

#define NACT 8
static const char *const act_value[NACT] = { "silent", "camera", "flashlight", "voicememos", "translate", "magnifier",
	"shortcut", "none" };
static const char *const act_title[NACT] = { "Silent Mode", "Camera", "Flashlight", "Voice Memo", "Translate",
	"Magnifier", "Shortcut", "No Action" };
static const char *const act_text[NACT] = {
	"Switch between Silent and Ring for calls and alerts.",
	"Open the Camera to quickly capture a moment.",
	"Turn on extra light when you need it.",
	"Open Voice Memos to record a note.",
	"Open Translate to translate phrases.",
	"Use your iPhone as a magnifying glass.",
	"Run the shortcut named \"Action Button\".",
	"The Action button won't do anything when pressed.",
};
static const char *const act_symbol[NACT] = { "bell.slash.fill", "camera.fill", "flashlight.on.fill", "waveform",
	"character.bubble.fill", "plus.magnifyingglass", "square.2.layers.3d.fill", "circle.slash" };
static const double act_rgb[NACT][3] = { { 1.0, 0.45, 0.0 }, { 0.45, 0.45, 0.5 }, { 0.95, 0.85, 0.55 }, { 0.95, 0.25, 0.25 },
	{ 0.2, 0.55, 1.0 }, { 0.5, 0.5, 0.55 }, { 0.55, 0.35, 0.95 }, { 0.3, 0.3, 0.32 } };

static int act_index;
static id act_pill, act_icon, act_dots, act_title_label, act_text_label;

#define DBL4(o, s, a, b, c, d) (((id (*)(id, SEL, double, double, double, double))objc_msgSend)((id)(o), S(s), a, b, c, d))
#define SETL(o, s, v) (((void (*)(id, SEL, long))objc_msgSend)((id)(o), S(s), (long)(v)))
#define SETD(o, s, v) (((void (*)(id, SEL, double))objc_msgSend)((id)(o), S(s), (double)(v)))
#define SETR(o, s, r) (((void (*)(id, SEL, x1s_rect))objc_msgSend)((id)(o), S(s), r))

static id
act_defaults(void)
{
	return M1(M0(objc_getClass("NSUserDefaults"), "alloc"), "initWithSuiteName:", STR("com.x1s.actionbutton"));
}

static id
act_colour(int i, double alpha)
{
	return DBL4(objc_getClass("UIColor"), "colorWithRed:green:blue:alpha:", act_rgb[i][0], act_rgb[i][1], act_rgb[i][2], alpha);
}

static void
act_show(void)
{
	id cfg = ((id (*)(id, SEL, double, long))objc_msgSend)((id)objc_getClass("UIImageSymbolConfiguration"),
	    S("configurationWithPointSize:weight:"), 40.0, 6 /* semibold */);
	id img = M2(objc_getClass("UIImage"), "systemImageNamed:withConfiguration:", STR(act_symbol[act_index]), cfg);
	M1(act_icon, "setImage:", img);
	M1(act_pill, "setBackgroundColor:", act_colour(act_index, 1.0));
	id layer = M0(act_pill, "layer");
	M1(layer, "setShadowColor:", M0(act_colour(act_index, 1.0), "CGColor"));
	SETL(act_dots, "setCurrentPage:", act_index);
	M1(act_title_label, "setText:", STR(act_title[act_index]));
	M1(act_text_label, "setText:", STR(act_text[act_index]));
}

static void
act_choose(int i)
{
	if (i < 0 || i >= NACT) return;
	act_index = i;
	id d = act_defaults();
	M2(d, "setObject:forKey:", STR(act_value[i]), STR("action"));
	M0(d, "synchronize");
	act_show();
	/* a light tap, as iOS gives between the actions */
	id g = M0(M0(objc_getClass("UISelectionFeedbackGenerator"), "alloc"), "init");
	if (g) M0(g, "selectionChanged");
}

static void act_left(id self, SEL cmd, id g) { (void)self; (void)cmd; (void)g; act_choose(act_index + 1); }
static void act_right(id self, SEL cmd, id g) { (void)self; (void)cmd; (void)g; act_choose(act_index - 1); }

static id
act_label(double size, double weight, double white)
{
	id l = M0(M0(objc_getClass("UILabel"), "alloc"), "init");
	id font = ((id (*)(id, SEL, double, double))objc_msgSend)((id)objc_getClass("UIFont"), S("systemFontOfSize:weight:"), size, weight);
	M1(l, "setFont:", font);
	M1(l, "setTextColor:", ((id (*)(id, SEL, double, double))objc_msgSend)((id)objc_getClass("UIColor"), S("colorWithWhite:alpha:"), white, 1.0));
	SETL(l, "setTextAlignment:", 1);
	SETL(l, "setNumberOfLines:", 0);
	return l;
}

static void
act_view_did_load(id self, SEL cmd)
{
	struct x1s_super sup = { self, objc_getClass("PSViewController") };
	((void (*)(struct x1s_super *, SEL))objc_msgSendSuper)(&sup, cmd);

	id view = M0(self, "view");
	x1s_rect b = ((x1s_rect (*)(id, SEL))objc_msgSend)(M0(objc_getClass("UIScreen"), "mainScreen"), S("bounds"));
	double W = b.w, H = b.h;
	M1(view, "setBackgroundColor:", ((id (*)(id, SEL, double, double))objc_msgSend)((id)objc_getClass("UIColor"), S("colorWithWhite:alpha:"), 0.04, 1.0));
	M1(self, "setTitle:", STR("Action Button"));

	/* where the button is: the pill */
	act_pill = M0(M0(objc_getClass("UIView"), "alloc"), "init");
	x1s_rect pr = { W / 2 - 52, H * 0.22, 104, 200 };
	SETR(act_pill, "setFrame:", pr);
	id layer = M0(act_pill, "layer");
	SETD(layer, "setCornerRadius:", 52);
	SETD(layer, "setShadowRadius:", 40);
	(((void (*)(id, SEL, float))objc_msgSend)(layer, S("setShadowOpacity:"), 0.9f));
	M1(view, "addSubview:", act_pill);

	act_icon = M0(M0(objc_getClass("UIImageView"), "alloc"), "init");
	x1s_rect ir = { 0, 0, 104, 200 };
	SETR(act_icon, "setFrame:", ir);
	SETL(act_icon, "setContentMode:", 4 /* center */);
	M1(act_icon, "setTintColor:", ((id (*)(id, SEL, double, double))objc_msgSend)((id)objc_getClass("UIColor"), S("colorWithWhite:alpha:"), 1.0, 1.0));
	M1(act_pill, "addSubview:", act_icon);

	act_dots = M0(M0(objc_getClass("UIPageControl"), "alloc"), "init");
	x1s_rect dr = { 0, pr.y + pr.h + 30, W, 26 };
	SETR(act_dots, "setFrame:", dr);
	SETL(act_dots, "setNumberOfPages:", NACT);
	SETL(act_dots, "setUserInteractionEnabled:", 0);
	M1(view, "addSubview:", act_dots);

	act_title_label = act_label(28, 0.4 /* bold */, 1.0);
	x1s_rect tr = { 20, dr.y + 40, W - 40, 36 };
	SETR(act_title_label, "setFrame:", tr);
	M1(view, "addSubview:", act_title_label);

	act_text_label = act_label(15, 0.0, 0.65);
	x1s_rect xr = { 40, tr.y + 40, W - 80, 60 };
	SETR(act_text_label, "setFrame:", xr);
	M1(view, "addSubview:", act_text_label);

	/* swipe between the actions, as on the iPhone */
	for (int d = 0; d < 2; d++) {
		id g = M2(M0(objc_getClass("UISwipeGestureRecognizer"), "alloc"), "initWithTarget:action:", self,
		    d ? S("x1sRight:") : S("x1sLeft:"));
		SETL(g, "setDirection:", d ? 1 /* right */ : 2 /* left */);
		M1(view, "addGestureRecognizer:", g);
	}

	/* what is chosen now (Silent Mode when nothing is) */
	id cur = M1(act_defaults(), "objectForKey:", STR("action"));
	act_index = 0;
	for (int i = 0; cur && i < NACT; i++)
		if (EQ(cur, STR(act_value[i]))) act_index = i;
	act_show();
}

static Class
act_page_class(void)
{
	static Class c;
	if (c) return c;
	if ((c = objc_getClass("X1SActionButtonController"))) return c;
	c = objc_allocateClassPair(objc_getClass("PSViewController"), "X1SActionButtonController", 0);
	class_addMethod(c, S("viewDidLoad"), (IMP)act_view_did_load, "v@:");
	class_addMethod(c, S("x1sLeft:"), (IMP)act_left, "v@:@");
	class_addMethod(c, S("x1sRight:"), (IMP)act_right, "v@:@");
	objc_registerClassPair(c);
	return c;
}
