/*
 * Apps on the G981B use Metal calls Jesvi's driver (RedwoodMetal) does not have, and die on the first one
 * (Telegram: -[RedwoodMetalRenderEncoder setDepthCleared], "unrecognized selector", 2026-11-22). Inserted into apps,
 * this gives RedwoodMetal's classes a +resolveInstanceMethod: that answers any unknown "set..." message (a void
 * setter: a state to apply, which RedwoodMetal does not have) with a method that does nothing, and logs it once
 * ("x1s-mtl: ..." on /dev/console). Anything else is left to fail as before.
 */
typedef unsigned long size_t;
typedef void *id, *SEL, *Class, *Method, *IMP;
typedef signed char BOOL;
extern int open(const char *, int, ...);
extern long write(int, const void *, size_t);
extern int close(int);
extern int snprintf(char *, size_t, const char *, ...);
extern int strncmp(const char *, const char *, size_t);
extern void _dyld_register_func_for_add_image(void (*)(const void *, long));
extern Class objc_getClass(const char *);
extern Class object_getClass(void *);
extern const char *class_getName(Class);
extern const char *sel_getName(SEL);
extern SEL sel_registerName(const char *);
extern BOOL class_addMethod(Class, SEL, IMP, const char *);
extern Method class_getClassMethod(Class, SEL);
extern IMP method_getImplementation(Method);

static const char *const names[] = {
	"RedwoodMetalArgumentEncoder", "RedwoodMetalBlitEncoder", "RedwoodMetalBuffer", "RedwoodMetalCommandBuffer",
	"RedwoodMetalCommandQueue", "RedwoodMetalComputeEncoder", "RedwoodMetalComputePipeline",
	"RedwoodMetalDepthStencilState", "RedwoodMetalDevice", "RedwoodMetalRenderEncoder", "RedwoodMetalRenderPipeline",
	"RedwoodMetalSampler", "RedwoodMetalTexture",
};
#define N (sizeof(names) / sizeof(names[0]))
static int done[N];

static void
say(const char *cls, const char *sel)
{
	char line[300];
	int fd = open("/dev/console", 0x20001), n = snprintf(line, sizeof(line), "x1s-mtl: -[%s %s] does nothing here%c", cls, sel, 10);
	if (fd >= 0) { write(fd, line, (size_t)n); close(fd); }
}

static void
nothing(id self, SEL cmd)
{
	(void)self; (void)cmd;
}

static BOOL
resolve(Class self, SEL cmd, SEL sel)
{
	const char *name = sel_getName(sel);

	(void)cmd;
	if (name && strncmp(name, "set", 3) == 0 && class_addMethod(self, sel, (IMP)nothing, "v@:")) {
		say(class_getName(self), name);
		return 1;
	}
	return 0;
}

/*
 * The asynchronous pipeline calls (TikTok, 2026-11-22: its completion block read the error's text when RedwoodMetal
 * handed back no pipeline and no error, strlen(NULL), a fault loop and no video). Done here through the synchronous
 * ...reflection:error: call; a failure without an error gets one, and the reason is logged.
 */
extern id objc_msgSend(id, SEL, ...);
extern IMP method_setImplementation(Method, IMP);
extern Method class_getInstanceMethod(Class, SEL);
struct block { void *isa; int flags, reserved; void *invoke; };

static id
make_error(const char *why)
{
	Class s = objc_getClass("NSString"), d = objc_getClass("NSDictionary"), e = objc_getClass("NSError");
	id text = ((id (*)(Class, SEL, const char *))objc_msgSend)(s, sel_registerName("stringWithUTF8String:"), why);
	id key = ((id (*)(Class, SEL, const char *))objc_msgSend)(s, sel_registerName("stringWithUTF8String:"), "NSLocalizedDescription");
	id info = ((id (*)(Class, SEL, id, id))objc_msgSend)(d, sel_registerName("dictionaryWithObject:forKey:"), text, key);
	id domain = ((id (*)(Class, SEL, const char *))objc_msgSend)(s, sel_registerName("stringWithUTF8String:"), "RedwoodMetal");
	return ((id (*)(Class, SEL, id, long, id))objc_msgSend)(e, sel_registerName("errorWithDomain:code:userInfo:"), domain, 1, info);
}

static void
say_failure(const char *what, id error)
{
	char line[700];
	id d = error ? ((id (*)(id, SEL))objc_msgSend)(error, sel_registerName("localizedDescription")) : 0;
	const char *c = d ? ((const char *(*)(id, SEL))objc_msgSend)(d, sel_registerName("UTF8String")) : 0;
	int fd = open("/dev/console", 0x20001), n = snprintf(line, sizeof(line), "x1s-mtl: %s failed: %s%c", what, c ? c : "(no error given)", 10);
	if (fd >= 0) { write(fd, line, (size_t)n); close(fd); }
}

/* render: ...options:completionHandler: -> (pipeline, reflection, error); ...completionHandler: -> (pipeline, error) */
static void
render_async_options(id self, SEL cmd, id desc, unsigned long options, struct block *done_block)
{
	id refl = 0, error = 0;
	id pso = ((id (*)(id, SEL, id, unsigned long, id *, id *))objc_msgSend)(self,
	    sel_registerName("newRenderPipelineStateWithDescriptor:options:reflection:error:"), desc, options, options ? &refl : 0, &error);
	(void)cmd;
	if (!pso) { if (!error) error = make_error("render pipeline not supported here"); say_failure("render pipeline", error); }
	{
		static int told;
		if (told++ < 3) {
			char line[300];
			id l = pso ? ((id (*)(id, SEL))objc_msgSend)(pso, sel_registerName("label")) : 0;
			const char *lc = l ? ((const char *(*)(id, SEL))objc_msgSend)(l, sel_registerName("UTF8String")) : 0;
			int fd = open("/dev/console", 0x20001), n = snprintf(line, sizeof(line),
			    "x1s-mtl: render pipeline (options %lu): %s, label %s, reflection %p, error %p%c", options,
			    pso ? class_getName(object_getClass(pso)) : "nil", lc ? lc : "nil", refl, error, 10);
			if (fd >= 0) { write(fd, line, (size_t)n); close(fd); }
		}
	}
	if (done_block) ((void (*)(void *, id, id, id))done_block->invoke)(done_block, pso, refl, error);
}

static void
render_async(id self, SEL cmd, id desc, struct block *done_block)
{
	id error = 0;
	id pso = ((id (*)(id, SEL, id, id *))objc_msgSend)(self, sel_registerName("newRenderPipelineStateWithDescriptor:error:"), desc, &error);
	(void)cmd;
	if (!pso) { if (!error) error = make_error("render pipeline not supported here"); say_failure("render pipeline", error); }
	if (done_block) ((void (*)(void *, id, id))done_block->invoke)(done_block, pso, error);
}

static void
compute_async_options(id self, SEL cmd, id desc, unsigned long options, struct block *done_block)
{
	id refl = 0, error = 0;
	id pso = ((id (*)(id, SEL, id, unsigned long, id *, id *))objc_msgSend)(self,
	    sel_registerName("newComputePipelineStateWithDescriptor:options:reflection:error:"), desc, options, options ? &refl : 0, &error);
	(void)cmd;
	if (!pso) { if (!error) error = make_error("compute pipeline not supported here"); say_failure("compute pipeline", error); }
	if (done_block) ((void (*)(void *, id, id, id))done_block->invoke)(done_block, pso, refl, error);
}

static void
compute_async_function(id self, SEL cmd, id function, struct block *done_block)
{
	id error = 0;
	id pso = ((id (*)(id, SEL, id, id *))objc_msgSend)(self, sel_registerName("newComputePipelineStateWithFunction:error:"), function, &error);
	(void)cmd;
	if (!pso) { if (!error) error = make_error("compute pipeline not supported here"); say_failure("compute pipeline", error); }
	if (done_block) ((void (*)(void *, id, id))done_block->invoke)(done_block, pso, error);
}

/* -label: RedwoodMetal's objects answer nil where Metal's have a string at least (TikTok: [pipeline label].UTF8String
 * into a C++ string -> strlen(NULL)). Its own answer when it has one, else the class's name. */
static IMP old_label[N];
static id label_text[N];

static id
label_for(id self, unsigned i)
{
	id l = old_label[i] ? ((id (*)(id, SEL))old_label[i])(self, sel_registerName("label")) : 0;
	if (l) return l;
	if (!label_text[i]) {
		label_text[i] = ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSString"),
		    sel_registerName("stringWithUTF8String:"), names[i]);
		((id (*)(id, SEL))objc_msgSend)(label_text[i], sel_registerName("retain"));
	}
	return label_text[i];
}
#define LABEL_FN(i) static id label_##i(id self, SEL cmd) { (void)cmd; return label_for(self, i); }
LABEL_FN(0) LABEL_FN(1) LABEL_FN(2) LABEL_FN(3) LABEL_FN(4) LABEL_FN(5) LABEL_FN(6) LABEL_FN(7) LABEL_FN(8)
LABEL_FN(9) LABEL_FN(10) LABEL_FN(11) LABEL_FN(12)
static const IMP label_fns[N] = { (IMP)label_0, (IMP)label_1, (IMP)label_2, (IMP)label_3, (IMP)label_4, (IMP)label_5,
	(IMP)label_6, (IMP)label_7, (IMP)label_8, (IMP)label_9, (IMP)label_10, (IMP)label_11, (IMP)label_12 };

static void
fix_label(Class c, unsigned i)
{
	SEL s = sel_registerName("label");
	Method m = class_getInstanceMethod(c, s);

	if (m) old_label[i] = method_setImplementation(m, label_fns[i]);
	else class_addMethod(c, s, label_fns[i], "@@:");
}

static void
replace_on(Class c, const char *name, IMP mine)
{
	Method m = class_getInstanceMethod(c, sel_registerName(name));
	if (m) method_setImplementation(m, mine);
}

static void
added(const void *mh, long slide)
{
	(void)mh; (void)slide;
	for (unsigned i = 0; i < N; i++) {
		Class c;
		if (done[i] || (c = objc_getClass(names[i])) == 0) continue;
		done[i] = 1;
		/* only if the class has none of its own (it would be NSObject's, which answers no) */
		class_addMethod(object_getClass(c), sel_registerName("resolveInstanceMethod:"), (IMP)resolve, "c@::");
		fix_label(c, i);
		if (strncmp(names[i], "RedwoodMetalDevice", 19) == 0) {
			replace_on(c, "newRenderPipelineStateWithDescriptor:options:completionHandler:", (IMP)render_async_options);
			replace_on(c, "newRenderPipelineStateWithDescriptor:completionHandler:", (IMP)render_async);
			replace_on(c, "newComputePipelineStateWithDescriptor:options:completionHandler:", (IMP)compute_async_options);
			replace_on(c, "newComputePipelineStateWithFunction:completionHandler:", (IMP)compute_async_function);
		}
	}
}

__attribute__((constructor)) static void
start(void)
{
	_dyld_register_func_for_add_image(added);
}
