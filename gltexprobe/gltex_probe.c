// gltex_probe -- sample a YUV buffer as a GL external texture, which is the
// path an app takes when it plays video into a TextureView/SurfaceTexture.
//
// WHY THIS EXISTS. DpBlitStream::setPQParameter is stubbed in our
// libdpframework: m_pqSupport is hard-zero, so it returns success and applies
// nothing. The blob instead reads DpDriver::getPQSupport(). The question is
// whether anything on this device reaches that seam, and four real consumers
// answered "no" for four different reasons -- Gallery, an emulator splash and
// Moonlight all render video through a SurfaceView, where the decoder buffer
// becomes its own SurfaceFlinger layer and HWC composites it directly, and
// Brave decodes in software. None of them ever makes the frame a GL texture.
//
// The seam sits behind the MTK GLES driver: libGLESv2_mtk imports
// GpuAuxDoConversionIfNeed, which tail-jumps into GuiExtAuxDoConversionIfNeed,
// and the setPQParameter call is inside that. Driving that API by hand would
// mean recovering the whole aux context and buffer-queue ABI first. Binding an
// external-OES texture is the same path from the other end: the driver decides
// for itself whether the buffer needs conversion, and calls it if so.
//
// ⚠️ A clean run does NOT mean the stub is unreachable. It means the driver did
// not need a conversion for the format this probe asked for. Read the printed
// format back before drawing any conclusion.
//
// Run it under su. Watch for the stub's own line in parallel:
//   logcat -s DpFramework:*   ->  "setPQParameter stubbed"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The extension entry points are prototyped only when these are defined first:
// eglGetNativeClientBufferANDROID, eglCreateImageKHR and
// glEGLImageTargetTexture2DOES all live behind them.
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES

#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#endif

static const char* kVert =
	"attribute vec4 aPos;\n"
	"attribute vec2 aUV;\n"
	"varying vec2 vUV;\n"
	"void main() { vUV = aUV; gl_Position = aPos; }\n";

// samplerExternalOES is the whole point: it is the sampler a SurfaceTexture
// binds, and the one that makes the driver convert a buffer it cannot read.
static const char* kFrag =
	"#extension GL_OES_EGL_image_external : require\n"
	"precision mediump float;\n"
	"uniform samplerExternalOES uTex;\n"
	"varying vec2 vUV;\n"
	"void main() { gl_FragColor = texture2D(uTex, vUV); }\n";

static GLuint compile(GLenum type, const char* src) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "shader compile failed: %s\n", log);
		return 0;
	}
	return s;
}

// One case: allocate a buffer in `fmt`, fill it, sample it, read back.
static int run_case(const char* name, uint32_t fmt, int w, int h) {
	printf("\n-- %s (format 0x%x, %dx%d) --\n", name, fmt, w, h);

	AHardwareBuffer_Desc d;
	memset(&d, 0, sizeof(d));
	d.width = w;
	d.height = h;
	d.layers = 1;
	d.format = fmt;
	// GPU_SAMPLED_IMAGE is what makes this a texture source; CPU_WRITE lets us
	// put known content in it so a wrong conversion is visible as wrong pixels.
	// IMPLEMENTATION_DEFINED is resolved by the allocator against the usage
	// bits, and the video path never asks for CPU access -- requesting it is
	// what makes that format fail to allocate at all.
	const int gpuOnly = (fmt == 0x7f000001u);
	d.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
	if (!gpuOnly) {
		d.usage |= AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
		           AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
	}

	AHardwareBuffer* buf = NULL;
	if (AHardwareBuffer_allocate(&d, &buf) != 0 || buf == NULL) {
		printf("  allocate FAILED (format unsupported here)\n");
		return -1;
	}

	AHardwareBuffer_Desc got;
	AHardwareBuffer_describe(buf, &got);
	printf("  allocated: %ux%u stride=%u format=0x%x\n",
	       got.width, got.height, got.stride, got.format);

	void* va = NULL;
	if (!gpuOnly &&
	    AHardwareBuffer_lock(buf, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &va) == 0 &&
	    va != NULL) {
		// A luma ramp with a mid-grey chroma block after it. The exact content
		// does not matter; that it is NOT uniform does -- a flat buffer cannot
		// show a conversion that drops or swaps planes.
		unsigned char* p = (unsigned char*)va;
		size_t ySize = (size_t)got.stride * h;
		for (size_t i = 0; i < ySize; i++) {
			p[i] = (unsigned char)(i % 251);
		}
		memset(p + ySize, 0x80, ySize / 2);
		AHardwareBuffer_unlock(buf, NULL);
	} else {
		printf("  lock failed (continuing; content undefined)\n");
	}

	EGLDisplay dpy = eglGetCurrentDisplay();
	EGLClientBuffer cb = eglGetNativeClientBufferANDROID(buf);
	if (cb == NULL) {
		printf("  eglGetNativeClientBufferANDROID FAILED\n");
		AHardwareBuffer_release(buf);
		return -1;
	}
	EGLint attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
	EGLImageKHR img = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attrs);
	if (img == EGL_NO_IMAGE_KHR) {
		printf("  eglCreateImageKHR FAILED (0x%x)\n", eglGetError());
		AHardwareBuffer_release(buf);
		return -1;
	}

	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)img);
	GLenum err = glGetError();
	if (err != GL_NO_ERROR) {
		printf("  glEGLImageTargetTexture2DOES FAILED (0x%x)\n", err);
	} else {
		printf("  bound as GL_TEXTURE_EXTERNAL_OES ok\n");
	}
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	// Draw it. The conversion, if the driver wants one, happens here or at bind.
	static const GLfloat pos[] = { -1,-1, 1,-1, -1,1, 1,1 };
	static const GLfloat uv[]  = {  0, 1, 1, 1,  0,0, 1,0 };
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish();

	// ⚠️ Read inside the PBUFFER (256x256), not at the source buffer's centre:
	// glReadPixels outside the surface returns nothing and looks like a black
	// frame, which reads as a failed conversion when it is a probe bug.
	unsigned char px[16];
	glReadPixels(128, 128, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, px);
	printf("  centre pixels: %02x%02x%02x %02x%02x%02x  (gl err 0x%x)\n",
	       px[0], px[1], px[2], px[4], px[5], px[6], glGetError());

	glDeleteTextures(1, &tex);
	eglDestroyImageKHR(dpy, img);
	AHardwareBuffer_release(buf);
	return 0;
}

int main(void) {
	EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
		fprintf(stderr, "eglInitialize failed\n");
		return 1;
	}
	printf("EGL %s\n", eglQueryString(dpy, EGL_VERSION));

	EGLint cfgAttrs[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	EGLConfig cfg;
	EGLint n = 0;
	if (!eglChooseConfig(dpy, cfgAttrs, &cfg, 1, &n) || n < 1) {
		fprintf(stderr, "eglChooseConfig failed\n");
		return 1;
	}
	EGLint pbAttrs[] = { EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE };
	EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbAttrs);
	EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
	if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(dpy, surf, surf, ctx)) {
		fprintf(stderr, "EGL context setup failed (0x%x)\n", eglGetError());
		return 1;
	}
	printf("GL_RENDERER: %s\n", (const char*)glGetString(GL_RENDERER));
	printf("GL_VERSION : %s\n", (const char*)glGetString(GL_VERSION));

	GLuint vs = compile(GL_VERTEX_SHADER, kVert);
	GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
	if (vs == 0 || fs == 0) {
		return 1;
	}
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glBindAttribLocation(prog, 0, "aPos");
	glBindAttribLocation(prog, 1, "aUV");
	glLinkProgram(prog);
	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetProgramInfoLog(prog, sizeof(log), NULL, log);
		fprintf(stderr, "link failed: %s\n", log);
		return 1;
	}
	glUseProgram(prog);
	glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
	glActiveTexture(GL_TEXTURE0);
	glViewport(0, 0, 256, 256);

	// The formats a decoder actually hands to a SurfaceTexture. YUV_420_888 is
	// the portable one; IMPLEMENTATION_DEFINED is what the MTK decoder used for
	// the layer GrayJay was playing (0x7f000001 in the SurfaceFlinger dump).
	run_case("YUV_420_888", AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420, 1280, 720);
	run_case("IMPLEMENTATION_DEFINED", 0x7f000001u, 1280, 720);
	run_case("RGBA_8888 (control: no conversion expected)",
	         AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 1280, 720);

	printf("\ndone. Check `logcat -s DpFramework:*` for \"setPQParameter stubbed\".\n");
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroySurface(dpy, surf);
	eglDestroyContext(dpy, ctx);
	eglTerminate(dpy);
	return 0;
}
